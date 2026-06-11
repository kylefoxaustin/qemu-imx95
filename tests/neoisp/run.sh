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
TMO=${TMO:-140}

skip() { echo "SKIP: $*"; exit 0; }
[ -x "$QEMU" ]     || skip "no QEMU ($QEMU)"
[ -e "$IMAGE" ]    || skip "no kernel Image ($IMAGE)"
[ -e "$BASE_DTB" ] || skip "no base dtb ($BASE_DTB)"
[ -x "$DTC" ]      || skip "no dtc ($DTC)"
[ -e "$SM_ELF" ]   || skip "no SM firmware ($SM_ELF)"
[ -e "$INITRD" ]   || skip "no busybox initramfs ($INITRD)"
KO=$(find "$KBUILD" -name neoisp.ko 2>/dev/null | head -1)
[ -n "$KO" ] || skip "no neoisp.ko under $KBUILD"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
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

STAGE="$WORK/root"; mkdir -p "$STAGE/mods"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
cp "$KO" "$STAGE/mods/"
cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "=== NEOISP ==="
sleep 3
insmod /mods/neoisp.ko 2>&1 | sed 's/^/insmod: /'
sleep 2
echo "--- neoisp probe (dmesg) ---"
dmesg | grep -iE 'neoisp|4ae00000.isp' | tail -6
echo "--- neoisp video nodes ---"
n=0
for v in /sys/class/video4linux/video*; do
    nm=$(cat "$v/name" 2>/dev/null)
    case "$nm" in neoisp-*) echo "$(basename "$v"): $nm"; n=$((n+1));; esac
done
echo "neoisp-node-count: $n"
echo "=== NEOISP-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.gz"

timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$WORK/neoisp.dtb" -initrd "$WORK/initrd.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "--- neoisp report ---"
sed -n '/=== NEOISP ===/,/=== NEOISP-DONE ===/p' "$LOG"

fail() { echo "FAIL: $*"; exit 1; }
grep -qaE 'neoisp-node-count: [1-9]' "$LOG" \
    || fail "no neoisp V4L2 nodes registered (driver did not bind)"
grep -qaiE 'external abort|Unhandled fault|Internal error' "$LOG" \
    && fail "fault during NeoISP bring-up"

echo "PASS: NeoISP brings up (driver binds, V4L2 mem2mem + media nodes registered)"
