#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 PCIe Root Complex (pcie0, DesignWare) - endpoint enumeration.
#
# Boots Linux with a PCIe endpoint (an Intel e1000e NIC) attached to pcie0's
# downstream bus and confirms the from-scratch DWC RC (hw/pci-host/imx95_pcie.c)
# brings the link up and enumerates the endpoint with its BARs assigned in the
# controller's >4 GiB MEM window:
#
#   imx95 dwc-core driver -> "app" glue (MPLL lock + LTSSM) -> link up
#       -> VIEWPORT iATU -> config window (0x60100000) -> endpoint config
#       -> BAR assigned in the MEM window (0x9_10000000+)
#
# PASS = "PCIe Gen.x x1 link up" + 0000:01:00.0 [8086:10d3] enumerated + a BAR
# assigned above 4 GiB, with no external abort.
#
# Required (override via env): QEMU, KBUILD (Image + dtb), SM_ELF. SKIPs if any
# is missing.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=${IMAGE:-$KBUILD/arch/arm64/boot/Image}
DTB=${DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
INITRD=${INITRD:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
TMO=${TMO:-120}

skip() { echo "SKIP: $*"; exit 0; }
[ -x "$QEMU" ]   || skip "no QEMU ($QEMU)"
[ -e "$IMAGE" ]  || skip "no kernel Image ($IMAGE)"
[ -e "$DTB" ]    || skip "no dtb ($DTB)"
[ -e "$SM_ELF" ] || skip "no SM firmware ($SM_ELF)"
[ -e "$INITRD" ] || skip "no busybox initramfs ($INITRD)"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/root"; mkdir -p "$STAGE"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "=== PCIE ==="
sleep 6
dmesg | grep -aiE '4c300000.pcie.*link up|0000:01:00.0' | head -12
echo "ep-vendor: $(cat /sys/bus/pci/devices/0000:01:00.0/vendor 2>/dev/null)"
echo "=== PCIE-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

LOG="$WORK/serial.log"
timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.cpio.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -device e1000e,bus=imx95-pcie \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "--- pcie report ---"
sed -n '/PCIE ===/,/PCIE-DONE/p' "$LOG" | grep -vE 'PCIE ==='
if grep -qaiE '4c300000.pcie.*link up' "$LOG" \
   && grep -qa '0000:01:00.0' "$LOG" \
   && grep -qaiE '0000:01:00.0.*0x9[0-9a-f]{8}.*assigned' "$LOG" \
   && ! grep -qa 'external abort' "$LOG"; then
    echo "PASS: pcie0 RC link up + e1000e endpoint enumerated, BAR in the >4GiB window"
    exit 0
fi
echo "FAIL: PCIe endpoint did not enumerate as expected"; exit 1
