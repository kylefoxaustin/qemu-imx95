#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 NeoISP bring-up - the camera image signal processor registers.
#
# The NeoISP (isp@4ae00000, "nxp,imx95-b0-neoisp") is the i.MX95's camera ISP: a
# register-driven V4L2 mem2mem device (debayer / tone / colour pipeline) with two
# MMIO windows - "registers" @0x4ae00000 and "stats" @0x4afe0000 - that the
# neoisp driver programs directly (no firmware/remoteproc; the separate
# nxp,imx95-isp-rproc node is a different, optional accelerator path).
#
# The EVK dtb ships neoisp0 status=disabled, so this enables it in a patched dtb
# and loads the neoisp module to confirm the whole driver binds end to end: it
# ioremaps both windows, builds its regmap, runs the soft-reset handshake, takes
# the camera power-domain + cameramix clocks, requests IRQ 222, and registers its
# eight node-groups of V4L2 nodes (neoisp-input0/input1/params/frame/ir/stats per
# group) + the per-group media devices. No external abort.
#
# This is the "brings up" milestone (the ISP registers + all its /dev/video nodes
# appear); the proprietary per-pixel ISP pipeline compute is not modelled, the
# same fidelity ceiling as the JPEG/Neutron blocks. The two MMIO windows are
# already backed by the machine, so this needs no model change - only the dtb
# enable + the module.
#
# Required (override via env): QEMU, KBUILD (Image + base dtb + dtc + neoisp.ko),
# SM_ELF. SKIPs if any is missing.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=${IMAGE:-$KBUILD/arch/arm64/boot/Image}
BASE_DTB=${BASE_DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
DTC=${DTC:-$KBUILD/scripts/dtc/dtc}
INITRD=${INITRD:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
TMO=${TMO:-260}
# holobench supplies these; nothing here hardcodes them, because the mount tags
# and the device type are per-board and a profile may rename them.
IN_TAG=${IN_TAG:-hbbatchin}
OUT_TAG=${OUT_TAG:-hbbatchout}
NFRAMES=${NFRAMES:-6}
BLIT_EVERY=${BLIT_EVERY:-3}
SRC_IMG=${SRC_IMG:-$ROOT/tests/camera-to-display/scene.png}
W=${W:-640}; H=${H:-480}
PHASE=${PHASE:-rggb}
CROSS=${CROSS:-aarch64-linux-gnu-}

skip() { echo "SKIP: $*"; exit 0; }
[ -x "$QEMU" ]     || skip "no QEMU ($QEMU)"
[ -e "$IMAGE" ]    || skip "no kernel Image ($IMAGE)"
[ -e "$BASE_DTB" ] || skip "no base dtb ($BASE_DTB)"
[ -x "$DTC" ]      || skip "no dtc ($DTC)"
[ -e "$SM_ELF" ]   || skip "no SM firmware ($SM_ELF)"
[ -e "$INITRD" ]   || skip "no busybox initramfs ($INITRD)"
KO=$(find "$KBUILD" -name neoisp.ko 2>/dev/null | head -1)
[ -n "$KO" ] || skip "no neoisp.ko under $KBUILD"
[ -e "$SRC_IMG" ] || skip "no source image ($SRC_IMG)"
command -v "${CROSS}gcc" >/dev/null || skip "no ${CROSS}gcc"
python3 -c "import PIL" 2>/dev/null || skip "no python3 Pillow (for mkbayer.py)"

WORK=$(mktemp -d)
# PRESERVE THE SERIAL LOG ON THE WAY OUT. The filtered report below shows only
# the lines this test knows to look for, so a failure mode nobody anticipated
# was being deleted by this trap along with the workdir - the run said "did not
# report completion" and destroyed the only record of WHY. Evidence outlives
# the run, especially a failing one.
trap 'cp -f "$WORK/serial.log" "${KEEP_LOG:-/tmp/isp-batch-serial.log}" 2>/dev/null; rm -rf "$WORK"' EXIT
LOG="$WORK/serial.log"

# Enable neoisp0 (status disabled -> okay) inside the isp@4ae00000 block.
"$DTC" -I dtb -O dts "$BASE_DTB" > "$WORK/base.dts" 2>/dev/null
python3 - "$WORK/base.dts" "$WORK/neoisp.dts" <<'PY'
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
print("neoisp0 enabled")
PY
"$DTC" -I dts -O dtb -o "$WORK/neoisp.dtb" "$WORK/neoisp.dts" 2>/dev/null \
    || { echo "FAIL: dtc could not rebuild the patched dtb"; exit 1; }



# --- the host folders the engineer would supply --------------------------------
# BATCH_IN_HOST / BATCH_OUT_HOST let a caller point the run at DURABLE folders
# instead of the harness's mktemp dir. The receipt records whatever paths were
# actually used, so with a temp workdir its provenance fields name a directory
# that is deleted before anyone can follow them - correct, well-formed, and
# pointing at nothing. Use these to produce a receipt whose paths still resolve.
IN_HOST="${BATCH_IN_HOST:-$WORK/in}"; OUT_HOST="${BATCH_OUT_HOST:-$WORK/out}"
mkdir -p "$IN_HOST" "$OUT_HOST"
# EVERY STAGED FRAME MUST BE DISTINCT.
#
# Six byte-identical frames make this test unable to fail the way it most
# plausibly would: a runner that develops frame 1 and writes it six times, or
# one that ignores its input and emits a constant, produces a perfect receipt
# with six OK lines. A batch tool's whole claim is PER-FRAME processing, so a
# fixture that cannot tell those apart is not evidence of it. Each frame gets a
# patch of raw sensor values keyed to its index; the receipt's in_hash and
# out_hash columns are then asserted all-distinct below.
for i in $(seq 1 "$NFRAMES"); do
    f="$IN_HOST/capture_$(printf '%03d' "$i").raw"
    python3 "$ROOT/tests/isp-develop/mkbayer.py" "$SRC_IMG" "$W" "$H" \
        "$f" rggb >/dev/null || exit 1
    python3 - "$f" "$i" "$W" <<'STAMP' || exit 1
import sys
path, idx, w = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
b = bytearray(open(path, 'rb').read())
val = (30 + idx * 27) & 0xff
for row in range(16, 32):                 # a 16x16 patch, same place each frame
    base = row * w + 16
    b[base:base + 16] = bytes([val]) * 16
open(path, 'wb').write(bytes(b))
STAMP
done
rm -f "$IN_HOST"/*.rgb24
echo "staged $(ls "$IN_HOST" | wc -l) raw frames in $IN_HOST"

"${CROSS}gcc" -O2 -Wall -static -o "$WORK/neoisp_batch" "$HERE/neoisp_batch.c" \
    || { echo "FAIL: could not build neoisp_batch.c"; exit 1; }

STAGE="$WORK/root"; mkdir -p "$STAGE/mods"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
cp "$KO" "$STAGE/mods/"; cp "$WORK/neoisp_batch" "$STAGE/neoisp_batch"
cat > "$STAGE/init" <<INIT
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "=== BATCH ==="
sleep 3
insmod /mods/neoisp.ko 2>&1 | sed 's/^/insmod: /'
sleep 2
mkdir -p /mnt/in /mnt/out
mount -t 9p -o trans=virtio,version=9p2000.L,ro $IN_TAG /mnt/in  || echo "MOUNT-IN-FAIL"
mount -t 9p -o trans=virtio,version=9p2000.L    $OUT_TAG /mnt/out || echo "MOUNT-OUT-FAIL"
echo "--- what does the guest actually see on the mount? ---"
ls -la /mnt/in 2>&1 | head -5
echo "byte count of the first file:"
wc -c /mnt/in/capture_001.raw 2>&1
echo "first bytes via dd:"
dd if=/mnt/in/capture_001.raw bs=1 count=8 2>/dev/null | od -An -tx1 | head -1
echo "--- input is read-only? (must fail) ---"
touch /mnt/in/should_not_exist 2>&1 | sed 's/^/rotest: /'
/neoisp_batch --in /mnt/in --out /mnt/out --mode raw --width $W --height $H \\
    --blit-every $BLIT_EVERY --in-tag $IN_TAG --out-tag $OUT_TAG \\
    --in-host '$IN_HOST' --out-host '$OUT_HOST'
sync
echo "=== BATCH-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.gz"

timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$WORK/neoisp.dtb" -initrd "$WORK/initrd.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -fsdev local,id=hbbin,path="$IN_HOST",security_model=none,readonly=on \
  -device virtio-9p-device,fsdev=hbbin,mount_tag="$IN_TAG" \
  -fsdev local,id=hbbout,path="$OUT_HOST",security_model=none \
  -device virtio-9p-device,fsdev=hbbout,mount_tag="$OUT_TAG" \
  -serial file:"$LOG" -serial null >/dev/null 2>"$WORK/qemu.err" || true

echo "--- batch report ---"
sed -n '/=== BATCH ===/,/=== BATCH-DONE ===/p' "$LOG" | grep -aE 'BATCH|MOUNT|rotest' | head -12

fail() { echo "FAIL: $*"; exit 1; }
grep -qa "MOUNT-IN-FAIL\|MOUNT-OUT-FAIL" "$LOG" && fail "a 9p mount did not come up"
grep -qa "BATCH COMPLETE" "$LOG" || fail "the batch did not report completion"

# The frames were staged distinct; if the receipt does not show them developing
# to distinct results, the runner is not processing them individually.
# (the hashes live in the RECEIPT, not the serial log - the OK lines are written
# to the output mount. Grepping $LOG here made the check able to report only
# zero, which is a check that cannot pass rather than one that cannot fail.)
[ -f "$OUT_HOST/receipt.txt" ] || fail "no receipt written"
nin=$(grep -oa 'in_hash=[0-9a-f]*' "$OUT_HOST/receipt.txt" | sort -u | wc -l)
nout=$(grep -oa 'out_hash=[0-9a-f]*' "$OUT_HOST/receipt.txt" | sort -u | wc -l)
[ "$nin" -eq "$NFRAMES" ] || fail "only $nin distinct input hashes for $NFRAMES staged frames - the runner is not reading each frame"
[ "$nout" -eq "$NFRAMES" ] || fail "only $nout distinct output hashes for $NFRAMES frames - the runner is not developing each frame"
# the input mount must reject writes AT THE FSDEV, not by our good manners
grep -qa "rotest:.*[Rr]ead-only" "$LOG" || fail "input mount was NOT read-only"

echo "--- receipt ---"
[ -f "$OUT_HOST/receipt.txt" ] || fail "no receipt written"
cat "$OUT_HOST/receipt.txt" | head -20
tail -1 "$OUT_HOST/receipt.txt" | grep -q "^COMPLETE$" \
    || fail "receipt has no COMPLETE marker as its last line"
produced=$(ls "$OUT_HOST" | grep -c '\.bgr$' || true)
[ "$produced" -eq "$NFRAMES" ] \
    || fail "produced $produced developed frames, expected $NFRAMES"
# output names must derive from input names
for f in "$IN_HOST"/*.raw; do
    b=$(basename "$f")
    [ -f "$OUT_HOST/$b.bgr" ] || fail "no output derived from input name $b"
done
# -T so a second run REPLACES the kept output instead of nesting a new copy
# inside it. Without it the newest receipt lands at $KEEP_OUT/out/receipt.txt
# while a STALE one from an earlier run still sits at $KEEP_OUT/receipt.txt -
# and the stale one is what you read. Nearly sent the wrong receipt that way.
[ -n "${KEEP_OUT:-}" ] && { rm -rf "$KEEP_OUT"; cp -rT "$OUT_HOST" "$KEEP_OUT"; }

echo "PASS: batch developed $produced frames, receipt complete, names derived, input read-only"
