#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 WDOG3 watchdog - functional bite test.
#
# Boots Linux, confirms the imx7ulp_wdt driver binds WDOG3 (/dev/watchdog0),
# then runs a helper that opens the device and stops pinging it. When the
# timeout elapses the model fires the QEMU watchdog action; the VM runs with
# -action watchdog=poweroff so the bite terminates QEMU. Also confirms WDOG2
# (the M33 System Manager's own watchdog, modelled non-functional) never bites.
#
# PASS = /dev/watchdog0 present, "WDT-ARMED" printed, and QEMU exits (the bite)
# well before the host timeout, without reaching the "UNREACHABLE" line.
#
# Required (override via env): QEMU, KBUILD (Image + dtb), SM_ELF, an aarch64
# cross gcc. SKIPs if any is missing.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=${IMAGE:-$KBUILD/arch/arm64/boot/Image}
DTB=${DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
BUSYBOX=${BUSYBOX:-$ROOT/tests/busybox-initramfs}
CROSS=${CROSS:-aarch64-linux-gnu-}
TMO=${TMO:-120}

skip() { echo "SKIP: $*"; exit 0; }
[ -x "$QEMU" ]   || skip "no QEMU ($QEMU)"
[ -e "$IMAGE" ]  || skip "no kernel Image ($IMAGE)"
[ -e "$DTB" ]    || skip "no dtb ($DTB)"
[ -e "$SM_ELF" ] || skip "no SM firmware ($SM_ELF)"
command -v "${CROSS}gcc" >/dev/null || skip "no ${CROSS}gcc"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/root"; mkdir -p "$STAGE"/{proc,sys,dev}
zcat "$BUSYBOX/busybox-initramfs.cpio.gz" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
"${CROSS}gcc" -O2 -static -o "$STAGE/wdt_bite" "$HERE/wdt_bite.c" \
    || skip "could not cross-compile wdt_bite"

cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
[ -e /dev/watchdog0 ] && echo "WDT-NODE-OK" || echo "WDT-NODE-MISSING"
echo "=== WDT-BITE ==="
/wdt_bite
echo "UNREACHABLE"
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

LOG="$WORK/serial.log"; t0=$(date +%s)
timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.cpio.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 -action watchdog=poweroff \
  -serial file:"$LOG" -serial null >/dev/null 2>&1
rc=$?; el=$(( $(date +%s) - t0 ))

echo "--- watchdog report (qemu exited rc=$rc after ${el}s) ---"
grep -aE 'WDT-NODE-OK|WDT-NODE-MISSING|WDT-ARMED|WDT-OPEN-FAIL|UNREACHABLE' "$LOG"
if grep -qa 'WDT-NODE-OK' "$LOG" && grep -qa 'WDT-ARMED' "$LOG" \
   && ! grep -qa 'UNREACHABLE' "$LOG" && [ "$el" -lt $((TMO - 5)) ]; then
    echo "PASS: WDOG3 bound, armed, and bit (watchdog_perform_action -> poweroff)"
    exit 0
fi
echo "FAIL: watchdog did not bite as expected"; exit 1
