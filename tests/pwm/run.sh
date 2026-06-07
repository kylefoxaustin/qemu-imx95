#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 TPM PWM test (hw/misc/imx_tpm_pwm.c, pwm-imx-tpm driver).
#
# The pwm@424e0000/42510000 controllers were UNIMP stubs, so the driver read
# PARAM == 0 and failed with "failed to add PWM chip". With the real model they
# register (npwm = 6) and the data path is functional: set a period + duty via
# sysfs and read them back.
#
# PASS = two pwmchips with npwm=6 + the period/duty round-trips.
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
# The TPM chips have 6 channels; find one by npwm (other PWMs, e.g. the adp5585,
# may enumerate as lower-numbered pwmchips).
C=""
for c in /sys/class/pwm/pwmchip*; do
    [ "$(cat $c/npwm 2>/dev/null)" = 6 ] && C=$c && break
done
echo "=== PWM-TEST ==="
echo "chips: $(ls /sys/class/pwm/ 2>/dev/null | tr '\n' ' ')"
echo "tpm=$C npwm=$(cat $C/npwm 2>/dev/null)"
echo 0 > $C/export 2>/dev/null
echo 1000000 > $C/pwm0/period 2>/dev/null
echo 400000 > $C/pwm0/duty_cycle 2>/dev/null
echo 1 > $C/pwm0/enable 2>/dev/null
echo "rd period=$(cat $C/pwm0/period 2>/dev/null) duty=$(cat $C/pwm0/duty_cycle 2>/dev/null) en=$(cat $C/pwm0/enable 2>/dev/null)"
echo "=== PWM-TEST-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$root/init"
( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"
timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -serial file:"$LOG" -serial null >/dev/null 2>&1 || true
echo "=== pwm report ==="; sed -n '/PWM-TEST ===/,/PWM-TEST-DONE/p' "$LOG" | grep -vE 'PWM-TEST ==='
pass=1
grep -qa 'npwm=6' "$LOG" && echo "  ok   pwmchip npwm=6" || { echo "  MISS npwm"; pass=0; }
grep -qa 'rd period=1000000 duty=400000 en=1' "$LOG" && echo "  ok   period/duty round-trip" || { echo "  MISS round-trip"; pass=0; }
[ "$pass" = 1 ] && { echo "PASS: TPM PWM registers + functional"; exit 0; }
echo "FAIL"; exit 1
