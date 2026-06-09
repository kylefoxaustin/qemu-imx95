#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX95 MIPI-DSI display output: DPU pixel link -> MIPI-DSI host -> DSI panel.
#
# Applies the rm67191 DSI panel overlay (the DPU pixel-link + pixel-interleaver
# + mipi_dsi enabled, with a Raydium RM67191 DSI panel), boots, and confirms the
# DSI display path brings up: the imx95-mipi-dsi bridge binds to the DPU pixel
# link, the panel's DSI DCS init command stream completes, and the DSI connector
# reports connected with the panel's mode - the same DRM scanout the LVDS path
# already uses for the boot logo, here over DSI.
#
# The QEMU imx95.dsi model (hw/display/imx95_dsi.c) answers the dw-mipi-dsi
# core's status polls (DSI_VERSION, DSI_CMD_PKT_STATUS idle, DSI_PHY_STATUS lock)
# so the bridge enables and the panel command FIFO drains; the DPU model does
# the actual framebuffer scanout.
#
# Required (override via env): QEMU, KBUILD (Image + base dtb + dtc +
# fdtoverlay), SM_ELF. The raydium-rm67191 panel driver must be built into the
# kernel (CONFIG_DRM_PANEL_RAYDIUM_RM67191=y). SKIPs if any input is missing.
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
TMO=${TMO:-130}

skip() { echo "SKIP: $*"; exit 0; }
[ -x "$QEMU" ]     || skip "no QEMU ($QEMU)"
[ -e "$IMAGE" ]    || skip "no kernel Image ($IMAGE)"
[ -e "$BASE_DTB" ] || skip "no base dtb ($BASE_DTB)"
[ -x "$DTC" ]      || skip "no dtc ($DTC)"
[ -x "$FDTO" ]     || skip "no fdtoverlay ($FDTO)"
[ -e "$SM_ELF" ]   || skip "no SM firmware ($SM_ELF)"
[ -e "$INITRD" ]   || skip "no busybox initramfs ($INITRD)"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG="$WORK/serial.log"

# Build the DSI dtb: base + rm67191 overlay (fdtoverlay uses the base __symbols__).
"$DTC" -@ -I dts -O dtb -o "$WORK/dsi.dtbo" "$HERE/rm67191-overlay.dtso" \
    2>/dev/null || { echo "FAIL: overlay would not compile"; exit 1; }
"$FDTO" -i "$BASE_DTB" -o "$WORK/patched.dtb" "$WORK/dsi.dtbo" \
    || { echo "FAIL: fdtoverlay could not apply the overlay"; exit 1; }

STAGE="$WORK/root"; mkdir -p "$STAGE"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "=== DSI ==="
sleep 4
echo "--- DSI host + panel ---"
dmesg | grep -aiE 'mipi-dsi.*Pixel Link|rm67191|raydium|dpudrmfb|Failed to send MCS|command FIFO' | head
echo "--- DSI connector status + modes ---"
for d in /sys/class/drm/card0-DSI*; do
    echo "$(basename "$d"): status=$(cat "$d/status" 2>/dev/null) mode=$(cat "$d/modes" 2>/dev/null)"
done
echo "--- fb/drm nodes ---"; ls /dev/fb0 /dev/dri/card0 2>/dev/null
echo "=== DSI-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.gz"

timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$WORK/patched.dtb" -initrd "$WORK/initrd.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "--- dsi report ---"
sed -n '/=== DSI ===/,/=== DSI-DONE ===/p' "$LOG"

fail() { echo "FAIL: $*"; exit 1; }
grep -qaE 'Using Pixel Link0 as input source' "$LOG" \
    || fail "imx95-mipi-dsi did not bind to the DPU pixel link"
grep -qaiE 'Failed to send MCS|failed to write command FIFO' "$LOG" \
    && fail "the panel DSI init command stream did not complete"
grep -qaE 'card0-DSI.*status=connected' "$LOG" \
    || fail "DSI connector did not report connected"
grep -qaE 'card0-DSI.*mode=[0-9]+x[0-9]+' "$LOG" \
    || fail "DSI connector advertised no mode (panel EDID/mode missing)"
grep -qaiE 'external abort|Unhandled fault|Internal error' "$LOG" \
    && fail "fault during DSI bring-up"

echo "PASS: MIPI-DSI brings up (bridge bound to DPU, rm67191 panel init OK, DSI connector connected with a mode)"
