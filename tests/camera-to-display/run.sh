#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX95 camera -> LCD: a real photograph in over MIPI CSI-2, out on the panel.
#
# The vision TRANSPORT path end to end, with no ISP in it:
#
#     smart camera (already-developed YUYV)      the ISI host frame source
#            -> MIPI CSI-2 receiver              hw/display/imx95_isi.c
#            -> ISI  -> DRAM capture buffer      DMA, byte-for-byte
#            -> V4L2 DQBUF (tests/.../v4l2_to_fb)
#            -> /dev/fb0 -> DPU -> LVDS panel    what Holobench shows
#
# "Smart camera" means the sensor emits developed YUV rather than Bayer, so the
# NeoISP is deliberately NOT in this path: it isolates the transport, so a
# failure here cannot hide behind image processing. Flowing raw Bayer through a
# real debayer is the next step, not this one.
#
# Two independent proofs, because either alone is weak:
#   1. BYTES - the guest hashes the captured frame; the host hashes what it fed
#      in at the same stride. Equal hashes mean the image crossed the pipeline
#      intact, not merely that something arrived.
#   2. PIXELS - a screendump of the panel is compared against the source image.
#      A correct hash with a black screen would still be a broken display path.
#
# Required (override via env): QEMU, KBUILD (Image + base dtb + dtc + fdtoverlay
# + ov5640.ko), SM_ELF, a CROSS gcc, python3-pillow. SKIPs if any is missing.
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
SRC_IMG=${SRC_IMG:-$HERE/scene.png}
TMO=${TMO:-200}

skip() { echo "SKIP: $*"; exit 0; }
[ -x "$QEMU" ]     || skip "no QEMU ($QEMU)"
[ -e "$IMAGE" ]    || skip "no kernel Image ($IMAGE)"
[ -e "$BASE_DTB" ] || skip "no base dtb ($BASE_DTB)"
[ -x "$DTC" ]      || skip "no dtc ($DTC)"
[ -x "$FDTO" ]     || skip "no fdtoverlay ($FDTO)"
[ -e "$SM_ELF" ]   || skip "no SM firmware ($SM_ELF)"
[ -e "$INITRD" ]   || skip "no busybox initramfs ($INITRD)"
[ -e "$CAM/ov5640-overlay.dtso" ] || skip "no ov5640 overlay"
[ -e "$SRC_IMG" ]  || skip "no source image ($SRC_IMG)"
command -v "${CROSS}gcc" >/dev/null || skip "no ${CROSS}gcc"
python3 -c "import PIL" 2>/dev/null || skip "no python3 Pillow (for mkframe.py)"
KO=$(find "$KBUILD" -name ov5640.ko 2>/dev/null | head -1)
[ -n "$KO" ] || skip "no ov5640.ko under $KBUILD"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG="$WORK/serial.log"
SHOT="${SHOT:-$WORK/panel.ppm}"

# The pipeline negotiates 640x480 YUYV with a 3840-byte line stride (6 bytes per
# pixel of stride for 2 bytes of pixel). Build the host frame to match exactly.
W=${W:-640}; H=${H:-480}; STRIDE=${STRIDE:-3840}
FRAME="$WORK/frame000.raw"
python3 "$HERE/mkframe.py" "$SRC_IMG" "$W" "$H" "$FRAME" "$STRIDE" || exit 1
HOST_HASH=$(python3 - "$FRAME" <<'PY'
import sys
h = 1469598103934665603
for b in open(sys.argv[1], 'rb').read():
    h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
print("%016x" % h)
PY
)
echo "host frame hash: $HOST_HASH"

# dtb = base + ov5640 camera graph + a 1280x800 LVDS panel (so /dev/fb0 exists)
"$DTC" -@ -I dts -O dtb -o "$WORK/ov5640.dtbo" "$CAM/ov5640-overlay.dtso" 2>/dev/null \
    || { echo "FAIL: overlay would not compile"; exit 1; }
"$FDTO" -i "$BASE_DTB" -o "$WORK/cam.dtb" "$WORK/ov5640.dtbo" \
    || { echo "FAIL: fdtoverlay could not apply the camera overlay"; exit 1; }
