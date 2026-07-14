#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 RGPIO (fsl,imx95-gpio) functional test.
#
# Boots stock Linux and checks (1) gpio-vf610 binds the four enabled SoC GPIO
# controllers as real gpiochips, and (2) the data path is FUNCTIONAL, not a
# stub: drive an output pin via devmem (PDDR/PSOR/PCOR/PTOR) and read its pad
# state back from PDIR. On the old UNIMP stub PDIR reads 0; the real model
# reflects (PDOR & PDDR).
#
# PASS = 4 gpiochips on the 0x438x0000 controllers + the devmem loopback tracks
#        + zero faults.
#
# Required artifacts (override via env): QEMU, KBUILD (Image + dtb), SM_ELF.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=$KBUILD/arch/arm64/boot/Image
DTB=$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb
TMO=${TMO:-120}

need() { [ -e "$2" ] || { echo "missing $1: $2"; exit 1; }; }
need QEMU "$QEMU"; need IMAGE "$IMAGE"; need DTB "$DTB"; need SM_ELF "$SM_ELF"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG=${LOG:-$WORK/serial.log}
root="$WORK/root"; mkdir -p "$root"
zcat "$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz" | \
    (cd "$root" && cpio -idmu 2>/dev/null)
cat > "$root/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox mount -t debugfs none /sys/kernel/debug 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
sleep 8
echo "=== GPIO-TEST ==="
echo "-- gpiochips --"
cat /sys/kernel/debug/gpio 2>/dev/null | grep -E '4381|4382|4384|4385' 
# GPIO2 @ 0x43810000, data block at +0x40: PDOR .40 PSOR .44 PCOR .48 PDIR .50 PDDR .54
echo "-- devmem loopback (pin28) --"
# Mask to pin28: undriven input pins idle high, so only the driven output
# bit is deterministic in PDIR.
devmem 0x43810054 32 0x10000000          # PDDR: pin28 output
devmem 0x43810044 32 0x10000000          # PSOR: set
s1=$(( $(devmem 0x43810050 32) & 0x10000000 ))   # PDIR, pin28 bit
devmem 0x43810048 32 0x10000000          # PCOR: clear
s0=$(( $(devmem 0x43810050 32) & 0x10000000 ))   # PDIR, pin28 bit
echo "set->bit28=$s1 clear->bit28=$s0"
echo "=== GPIO-TEST-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$root/init"
( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "=== gpio test report ==="
sed -n '/GPIO-TEST ===/,/GPIO-TEST-DONE/p' "$LOG" | grep -vE 'GPIO-TEST ==='

pass=1
nchips=$(grep -acE '438[1245]0000.gpio' "$LOG")
[ "$nchips" -ge 4 ] && echo "  ok   4 SoC gpiochips bound ($nchips)" || { echo "  MISS gpiochips ($nchips/4)"; pass=0; }
grep -qa 'set->bit28=268435456 clear->bit28=0' "$LOG" \
    && echo "  ok   devmem loopback functional (PDIR tracks PDOR&PDDR)" \
    || { echo "  MISS devmem loopback (stub?)"; pass=0; }
for p in 'external abort' 'Unhandled fault' 'Oops:' 'Kernel panic'; do
    n=$(grep -caE "$p" "$LOG" 2>/dev/null || true); [ "${n:-0}" = "0" ] || { echo "  anomaly: $p=$n"; pass=0; }
done

[ "$pass" = 1 ] && { echo "PASS: imx95 RGPIO binds + functional"; exit 0; }
echo "FAIL"; exit 1
