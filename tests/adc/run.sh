#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 ADC test (hw/misc/imx93_adc.c, imx93_adc iio driver).
#
# adc@44530000 was a UNIMP stub (MSR == 0), so the driver looped on "ADC do not
# in power down mode". The real model runs the power/calibration/conversion
# state machine, so the iio device registers and every channel converts.
#
# PASS = iio:device0 is imx93-adc and in_voltage0_raw reads a value.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=$KBUILD/arch/arm64/boot/Image
DTB=$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb
TMO=${TMO:-120}
need() { [ -e "$2" ] || { echo "missing $1: $2"; exit 1; }; }
need QEMU "$QEMU"; need IMAGE "$IMAGE"; need DTB "$DTB"; need SM_ELF "$SM_ELF"
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG=${LOG:-$WORK/serial.log}; root="$WORK/root"; mkdir -p "$root"
zcat "$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz" | (cd "$root" && cpio -idmu 2>/dev/null)
cat > "$root/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc; /bin/busybox mount -t sysfs sysfs /sys
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
sleep 9
echo "=== ADC-TEST ==="
D=/sys/bus/iio/devices/iio:device0
echo "name=$(cat $D/name 2>/dev/null)"
echo "ch0=$(cat $D/in_voltage0_raw 2>/dev/null) ch3=$(cat $D/in_voltage3_raw 2>/dev/null)"
echo "=== ADC-TEST-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$root/init"
( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"
timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -serial file:"$LOG" -serial null >/dev/null 2>&1 || true
echo "=== adc report ==="; sed -n '/ADC-TEST ===/,/ADC-TEST-DONE/p' "$LOG" | grep -vE 'ADC-TEST ==='
pass=1
grep -qa 'name=imx93-adc' "$LOG" && echo "  ok   imx93-adc iio device" || { echo "  MISS iio device"; pass=0; }
grep -qaE 'ch0=[0-9]+ ch3=[0-9]+' "$LOG" && echo "  ok   channels convert" || { echo "  MISS conversion"; pass=0; }
pd=$(grep -acE 'do not in power down' "$LOG" 2>/dev/null || true)
[ "${pd:-0}" = 0 ] && echo "  ok   no power-down loop" || { echo "  MISS power-down ($pd)"; pass=0; }
[ "$pass" = 1 ] && { echo "PASS: imx93 ADC registers + converts"; exit 0; }
echo "FAIL"; exit 1
