#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 LPI2C i2c-2 (0x42530000) bus test.
#
# That bus was an UNIMP stub, so its on-board devices timed out (-110). With a
# real master + register-file slaves they now probe: the PCA953x IO-expander
# (2-0020) and the PCA9632 LED controller (2-0062, leds-pca963x).
#
# PASS = the LED class device registers and the expander probes (no -110).
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
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
sleep 9
echo "=== I2C2-TEST ==="
echo "leds: $(ls /sys/class/leds/ 2>/dev/null | tr '\n' ' ')"
echo "=== I2C2-TEST-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$root/init"
( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "=== i2c-2 report ==="
sed -n '/I2C2-TEST ===/,/I2C2-TEST-DONE/p' "$LOG" | grep -vE 'I2C2-TEST ==='
pass=1
grep -qa 'pca963x:backlight' "$LOG" && echo "  ok   PCA9632 LED controller bound" || { echo "  MISS leds-pca963x"; pass=0; }
n=$(grep -caE 'pca953x 2-0020.* -110' "$LOG" 2>/dev/null || true)
[ "${n:-0}" = 0 ] && echo "  ok   PCA953x expander probed (no -110)" || { echo "  MISS expander (-110)"; pass=0; }
[ "$pass" = 1 ] && { echo "PASS: i2c-2 bus + devices functional"; exit 0; }
echo "FAIL"; exit 1
