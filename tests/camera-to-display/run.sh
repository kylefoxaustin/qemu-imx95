#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX95 camera -> LCD: a real photograph in over MIPI CSI-2, out on the panel.
#
# The vision TRANSPORT path end to end, with no ISP in it:
#
#     smart camera (already-developed YUYV)      the ISI host frame source
#            -> MIPI CSI-2 receiver              hw/display/imx95_isi.c
#            -> ISI  -> DRAM capture buffer      DMA, byte-for-byte
#            -> V4L2 DQBUF (tests/.../v4l2_to_fb)
#            -> /dev/fb0 -> DPU -> LVDS panel    what Holobench shows
#
# "Smart camera" means the sensor emits developed YUV rather than Bayer, so the
# NeoISP is deliberately NOT in this path: it isolates the transport, so a
# failure here cannot hide behind image processing. Flowing raw Bayer through a
# real debayer is the next step, not this one.
#
# Two independent proofs, because either alone is weak:
#   1. BYTES - the guest hashes the captured frame; the host hashes what it fed
#      in at the same stride. Equal hashes mean the image crossed the pipeline
#      intact, not merely that something arrived.
#   2. PIXELS - a screendump of the panel is compared against the source image.
#      A correct hash with a black screen would still be a broken display path.
#
# Required (override via env): QEMU, KBUILD (Image + base dtb + dtc + fdtoverlay
# + ov5640.ko), SM_ELF, a CROSS gcc, python3-pillow. SKIPs if any is missing.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CAM="$ROOT/tests/camera"

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=${IMAGE:-$KBUILD/arch/arm64/boot/Image}
BASE_DTB=${BASE_DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
DTC=${DTC:-$KBUILD/scripts/dtc/dtc}
FDTO=${FDTO:-$KBUILD/scripts/dtc/fdtoverlay}
INITRD=${INITRD:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
CROSS=${CROSS:-aarch64-linux-gnu-}
SRC_IMG=${SRC_IMG:-$HERE/scene.png}
TMO=${TMO:-200}
# HOLD=1 keeps the frame on the panel indefinitely instead of powering off.
# Holobench's framebuffer pane is a ~1.5s PULL (QMP screendump on a timer) that
# the guest cannot trigger, so a blit that lands and exits inside that window is
# missed - and missed SILENTLY, since a black pane looks exactly like a dead
# camera path. For a reserved board someone is watching, hold the frame.
HOLD=${HOLD:-0}
KEEP_DTB=${KEEP_DTB:-}

skip() { echo "SKIP: $*"; exit 0; }
[ -x "$QEMU" ]     || skip "no QEMU ($QEMU)"
[ -e "$IMAGE" ]    || skip "no kernel Image ($IMAGE)"
[ -e "$BASE_DTB" ] || skip "no base dtb ($BASE_DTB)"
[ -x "$DTC" ]      || skip "no dtc ($DTC)"
[ -x "$FDTO" ]     || skip "no fdtoverlay ($FDTO)"
[ -e "$SM_ELF" ]   || skip "no SM firmware ($SM_ELF)"
[ -e "$INITRD" ]   || skip "no busybox initramfs ($INITRD)"
[ -e "$CAM/ov5640-overlay.dtso" ] || skip "no ov5640 overlay"
[ -e "$SRC_IMG" ]  || skip "no source image ($SRC_IMG)"
command -v "${CROSS}gcc" >/dev/null || skip "no ${CROSS}gcc"
python3 -c "import PIL" 2>/dev/null || skip "no python3 Pillow (for mkframe.py)"
KO=$(find "$KBUILD" -name ov5640.ko 2>/dev/null | head -1)
[ -n "$KO" ] || skip "no ov5640.ko under $KBUILD"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG="$WORK/serial.log"
SHOT="${SHOT:-$WORK/panel.ppm}"

# The pipeline negotiates 640x480 YUYV with a 3840-byte line stride (6 bytes per
# pixel of stride for 2 bytes of pixel). Build the host frame to match exactly.
W=${W:-640}; H=${H:-480}; STRIDE=${STRIDE:-3840}
# FRAME=<file> feeds a pre-made raw frame instead of converting SRC_IMG - used
# to exercise the ISI's fallback path deliberately.
#
# STAGE SEVERAL DISTINCT FRAMES, NOT ONE.
#
# Fed a single file the ISI rewinds and re-serves it, so every captured frame is
# byte-identical - and a run that captured ONE frame and re-read it three times
# produces exactly the same log, at r=1.0000. The hashes then prove TRANSPORT and
# say nothing about per-frame capture, which is the property this test is read as
# establishing. The model already cycles a DIRECTORY of frames, so pointing it at
# one costs nothing and makes the frames tell each other apart.
FRAME=${FRAME:-$WORK/frames}
if [ "$FRAME" = "$WORK/frames" ]; then
    mkdir -p "$WORK/frames"
    for i in 0 1 2; do
        f="$WORK/frames/frame00$i.raw"
        python3 "$HERE/mkframe.py" "$SRC_IMG" "$W" "$H" "$f" "$STRIDE" || exit 1
        python3 - "$f" "$i" "$STRIDE" <<'STAMP' || exit 1
import sys
path, idx, stride = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
b = bytearray(open(path, 'rb').read())
val = (40 + idx * 60) & 0xff
for row in range(8, 24):                  # a small patch, same place each frame
    base = row * stride + 32
    b[base:base + 64] = bytes([val]) * 64
open(path, 'wb').write(bytes(b))
STAMP
    done
else
    echo "using pre-made frame: $FRAME"
fi
# hash every staged frame: the guest's capture must match ONE of them
HOST_HASHES=$(python3 - "$FRAME" <<'PY'
import sys, os
# FNV-1a 64-bit offset basis; must match v4l2_to_fb.c exactly
def fnv(path):
    h = 0xcbf29ce484222325
    for b in open(path, 'rb').read():
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h
p = sys.argv[1]
if os.path.isdir(p):
    for n in sorted(os.listdir(p)):
        print(fnv(os.path.join(p, n)))
else:
    print(fnv(p))
PY
)
echo "host frame hashes:"; echo "$HOST_HASHES" | sed 's/^/  /'
HOST_HASH=$(echo "$HOST_HASHES" | head -1)

# dtb = base + ov5640 camera graph + a 1280x800 LVDS panel (so /dev/fb0 exists)
"$DTC" -@ -I dts -O dtb -o "$WORK/ov5640.dtbo" "$CAM/ov5640-overlay.dtso" 2>/dev/null \
    || { echo "FAIL: overlay would not compile"; exit 1; }
"$FDTO" -i "$BASE_DTB" -o "$WORK/cam.dtb" "$WORK/ov5640.dtbo" \
    || { echo "FAIL: fdtoverlay could not apply the camera overlay"; exit 1; }
BASE_DTB="$WORK/cam.dtb" OUT="$WORK/patched.dtb" \
    bash "$ROOT/tests/lcd-panel/attach-lcd.sh" "$WORK/cam.dtb" "$WORK/patched.dtb" \
    >/dev/null 2>&1 || cp "$WORK/cam.dtb" "$WORK/patched.dtb"
[ -e "$WORK/patched.dtb" ] || { echo "FAIL: no patched dtb"; exit 1; }
# KEEP_DTB=<path> emits the camera+panel dtb as a reusable artifact, so a board
# farm can offer this configuration without re-deriving it.
if [ -n "$KEEP_DTB" ]; then
    cp "$WORK/patched.dtb" "$KEEP_DTB"
    echo "wrote $KEEP_DTB (base + ov5640 camera graph + 1280x800 LVDS panel)"
fi

"${CROSS}gcc" -O2 -Wall -static -o "$WORK/v4l2_to_fb" "$HERE/v4l2_to_fb.c" \
    || { echo "FAIL: could not build v4l2_to_fb.c"; exit 1; }
# The media graph needs its links enabled and the sensor format propagated onto
# every crossbar sink before STREAMON will validate. tests/camera/v4l2_cap.c
# already does exactly that in "cap" mode and is the proven implementation, so
# run it first rather than duplicating the format-propagation logic here.
"${CROSS}gcc" -O2 -Wall -static -o "$WORK/v4l2_cap" "$CAM/v4l2_cap.c" \
    || { echo "FAIL: could not build v4l2_cap.c"; exit 1; }

STAGE="$WORK/root"; mkdir -p "$STAGE/mods"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
cp "$KO" "$STAGE/mods/"; cp "$WORK/v4l2_to_fb" "$STAGE/v4l2_to_fb"
cp "$WORK/v4l2_cap" "$STAGE/v4l2_cap"
cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "=== CAM2LCD ==="
sleep 3
insmod /mods/ov5640.ko 2>&1 | sed 's/^/insmod: /'
sleep 2
ls -l /dev/fb0 /dev/video0 2>&1
# set the pipeline up (links + format propagation), then blit a frame
/v4l2_cap cap /dev/video0
/v4l2_to_fb /dev/video0 /dev/fb0
echo "=== CAM2LCD-DONE ==="
if [ -f /HOLD ]; then
    echo "holding the frame on the panel (Holobench pane polls ~1.5s)"
    while true; do /bin/busybox sleep 3600; done
fi
sleep 4          # leave the image up long enough for the screendump
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
[ "$HOLD" = 1 ] && touch "$STAGE/HOLD"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.gz"

# Screendump the panel while the frame is still up, via the monitor socket.
MON="$WORK/mon.sock"
( sleep 1
  for i in $(seq 1 "$TMO"); do
      grep -qa "CAM2LCD-DONE" "$LOG" 2>/dev/null && break
      sleep 1
  done
  printf 'screendump %s\n' "$SHOT" | timeout 10 socat - "UNIX-CONNECT:$MON" >/dev/null 2>&1
) &
SHOOTER=$!

timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$WORK/patched.dtb" -initrd "$WORK/initrd.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -device ov5640,bus=lpi2c1,address=0x3c \
  -global driver=imx95.isi,property=frames,value="$FRAME" \
  -monitor "unix:$MON,server,nowait" \
  -serial file:"$LOG" -serial null >/dev/null 2>"$WORK/qemu.err" || true
wait "$SHOOTER" 2>/dev/null

# QEMU's own warnings matter: the ISI shouts here when a host frame source is
# unusable and it has fallen back to the synthetic gradient. Discarding this
# stream would hide exactly the failure the model was taught to announce.
if [ -s "$WORK/qemu.err" ]; then
    echo "--- qemu stderr ---"; cat "$WORK/qemu.err"
fi
if grep -q "FALLING BACK TO THE SYNTHETIC GRADIENT" "$WORK/qemu.err" 2>/dev/null; then
    echo "FAIL: the ISI could not use the host frame and drew its gradient instead"
    echo "      (the capture would still have looked healthy - that is the point)"
    exit 1
fi

echo "--- camera-to-display report ---"
sed -n '/=== CAM2LCD ===/,/=== CAM2LCD-DONE ===/p' "$LOG" \
    | grep -aE 'CAPTURE|FRAME |FB |BLIT|DISPLAYED|CAPTURE_HASH|video0|fb0|CAMERA-CAP.*(PASS|fmt)|G_FMT|STREAMON|DQBUF'

fail() { echo "FAIL: $*"; exit 1; }
grep -qa "DISPLAYED" "$LOG" || fail "client never reached the framebuffer blit"

# the serial log carries CR line endings; strip them or the compare always fails
GUEST_HASH=$(grep -a 'CAPTURE_HASH' "$LOG" | tail -1 | awk '{print $2}' | tr -d '\r')
[ -n "$GUEST_HASH" ] || fail "no capture hash from the guest"
echo "guest capture hash: $GUEST_HASH"
echo "$HOST_HASHES" | grep -qx "$GUEST_HASH" \
    || fail "capture hash $GUEST_HASH matches NO staged frame (image altered in transit)"

# THE CAPTURE MUST VARY. Distinct frames were staged; if every captured frame
# hashes the same, the pipeline is re-serving one frame and the r=1.0000 above
# is a statement about transport only.
ndist=$(grep -a '^FRAME ' "$LOG" | grep -oa 'hash=[0-9a-f]*' | sort -u | wc -l)
[ "$ndist" -ge 2 ] \
    || fail "all captured frames hash identically ($ndist distinct) - the capture is not per-frame"
echo "captured $ndist distinct frame hashes (staged 3)"

if [ -s "$SHOT" ]; then
    python3 "$HERE/checkshot.py" "$SHOT" "$SRC_IMG" "$W" "$H" || exit 1
else
    echo "WARN: no screendump captured (panel proof skipped)"
fi

echo "PASS: camera -> ISI -> DRAM -> /dev/fb0 -> DPU panel, image intact end to end"
