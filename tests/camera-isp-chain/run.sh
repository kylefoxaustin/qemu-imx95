#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 camera -> ISP chain: raw Bayer in over CSI-2, developed colour out,
# with DRAM as the join.
#
#   host .raw (RGGB mosaic)
#     -> QEMU imx95.isi "frames" property
#     -> ov5640 / MIPI CSI-2 / ISI            what the sensor would deliver
#     -> ISI DMA into a V4L2 capture buffer   the frame is now in DRAM
#     -> VIDIOC_EXPBUF -> dmabuf
#     -> queued as neoisp-input0              the ISP reads THAT buffer
#     -> NeoISP develop -> neoisp-frame       developed colour out
#
# This is the path tests/isp-develop and tests/camera-to-display each prove
# half of. isp-develop feeds the ISP from a FILE, so it never shows a captured
# frame reaching the ISP; camera-to-display captures a frame but deliberately
# keeps the ISP out of the path (a smart camera emitting developed YUYV). The
# clause neither one establishes is the join, and the join is what a real
# vision pipeline is made of.
#
# Two gates, because either alone passes while broken:
#   1. CORRELATION - the developed frame is correlated per channel against the
#      RGB its mosaic was sampled from (tests/isp-develop/checkdev.py, gating
#      on the weakest channel; a wrong Bayer phase swaps red and blue, which
#      luma barely notices).
#   2. PROVENANCE - each developed frame is matched to WHICH staged frame it
#      came from, by hashing the captured Bayer, and must correlate against
#      that frame's own reference. At least two DISTINCT sources must appear,
#      so a pipeline that develops one frame and re-serves it cannot pass.
#      The ISI cycles its host-frame source on its own timer, so the guest
#      cannot choose which frame it gets - hence match-by-hash rather than
#      assuming frame N is file N.
#
# Required (override via env): QEMU, KBUILD (Image + base dtb + dtc +
# fdtoverlay + neoisp.ko + ov5640.ko), SM_ELF, INITRD, a CROSS gcc, python3
# Pillow. SKIPs if any is missing.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=${IMAGE:-$KBUILD/arch/arm64/boot/Image}
BASE_DTB=${BASE_DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
DTC=${DTC:-$KBUILD/scripts/dtc/dtc}
FDTO=${FDTO:-$KBUILD/scripts/dtc/fdtoverlay}
INITRD=${INITRD:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
CROSS=${CROSS:-aarch64-linux-gnu-}
TMO=${TMO:-240}

# Geometry. 1280x720 is not a preference, it is what the sensor allows: raw
# Bayer is 8bpp and the ov5640 driver forces any 8bpp mode below 1280 wide up
# to 720p (ov5640_try_fmt_internal). Requesting 640x480 therefore yields a
# 1280x720 sensor format, every downstream sink disagrees, and STREAMON fails
# with -EPIPE and nothing else.
# Width must also be a multiple of 32: the neoisp driver's format table sets
# .align = 32 on the Bayer formats, so a non-aligned width gets a padded
# bytesperline and the ISI model's bpp=pitch/width misreads the row stride.
W=${W:-1280}; H=${H:-720}
NFRAMES=${NFRAMES:-2}
PHASE=${PHASE:-rggb}
# Two different scenes, so a re-served frame cannot pass gate 2. There is only
# one scene in the tree, so the second frame is mirrored from it unless SRC2
# names another image.
SRC1=${SRC1:-$ROOT/tests/camera-to-display/scene.png}
SRC2=${SRC2:-}

skip() { echo "SKIP: $*"; exit 0; }
[ -x "$QEMU" ]     || skip "no QEMU ($QEMU)"
[ -e "$IMAGE" ]    || skip "no kernel Image ($IMAGE)"
[ -e "$BASE_DTB" ] || skip "no base dtb ($BASE_DTB)"
[ -x "$DTC" ]      || skip "no dtc ($DTC)"
[ -x "$FDTO" ]     || skip "no fdtoverlay ($FDTO)"
[ -e "$SM_ELF" ]   || skip "no SM firmware ($SM_ELF)"
[ -e "$INITRD" ]   || skip "no busybox initramfs ($INITRD)"
NEOISP_KO=$(find "$KBUILD" -name neoisp.ko 2>/dev/null | head -1)
[ -n "$NEOISP_KO" ] || skip "no neoisp.ko under $KBUILD"
OV5640_KO=$(find "$KBUILD" -name ov5640.ko 2>/dev/null | head -1)
[ -n "$OV5640_KO" ] || skip "no ov5640.ko under $KBUILD"
[ -e "$SRC1" ] || skip "no source image ($SRC1)"
command -v "${CROSS}gcc" >/dev/null || skip "no ${CROSS}gcc (set CROSS=)"
python3 -c "import PIL" 2>/dev/null || skip "no python3 Pillow (for mkbayer.py)"
[ $(( W % 32 )) -eq 0 ] || { echo "FAIL: W=$W is not a multiple of 32"; exit 1; }

WORK=$(mktemp -d)
# KEEP_LOG=<path> preserves the serial log past the cleanup trap: the graph and
# link-oracle diagnostics live in it, and they are exactly what is wanted after
# a -EPIPE.
# KEEP_LOG is a PATH, not a flag: "KEEP_LOG=1" would quietly drop a file named
# "1" in the current directory, so require something that looks like a path.
if [ -n "${KEEP_LOG:-}" ] && [ "${KEEP_LOG#*/}" = "${KEEP_LOG}" ]; then
    echo "KEEP_LOG must be a path (e.g. /tmp/chain-serial.log), got '$KEEP_LOG'" >&2
    exit 1
fi
trap 'cp -f "$WORK/serial.log" "${KEEP_LOG:-}" 2>/dev/null; rm -rf "$WORK"' EXIT
LOG="$WORK/serial.log"
OUT_TAG=${OUT_TAG:-chainout}
# A developed 1280x720 RGB32 frame is 3.7 MB, so the frames come out over a 9p
# share rather than base64 on the console: two of them would be ~10 MB of
# byte-at-a-time MMIO through the UART model.
OUT_HOST="${CHAIN_OUT_HOST:-$WORK/out}"; mkdir -p "$OUT_HOST"

# ---- 1. dtb: base + ov5640 camera graph, then enable the ISP ---------------
"$DTC" -@ -I dts -O dtb -o "$WORK/ov5640.dtbo" \
    "$ROOT/tests/camera/ov5640-overlay.dtso" 2>/dev/null \
    || { echo "FAIL: camera overlay would not compile"; exit 1; }
"$FDTO" -i "$BASE_DTB" -o "$WORK/cam.dtb" "$WORK/ov5640.dtbo" \
    || { echo "FAIL: fdtoverlay could not apply the camera overlay"; exit 1; }

# The EVK dtb ships neoisp0 status=disabled; flip it inside isp@4ae00000 only.
# SKIP_ISP_DTB=1 leaves neoisp0 disabled, so this harness reduces to the
# plain camera-capture graph. With SKIP_NEOISP_MOD=1 it then isolates whether
# a failure comes from THIS program or from the ISP sharing the camera power
# domain and clocks with the ISI.
if [ -n "${SKIP_ISP_DTB:-}" ]; then
    echo "SKIP_ISP_DTB: leaving neoisp0 disabled"
    cp "$WORK/cam.dtb" "$WORK/chain.dtb"
else
"$DTC" -I dtb -O dts "$WORK/cam.dtb" > "$WORK/cam.dts" 2>/dev/null
python3 - "$WORK/cam.dts" "$WORK/chain.dts" <<'PY'
import sys
t = open(sys.argv[1]).read()
i = t.index('isp@4ae00000'); b = t.index('{', i)
depth, j = 0, b
while True:
    c = t[j]
    if c == '{': depth += 1
    elif c == '}':
        depth -= 1
        if depth == 0: break
    j += 1
blk = t[b:j].replace('status = "disabled"', 'status = "okay"')
assert 'status = "okay"' in blk, "neoisp0 status not flipped"
open(sys.argv[2], 'w').write(t[:b] + blk + t[j:])
print("neoisp0 enabled on top of the camera graph")
PY
"$DTC" -I dts -O dtb -o "$WORK/chain.dtb" "$WORK/chain.dts" 2>/dev/null \
    || { echo "FAIL: dtc could not rebuild the patched dtb"; exit 1; }
fi
[ -n "${KEEP_DTB:-}" ] && cp "$WORK/chain.dtb" "$KEEP_DTB"

# ---- 2. stage the raw Bayer frames the "sensor" will deliver ---------------
# A directory of *.raw: the ISI model serves them in sorted order, one per
# frame tick, looping. Each gets its .rgb24 companion - the RGB the mosaic was
# sampled from - which is the ground truth the developed frame is scored on.
FRAMES="$WORK/frames"; mkdir -p "$FRAMES"
python3 "$HERE/../isp-develop/mkbayer.py" "$SRC1" "$W" "$H" \
    "$FRAMES/00.raw" "$PHASE" || exit 1
if [ "$NFRAMES" -gt 1 ]; then
    if [ -n "$SRC2" ] && [ -e "$SRC2" ]; then
        SRC_B="$SRC2"
    else
        # Mirror the first scene. Gate 2 needs two DISTINCT sources, and a
        # mirrored photograph stays a real image - a synthetic pattern would
        # weaken the correlation gate it also has to pass.
        python3 - "$SRC1" "$W" "$H" "$WORK/scene2.png" <<'PY'
import sys
from PIL import Image, ImageOps
src, W, H, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
im = Image.open(src).convert("RGB").resize((W, H), Image.LANCZOS)
ImageOps.mirror(im).save(out)
PY
        SRC_B="$WORK/scene2.png"
    fi
    python3 "$HERE/../isp-develop/mkbayer.py" "$SRC_B" "$W" "$H" \
        "$FRAMES/01.raw" "$PHASE" || exit 1
    cmp -s "$FRAMES/00.raw" "$FRAMES/01.raw" \
        && { echo "FAIL: the two staged mosaics are identical, so gate 2" \
                  "could not detect a re-served frame"; exit 1; }
fi
echo "staged $(ls "$FRAMES"/*.raw | wc -l) Bayer frame(s) in $FRAMES"
for f in "$FRAMES"/*.raw; do
    echo "  $(basename "$f")  $(stat -c%s "$f") bytes"
done

# ---- 3. the guest program -------------------------------------------------
"${CROSS}gcc" -O2 -Wall -static -o "$WORK/cam_isp_chain" \
    "$HERE/cam_isp_chain.c" \
    || { echo "FAIL: could not build cam_isp_chain.c"; exit 1; }
# The link_validate oracle from the capture test, staged as a diagnostic: when
# STREAMON returns -EPIPE the kernel logs the offending link only at dev_dbg,
# which the BSP does not enable, so dump the graph and the per-link formats
# right after the attempt.
"${CROSS}gcc" -O2 -Wall -static -o "$WORK/v4l2_cap" \
    "$ROOT/tests/camera/v4l2_cap.c" \
    || { echo "FAIL: could not build v4l2_cap.c"; exit 1; }

# Control knobs forwarded into the guest. CHAIN_CODE / CHAIN_FOURCC /
# CHAIN_CAPTURE_ONLY run the SAME program on a format already known to stream,
# which is how you tell "this program is wrong" from "this format does not
# stream" after a -EPIPE.
CHAIN_ENV=""
for v in CHAIN_CODE CHAIN_FOURCC CHAIN_CAPTURE_ONLY CHAIN_NO_SENSOR_FMT CHAIN_ALL_SUBDEVS \
         CHAIN_SINKS_ONLY CHAIN_NO_CAP_SFMT CHAIN_NO_EXPBUF; do
    eval "val=\${$v:-}"
    [ -n "$val" ] && CHAIN_ENV="$CHAIN_ENV$v=$val "
done
[ -n "$CHAIN_ENV" ] && echo "forwarding to guest: $CHAIN_ENV"

# Dynamic debug. Every report on the failing path - mc-entity's per-pad and
# per-link validation, v4l2-subdev's default validator (which is the thing
# that actually returns -EPIPE, and which names width/height/code/field), the
# ISI pipe's own routing - is a dev_dbg. With CONFIG_DYNAMIC_DEBUG off they
# are compiled out and -EPIPE arrives with no explanation at all. Turn the
# relevant sites on in the guest, just before the attempt, and raise the
# console loglevel so KERN_DEBUG reaches the serial log.
#
# Module names here are KBUILD_MODNAMEs, not driver name strings: the ISI is
# built in as imx8-isi -> "imx8_isi", NOT the "mxc-isi" that appears in its
# dev_* prefix. A query that matches nothing is silently accepted, so the
# count of enabled sites is printed as proof the queries landed.
DYNDBG="${DYNDBG:-1}"
DYNDBG_SH=":"
LOGLEVEL_ARG=""
if [ "$DYNDBG" != "0" ]; then
    LOGLEVEL_ARG="loglevel=8"
    DYNDBG_SH=$(cat <<'DDSH'
/bin/busybox mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
DD=/sys/kernel/debug/dynamic_debug/control
if [ -f $DD ]; then
    for q in mc videodev imx8_isi imx_csi_formatter dwc_mipi_csi2 ov5640 neoisp; do
        echo "module $q +p" > $DD 2>/dev/null || echo "DYNDBG-REJECTED $q"
    done
    echo "DYNDBG sites enabled: $(/bin/busybox grep -c '=p' $DD)"
else
    echo "DYNDBG-UNAVAILABLE: no dynamic_debug/control (CONFIG_DYNAMIC_DEBUG off)"
fi
DDSH
)
fi

STAGE="$WORK/root"; mkdir -p "$STAGE/mods"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
cp "$NEOISP_KO" "$OV5640_KO" "$STAGE/mods/"
cp "$WORK/cam_isp_chain" "$STAGE/cam_isp_chain"
cp "$WORK/v4l2_cap" "$STAGE/v4l2_cap"
cat > "$STAGE/init" <<INIT
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "=== CHAIN ==="
sleep 3
insmod /mods/ov5640.ko 2>&1 | sed 's/^/insmod: /'
${SKIP_NEOISP_MOD:+: }insmod /mods/neoisp.ko 2>&1 | sed 's/^/insmod: /'
sleep 2
$DYNDBG_SH
/bin/busybox mkdir -p /mnt/out
/bin/busybox mount -t 9p -o trans=virtio,version=9p2000.L $OUT_TAG /mnt/out \
    || echo "MOUNT-OUT-FAIL"
${USE_V4L2CAP:+: }${CHAIN_ENV}/cam_isp_chain $NFRAMES $W $H /mnt/out/developed
${USE_V4L2CAP:+/v4l2_cap cap /dev/video0}
/bin/busybox sync
echo "--- media graph (diagnostic) ---"
/v4l2_cap topo
echo "--- link_validate oracle (diagnostic) ---"
/v4l2_cap links
echo "--- kernel view ---"
/bin/busybox dmesg 2>/dev/null | /bin/busybox grep -i "neoisp\\|isi\\|ov5640\\|csi" | /bin/busybox tail -25
echo "--- developed frames on the share ---"
/bin/busybox ls -l /mnt/out 2>/dev/null
/bin/busybox umount /mnt/out 2>/dev/null
echo "=== CHAIN-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.gz"

# ---- 4. run ---------------------------------------------------------------
# SKIP_FRAMES / SKIP_9P drop the two QEMU arguments this harness adds over
# tests/camera/run.sh, so a failure can be attributed to one of them.
# Note: ${VAR:-default} yields VAR's VALUE when VAR is set, so writing the
# argument inline as ${SKIP_FRAMES:-"-global"} passes the literal "1" to QEMU
# and the run dies before booting. Build the argument lists instead.
FRAMES_ARGS=""
if [ -z "${SKIP_FRAMES:-}" ]; then
    FRAMES_ARGS="-global driver=imx95.isi,property=frames,value=$FRAMES"
fi
NINEP_ARGS=""
if [ -z "${SKIP_9P:-}" ]; then
    NINEP_ARGS="-fsdev local,id=chainout,path=$OUT_HOST,security_model=none"
    NINEP_ARGS="$NINEP_ARGS -device virtio-9p-device,fsdev=chainout,mount_tag=$OUT_TAG"
fi

timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$WORK/chain.dtb" -initrd "$WORK/initrd.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init $LOGLEVEL_ARG" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -device ov5640,bus=lpi2c1,address=0x3c \
  $FRAMES_ARGS $NINEP_ARGS \
  -serial file:"$LOG" -serial null >/dev/null 2>"$WORK/qemu.err" || true

echo "--- camera-isp-chain report ---"
sed -n '/=== CHAIN ===/,/=== CHAIN-DONE ===/p' "$LOG" \
    | grep -aE 'CHAIN|insmod|neoisp|isi|ov5640|error|Error|TOPO|LINK|MISMATCH' \
    | head -80
[ -s "$WORK/qemu.err" ] && { echo "--- qemu stderr ---"; head -5 "$WORK/qemu.err"; }

# The link-validation verdict, in the kernel's own words. These lines only
# exist when DYNDBG is on; they name the pad being validated, every link that
# validated, the one that did not, and the field that disagreed.
if [ "$DYNDBG" != "0" ]; then
    echo "--- kernel link validation (dynamic debug) ---"
    grep -aE "DYNDBG|Validating pad|failed validation|is valid|does not match|must be connected|no route|not a (sink|source) pad|Failed to (enable|start)|link was" "$LOG" \
        | tail -60
fi

fail() { echo "FAIL: $*"; exit 1; }

# The ISI model falls back to its synthetic gradient if the host frame is the
# wrong size, and says so. That would still "capture" and still "develop" -
# just not our image - so treat it as a hard failure, not a warning.
grep -qa "FALLING BACK TO THE SYNTHETIC GRADIENT" "$WORK/qemu.err" \
    && fail "the ISI rejected the staged frames and drew its own gradient"

# netfs cannot pin the pages behind a V4L2 dma-contig mmap, so a write()
# straight out of a capture buffer to the 9p share returns -EFAULT and is
# RETRIED forever: the write never completes, never errors, and the only
# symptom is thousands of identical lines a second. Name it rather than
# letting the next person watch a 29 MB serial log and call it a hang.
grep -qa "Couldn't get user pages" "$LOG" \
    && fail "9p write from an unpinnable V4L2 mapping (netfs -EFAULT retry loop); stage the frame on the heap first"

# Without the sensor there is no graph at all, and the first symptom is an I2C
# timeout (-110) rather than anything about cameras. Name it.
grep -qa "ov5640.*failed to power on" "$LOG" \
    && fail "the ov5640 never answered I2C - is -device ov5640,bus=lpi2c1,address=0x3c on the command line?"

grep -qa "MOUNT-OUT-FAIL" "$LOG" \
    && fail "the 9p output share did not mount, so no frame could be recovered"

grep -qa "CHAIN PASS" "$LOG" || fail "the chain did not complete all frames"

# ---- 5. gate 1+2: decode each frame, identify its source, correlate -------
python3 - "$LOG" "$FRAMES" "$W" "$H" "$OUT_HOST" "$HERE" <<'GATE' || exit 1
import os, re, subprocess, sys

log, frames_dir, W, H, out_host, here = sys.argv[1:7]
W, H = int(W), int(H)
text = open(log, "rb").read().decode("utf-8", "replace").replace("\r", "")

def fnv1a(b):
    h = 0xcbf29ce484222325
    for c in b:
        h = ((h ^ c) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h

# Hash every staged mosaic exactly as the guest hashes the captured bytes, so
# a developed frame can be attributed to the source it actually came from. The
# ISI cycles its frame source on its own timer; the guest cannot choose which
# one it receives, so frame N is not file N.
staged = {}
for name in sorted(os.listdir(frames_dir)):
    if not name.endswith(".raw"):
        continue
    path = os.path.join(frames_dir, name)
    with open(path, "rb") as f:
        staged[fnv1a(f.read(W * H))] = path
print(f"staged sources: {len(staged)}")

caps = {int(i): int(h, 16)
        for i, h in re.findall(r"CHAIN frame=(\d+) cap-index=\d+ "
                               r"cap-bytes=\d+ cap-hash=([0-9a-f]{16})", text)}
if not caps:
    sys.exit("FAIL: no capture hashes in the log")

rc, matched = 0, {}
for idx in sorted(caps):
    dev = os.path.join(out_host, f"developed-{idx}.raw")
    if not os.path.exists(dev) or os.path.getsize(dev) == 0:
        print(f"FAIL: frame {idx}: nothing at {dev}")
        rc = 1
        continue

    src = staged.get(caps[idx])
    if src is None:
        print(f"FAIL: frame {idx}: captured hash {caps[idx]:016x} matches NO "
              f"staged mosaic - the bytes changed between host and ISP")
        rc = 1
        continue
    matched[idx] = src
    print(f"frame {idx}: captured mosaic == {os.path.basename(src)} "
          f"({os.path.getsize(dev):,} bytes developed)")
    r = subprocess.run([sys.executable,
                        os.path.join(here, "..", "isp-develop", "checkdev.py"),
                        dev, src + ".rgb24", str(W), str(H)])
    if r.returncode != 0:
        print(f"FAIL: frame {idx} does not reconstruct {os.path.basename(src)}")
        rc = 1

distinct = len(set(matched.values()))
print(f"distinct sources developed: {distinct}")
if len(staged) > 1 and distinct < 2:
    print("FAIL: only one distinct source was developed - a pipeline that "
          "re-serves one frame would pass this run, so the per-frame claim "
          "is not established")
    rc = 1
sys.exit(rc)
GATE
echo "PASS: raw Bayer travelled CSI-2 -> ISI -> DRAM -> NeoISP and the" \
     "developed frames reconstruct their own source scenes"
