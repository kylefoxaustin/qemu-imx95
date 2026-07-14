#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Extra SAI controllers (sai2/sai4/sai5) registration bring-up.
#
# The 19x19 EVK wires only sai1 + sai3 (already functional - WM8962 playback +
# BT-SCO); sai2@4c880000, sai4@42660000 and sai5@42670000 are status=disabled.
# Per the "model every SoC peripheral" directive the machine now instantiates
# all five (same fsl,imx95-sai IP), and this test patches the dtb to enable the
# three extra nodes and confirms the fsl-sai driver probes and binds each one
# (its CPU DAI component registers). They share the modelled SAI IP; a full
# playback card would also need a codec + card node, so this is registration
# bring-up, like the sai1/sai3 controllers before their cards were wired.
#
# Required (override via env): QEMU, KBUILD (Image + dtb + dtc + the snd .ko
# tree), SM_ELF. SKIPs if any is missing.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=${IMAGE:-$KBUILD/arch/arm64/boot/Image}
DTB=${DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
DTC=${DTC:-$KBUILD/scripts/dtc/dtc}
INITRD=${INITRD:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
TMO=${TMO:-130}

skip() { echo "SKIP: $*"; exit 0; }
[ -x "$QEMU" ]   || skip "no QEMU ($QEMU)"
[ -e "$IMAGE" ]  || skip "no kernel Image ($IMAGE)"
[ -e "$DTB" ]    || skip "no dtb ($DTB)"
[ -e "$DTC" ]    || skip "no dtc ($DTC)"
[ -e "$SM_ELF" ] || skip "no SM firmware ($SM_ELF)"
[ -e "$INITRD" ] || skip "no busybox initramfs ($INITRD)"

MODS="snd-soc-fsl-utils imx-pcm-dma snd-soc-fsl-sai"
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG="$WORK/serial.log"

# Enable sai2/sai4/sai5 (flip each node's own status to okay).
"$DTC" -I dtb -O dts "$DTB" > "$WORK/evk.dts" 2>/dev/null
python3 - "$WORK/evk.dts" "$WORK/patched.dts" <<'PY'
import sys, re
t = open(sys.argv[1]).read()
def enable(node):
    global t
    i = t.index(node); b = t.index('{', i); depth, j = 0, b
    while True:
        c = t[j]
        if c == '{': depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0: break
        j += 1
    body = t[b:j].replace('status = "disabled";', 'status = "okay";')
    t = t[:b] + body + t[j:]
for n in ('sai@4c880000', 'sai@42660000', 'sai@42670000'):
    enable(n)
open(sys.argv[2], 'w').write(t)
PY
"$DTC" -I dts -O dtb -o "$WORK/patched.dtb" "$WORK/patched.dts" 2>/dev/null \
    || { echo "FAIL: dtc could not rebuild patched dtb"; exit 1; }

STAGE="$WORK/root"; mkdir -p "$STAGE/mods"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
for m in $MODS; do
    f=$(find "$KBUILD" -name "$m.ko" 2>/dev/null | head -1)
    [ -n "$f" ] || skip "missing $m.ko under $KBUILD"
    cp "$f" "$STAGE/mods/"
done

cat > "$STAGE/init" <<INIT
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "=== SAI-EXTRA ==="
for m in $MODS; do insmod /mods/\$m.ko 2>&1 | sed "s/^/insmod \$m: /"; done
sleep 3
echo "--- fsl-sai bound devices ---"
ls -1 /sys/bus/platform/drivers/fsl-sai/ 2>/dev/null | grep -E '\.sai$'
echo "--- sai dmesg ---"
dmesg | grep -aiE '\.sai|fsl-sai|fsl_sai' | grep -aivE 'already present' | head
echo "=== SAI-EXTRA-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$WORK/patched.dtb" -initrd "$WORK/initrd.cpio.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "--- sai-extra report ---"
sed -n '/=== SAI-EXTRA ===/,/=== SAI-EXTRA-DONE ===/p' "$LOG"

fail() { echo "FAIL: $*"; exit 1; }
for d in 4c880000.sai 42660000.sai 42670000.sai; do
    grep -qa "$d" "$LOG" || fail "$d did not bind to fsl-sai"
done
grep -qaiE 'external abort|Unhandled fault|Internal error' "$LOG" \
    && fail "fault during sai bring-up"

echo "PASS: sai2/sai4/sai5 all bound to fsl-sai"
