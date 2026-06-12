#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX95 ISI "virtual camera" - feed real host frames through CSI/ISI capture.
#
# The ISI (hw/display/imx95_isi.c) normally DMAs a synthetic moving test pattern
# into the capture buffers. With the "frames" property set, it instead scans
# REAL host images out of the capture pipeline - a virtual camera needing no
# sensor and no silicon, e.g. to drive a vision pipeline or the NPU with a fixed
# sequence of frames. (Ported from the i.MX93 ISI's host frame source.)
#
# This boots the camera media graph (the same ov5640 overlay + V4L2 client as
# tests/camera) but adds `-global driver=imx95.isi,property=frames,value=<file>`
# pointing at a host frame, then captures off /dev/video0 and asserts the
# DQBUF'd frames are the HOST frame (a known 0xAB fill) rather than the synthetic
# gradient (whose first bytes would advance 00 04 08 ...). That proves the host
# image reached the capture buffer byte-for-byte through the ISI DMA.
#
# Required (override via env): QEMU, KBUILD (Image + base dtb + dtc + fdtoverlay
# + ov5640.ko), SM_ELF, a CROSS gcc. SKIPs if any is missing.
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
TMO=${TMO:-140}

skip() { echo "SKIP: $*"; exit 0; }
[ -x "$QEMU" ]     || skip "no QEMU ($QEMU)"
[ -e "$IMAGE" ]    || skip "no kernel Image ($IMAGE)"
[ -e "$BASE_DTB" ] || skip "no base dtb ($BASE_DTB)"
[ -x "$DTC" ]      || skip "no dtc ($DTC)"
[ -x "$FDTO" ]     || skip "no fdtoverlay ($FDTO)"
[ -e "$SM_ELF" ]   || skip "no SM firmware ($SM_ELF)"
[ -e "$INITRD" ]   || skip "no busybox initramfs ($INITRD)"
[ -e "$CAM/v4l2_cap.c" ] || skip "no tests/camera/v4l2_cap.c"
[ -e "$CAM/ov5640-overlay.dtso" ] || skip "no ov5640 overlay"
command -v "${CROSS}gcc" >/dev/null || skip "no ${CROSS}gcc (set CROSS=)"
KO=$(find "$KBUILD" -name ov5640.ko 2>/dev/null | head -1)
[ -n "$KO" ] || skip "no ov5640.ko under $KBUILD"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG="$WORK/serial.log"

# The capture is 640x480 with a 6 bytes/pixel ISI output stride -> 1843200-byte
# frames. Make an all-0xAB host frame (distinct from the gradient's 00 04 08 …).
FRAME="$WORK/frame000.raw"
python3 -c "open('$FRAME','wb').write(b'\xab'*1843200)"

"$DTC" -@ -I dts -O dtb -o "$WORK/ov5640.dtbo" "$CAM/ov5640-overlay.dtso" \
    2>/dev/null || { echo "FAIL: overlay would not compile"; exit 1; }
"$FDTO" -i "$BASE_DTB" -o "$WORK/patched.dtb" "$WORK/ov5640.dtbo" \
    || { echo "FAIL: fdtoverlay could not apply the overlay"; exit 1; }
"${CROSS}gcc" -O2 -Wall -static -o "$WORK/v4l2_cap" "$CAM/v4l2_cap.c" \
    || { echo "FAIL: could not build v4l2_cap.c"; exit 1; }

STAGE="$WORK/root"; mkdir -p "$STAGE/mods"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
cp "$KO" "$STAGE/mods/"; cp "$WORK/v4l2_cap" "$STAGE/v4l2_cap"
cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "=== VCAM ==="
sleep 3
insmod /mods/ov5640.ko 2>&1 | sed 's/^/insmod: /'
sleep 2
/v4l2_cap cap /dev/video0
echo "=== VCAM-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.gz"

timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$WORK/patched.dtb" -initrd "$WORK/initrd.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -device ov5640,bus=lpi2c1,address=0x3c \
  -global driver=imx95.isi,property=frames,value="$FRAME" \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "--- virtual-camera report ---"
sed -n '/=== VCAM ===/,/=== VCAM-DONE ===/p' "$LOG" | grep -aE 'CAMERA-CAP'

fail() { echo "FAIL: $*"; exit 1; }
grep -qaE 'CAMERA-CAP.*: PASS' "$LOG" \
    || fail "capture did not complete"
# Every DQBUF'd frame must be the host 0xAB fill, not the synthetic gradient.
got=$(grep -acE 'p\[0..3\]=abababab' "$LOG")
[ "$got" -ge 3 ] \
    || fail "captured frames are not the host image (got $got abababab frames)"
grep -qaE 'p\[0..3\]=00040[08]' "$LOG" \
    && fail "saw the synthetic gradient - host frame did not override it"

echo "PASS: virtual camera - host frame scanned through ISI to DQBUF byte-for-byte ($got/5 frames)"
