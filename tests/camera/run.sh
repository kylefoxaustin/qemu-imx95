#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX95 camera bring-up: MIPI CSI-2 -> CSI formatter -> ISI media graph.
#
# Applies the ov5640 camera overlay (sensor on lpi2c3 + the full media graph),
# attaches the QEMU ov5640 sensor model, loads the ov5640 driver, and confirms
# the whole V4L2 media graph binds end to end:
#   ov5640 (sensor) -> csidev-...csi (MIPI CSI-2 rx) -> syscon:formatter
#     -> crossbar -> mxc_isi.0 -> mxc_isi.0.capture (/dev/video0)
# The ISI capture engine (hw/display/imx95_isi.c) registers all eight
# mxc_isi.N.capture video nodes and, on a channel's CHNL_EN, DMAs a synthesised
# moving test pattern into the capture buffers. The QEMU ov5640 model answers
# the sensor chip-ID read so the sensor subdev binds and the graph links.
#
# This gate proves the camera subsystem brings up (every subdev binds + the
# capture nodes register, no external abort). The final STREAMON/DQBUF frame
# capture additionally needs the imx8-isi crossbar's per-stream media-ctl format
# recipe; that is a documented follow-on (see docs/README camera entry).
#
# Required (override via env): QEMU, KBUILD (Image + base dtb + dtc + fdtoverlay
# + ov5640.ko), SM_ELF. SKIPs if any is missing.
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
TMO=${TMO:-140}

skip() { echo "SKIP: $*"; exit 0; }
[ -x "$QEMU" ]     || skip "no QEMU ($QEMU)"
[ -e "$IMAGE" ]    || skip "no kernel Image ($IMAGE)"
[ -e "$BASE_DTB" ] || skip "no base dtb ($BASE_DTB)"
[ -x "$DTC" ]      || skip "no dtc ($DTC)"
[ -x "$FDTO" ]     || skip "no fdtoverlay ($FDTO)"
[ -e "$SM_ELF" ]   || skip "no SM firmware ($SM_ELF)"
[ -e "$INITRD" ]   || skip "no busybox initramfs ($INITRD)"
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

STAGE="$WORK/root"; mkdir -p "$STAGE/mods"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
cp "$OV5640_KO" "$STAGE/mods/"
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
echo "=== CAM-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.gz"

timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
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

echo "PASS: camera media graph binds end to end (ov5640 -> csi -> formatter -> crossbar -> ISI; capture nodes registered)"
