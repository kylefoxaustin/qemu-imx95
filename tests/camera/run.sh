#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX95 camera capture: MIPI CSI-2 -> CSI formatter -> ISI -> V4L2 DQBUF.
#
# Applies the ov5640 camera overlay (sensor on lpi2c1 + the full media graph),
# attaches the QEMU ov5640 sensor model, loads the ov5640 driver, and drives the
# whole V4L2 media graph end to end:
#   ov5640 (sensor) -> csidev-...csi (MIPI CSI-2 rx) -> syscon:formatter
#     -> crossbar -> mxc_isi.0 -> mxc_isi.0.capture (/dev/video0)
# The ISI capture engine (hw/display/imx95_isi.c) registers all eight
# mxc_isi.N.capture video nodes and, on a channel's CHNL_EN, DMAs a synthesised
# moving test pattern into the capture buffers. The QEMU ov5640 model answers
# the sensor chip-ID read so the sensor subdev binds and the graph links.
#
# A cross-compiled V4L2 client (v4l2_cap.c) sets up the streams-API pipeline -
# enables the sensor link, reads the sensor's UYVY8_1X16 format, propagates it
# along the crossbar's connected sink so the media-core link_validate passes -
# then REQBUFS/STREAMON and DQBUFs five frames via MMAP. The test asserts the
# capture PASS, which exercises the ISI channel DMA (CHNL_OUT_BUF / CHNL_EN) and
# the frame-stored interrupts: the dequeued buffers carry the model's moving
# test pattern (non-zero, advancing per frame). The "links" mode is a userspace
# link_validate oracle (per-link format diff) kept for diagnostics.
#
# Required (override via env): QEMU, KBUILD (Image + base dtb + dtc + fdtoverlay
# + ov5640.ko), SM_ELF, a CROSS gcc for the V4L2 client. SKIPs if any is
# missing.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

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
command -v "${CROSS}gcc" >/dev/null || skip "no ${CROSS}gcc (set CROSS=)"
OV5640_KO=$(find "$KBUILD" -name ov5640.ko 2>/dev/null | head -1)
[ -n "$OV5640_KO" ] || skip "no ov5640.ko under $KBUILD"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG="$WORK/serial.log"

# Build the camera-graph dtb: base + ov5640 overlay (fdtoverlay uses the base
# dtb's __symbols__, which the BSP dtb carries).
"$DTC" -@ -I dts -O dtb -o "$WORK/ov5640.dtbo" "$HERE/ov5640-overlay.dtso" \
    2>/dev/null || { echo "FAIL: overlay would not compile"; exit 1; }
"$FDTO" -i "$BASE_DTB" -o "$WORK/patched.dtb" "$WORK/ov5640.dtbo" \
    || { echo "FAIL: fdtoverlay could not apply the overlay"; exit 1; }

# Cross-compile the V4L2 capture client (streams-API pipeline + MMAP DQBUF).
"${CROSS}gcc" -O2 -Wall -static -o "$WORK/v4l2_cap" "$HERE/v4l2_cap.c" \
    || { echo "FAIL: could not build v4l2_cap.c"; exit 1; }

STAGE="$WORK/root"; mkdir -p "$STAGE/mods"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
cp "$OV5640_KO" "$STAGE/mods/"
cp "$WORK/v4l2_cap" "$STAGE/v4l2_cap"
cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "=== CAM ==="
sleep 3
insmod /mods/ov5640.ko 2>&1 | sed 's/^/insmod: /'
sleep 2
echo "--- v4l2 subdevs (media graph) ---"
for s in /sys/class/video4linux/v4l-subdev*; do
    echo "$(basename "$s"): $(cat "$s/name" 2>/dev/null)"
done
echo "--- ISI capture video nodes ---"
for v in /sys/class/video4linux/video*; do
    n=$(cat "$v/name" 2>/dev/null)
    case "$n" in *isi*capture*) echo "$(basename "$v"): $n";; esac
done
echo "--- V4L2 capture (streams pipeline + DQBUF) ---"
/v4l2_cap cap /dev/video0
echo "=== CAM-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.gz"

timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$WORK/patched.dtb" -initrd "$WORK/initrd.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -device ov5640,bus=lpi2c1,address=0x3c \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "--- camera report ---"
sed -n '/=== CAM ===/,/=== CAM-DONE ===/p' "$LOG"

fail() { echo "FAIL: $*"; exit 1; }
# The sensor model must bind (chip-ID read over I2C) and the whole graph link.
grep -qaE 'ov5640 2-003c'            "$LOG" || fail "ov5640 sensor subdev did not bind"
grep -qaE 'csidev-4ad30000.csi'      "$LOG" || fail "MIPI CSI-2 receiver subdev missing"
grep -qaE 'formatter@20'             "$LOG" || fail "CSI formatter subdev missing"
grep -qaE 'crossbar'                 "$LOG" || fail "ISI crossbar subdev missing"
grep -qaE 'mxc_isi.0.capture'        "$LOG" || fail "ISI capture /dev/video node missing"
grep -qaiE 'external abort|Unhandled fault|Internal error' "$LOG" \
    && fail "fault during camera bring-up"
# The full capture path: STREAMON + five DQBUF'd frames carrying ISI test data.
grep -qaE 'CAMERA-CAP.*: PASS' "$LOG" \
    || fail "V4L2 capture did not DQBUF frames (STREAMON/pipeline)"

echo "PASS: camera capture end to end (ov5640 -> csi -> formatter -> crossbar -> ISI; STREAMON + DQBUF five frames)"
