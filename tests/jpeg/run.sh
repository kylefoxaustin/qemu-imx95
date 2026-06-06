#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# HW JPEG codec registration bar (done-at-bar).
#
# The i.MX 95 has two dedicated HW JPEG codecs, separate from the Wave6 VPU:
# jpegdec @ 0x4c500000 and jpegenc @ 0x4c550000 ("fsl,imx9-jpgdec/jpgenc",
# Linux driver mxc-jpeg, V4L2 mem2mem). This boots Linux, loads the codec
# modules, and checks both register their V4L2 device node.
#
# PASS = both nodes register:
#   mxc-jpeg 4c500000.jpegdec: decoder device registered as /dev/video2
#   mxc-jpeg 4c550000.jpegenc: encoder device registered as /dev/video3
#
# Scope: mxc_jpeg_probe touches no codec registers - it ioremaps, requests its
# IRQs, pulls the SCMI VPU/VPUJPEG clocks + PD_VPU power domain, and registers
# the m2m device - so the nodes appear with no device model. The machine backs
# the two MMIO windows (read-0) so a stream-on (which writes the codec's
# GLB_CTRL/config) does not take a synchronous external abort. Functional
# encode/decode would need the codec compute engine + its frame-done IRQ and is
# out of scope here.
#
# Artifacts (all from the NXP BSP, not redistributable):
#   KERNEL/DTB - stock NXP Image + imx95-19x19-evk.dtb.
#   SM_ELF     - System Manager firmware m33_image.elf.
#   JPEG_MODDIR - the BSP module tree holding v4l2-jpeg.ko and
#                 mxc-jpeg-encdec.ko (CONFIG_VIDEO_IMX8_JPEG=m,
#                 CONFIG_V4L2_JPEG_HELPER=m); the rest of V4L2 core is built in.
#                 e.g. <rootfs>/usr/lib/modules/<kver>/kernel/drivers/media

set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
ART=${IMX95_ARTIFACTS:-$HOME/imx95-artifacts}
KERNEL=${KERNEL:-$ART/linux-build/arch/arm64/boot/Image}
DTB=${DTB:-$ART/linux-build/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
SM_ELF=${SM_ELF:-$ART/m33_image.elf}
BUSYBOX=${BUSYBOX:-$REPO/tests/busybox-initramfs}
JPEG_MODDIR=${JPEG_MODDIR:-}
TMO=${TMO:-120}

need() { [ -e "$1" ] || { echo "missing: $1 (set $2)" >&2; exit 2; }; }
need "$QEMU" QEMU
need "$KERNEL" KERNEL
need "$DTB" DTB
need "$SM_ELF" SM_ELF
[ -n "$JPEG_MODDIR" ] || { echo "set JPEG_MODDIR to the BSP media module dir" >&2; exit 2; }

V4L2_JPEG=$(find "$JPEG_MODDIR" -name 'v4l2-jpeg.ko' | head -1)
MXC_JPEG=$(find "$JPEG_MODDIR" -name 'mxc-jpeg-encdec.ko' | head -1)
need "$V4L2_JPEG" "v4l2-jpeg.ko under JPEG_MODDIR"
need "$MXC_JPEG" "mxc-jpeg-encdec.ko under JPEG_MODDIR"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
root="$WORK/root"; mkdir -p "$root"/{bin,proc,sys,dev,mods}
BB=$(zcat "$BUSYBOX/busybox-initramfs.cpio.gz" | (cd "$root" && cpio -idmu 2>/dev/null); \
     find "$root" -name busybox | head -1)
cp "$V4L2_JPEG" "$MXC_JPEG" "$root/mods/"
cat > "$root/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
insmod /mods/v4l2-jpeg.ko
insmod /mods/mxc-jpeg-encdec.ko
sleep 1
echo "=== JPEG-REGISTER-CHECK ==="
for v in /sys/class/video4linux/*/name; do echo "  $(dirname "$v" | xargs basename) = $(cat "$v")"; done
echo "=== JPEG-REGISTER-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$root/init"
( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

LOG="$WORK/serial.log"
timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$KERNEL" -dtb "$DTB" -initrd "$WORK/initrd.cpio.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "--- video4linux nodes ---"; grep -aE 'registered as /dev/video|mxc-jpeg-(dec|enc)' "$LOG" || true
if grep -qa '4c500000.jpegdec: decoder device registered' "$LOG" && \
   grep -qa '4c550000.jpegenc: encoder device registered' "$LOG"; then
    echo "PASS: both HW JPEG codecs registered (/dev/video2 dec, /dev/video3 enc)"
    exit 0
fi
echo "FAIL: jpeg codec nodes did not register"; exit 1