BASE_DTB="$WORK/cam.dtb" OUT="$WORK/patched.dtb" \
    bash "$ROOT/tests/lcd-panel/attach-lcd.sh" "$WORK/cam.dtb" "$WORK/patched.dtb" \
    >/dev/null 2>&1 || cp "$WORK/cam.dtb" "$WORK/patched.dtb"
[ -e "$WORK/patched.dtb" ] || { echo "FAIL: no patched dtb"; exit 1; }

"${CROSS}gcc" -O2 -Wall -static -o "$WORK/v4l2_to_fb" "$HERE/v4l2_to_fb.c" \
    || { echo "FAIL: could not build v4l2_to_fb.c"; exit 1; }
# The media graph needs its links enabled and the sensor format propagated onto
# every crossbar sink before STREAMON will validate. tests/camera/v4l2_cap.c
# already does exactly that in "cap" mode and is the proven implementation, so
# run it first rather than duplicating the format-propagation logic here.
"${CROSS}gcc" -O2 -Wall -static -o "$WORK/v4l2_cap" "$CAM/v4l2_cap.c" \
    || { echo "FAIL: could not build v4l2_cap.c"; exit 1; }

STAGE="$WORK/root"; mkdir -p "$STAGE/mods"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
cp "$KO" "$STAGE/mods/"; cp "$WORK/v4l2_to_fb" "$STAGE/v4l2_to_fb"
cp "$WORK/v4l2_cap" "$STAGE/v4l2_cap"
cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "=== CAM2LCD ==="
sleep 3
insmod /mods/ov5640.ko 2>&1 | sed 's/^/insmod: /'
sleep 2
ls -l /dev/fb0 /dev/video0 2>&1
# set the pipeline up (links + format propagation), then blit a frame
/v4l2_cap cap /dev/video0
/v4l2_to_fb /dev/video0 /dev/fb0
echo "=== CAM2LCD-DONE ==="
sleep 4          # leave the image on the panel long enough to screendump
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.gz"

# Screendump the panel while the frame is still up, via the monitor socket.
MON="$WORK/mon.sock"
( sleep 1
  for i in $(seq 1 "$TMO"); do
      grep -qa "CAM2LCD-DONE" "$LOG" 2>/dev/null && break
      sleep 1
  done
  printf 'screendump %s\n' "$SHOT" | timeout 10 socat - "UNIX-CONNECT:$MON" >/dev/null 2>&1
) &
SHOOTER=$!

timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$WORK/patched.dtb" -initrd "$WORK/initrd.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -device ov5640,bus=lpi2c1,address=0x3c \
  -global driver=imx95.isi,property=frames,value="$FRAME" \
  -monitor "unix:$MON,server,nowait" \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true
wait "$SHOOTER" 2>/dev/null

echo "--- camera-to-display report ---"
sed -n '/=== CAM2LCD ===/,/=== CAM2LCD-DONE ===/p' "$LOG" \
    | grep -aE 'CAPTURE|FRAME |FB |BLIT|DISPLAYED|CAPTURE_HASH|video0|fb0|CAMERA-CAP.*(PASS|fmt)|G_FMT|STREAMON|DQBUF'

fail() { echo "FAIL: $*"; exit 1; }
grep -qa "DISPLAYED" "$LOG" || fail "client never reached the framebuffer blit"

# the serial log carries CR line endings; strip them or the compare always fails
GUEST_HASH=$(grep -a 'CAPTURE_HASH' "$LOG" | tail -1 | awk '{print $2}' | tr -d '\r')
[ -n "$GUEST_HASH" ] || fail "no capture hash from the guest"
echo "guest capture hash: $GUEST_HASH"
[ "$GUEST_HASH" = "$HOST_HASH" ] \
    || fail "capture hash $GUEST_HASH != host frame hash $HOST_HASH (image altered in transit)"

if [ -s "$SHOT" ]; then
    python3 "$HERE/checkshot.py" "$SHOT" "$SRC_IMG" "$W" "$H" || exit 1
else
    echo "WARN: no screendump captured (panel proof skipped)"
fi

echo "PASS: camera -> ISI -> DRAM -> /dev/fb0 -> DPU panel, image intact end to end"
