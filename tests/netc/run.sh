#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Boot the QEMU i.MX 95 EVK with the modelled NETC ENETC0 PF and check that
# Linux's enetc4_pf (nxp_enetc4) driver binds to it.
#
# The base BSP DTB maps ENETC0 to PCI devfn 00.0, where QEMU's gpex parks its
# own root bridge, and its ethernet nodes carry efuse-nvmem MAC dependencies
# that never resolve under emulation. We therefore place the modelled PF at
# devfn 02.0 and merge a small overlay (enetc-overlay.dtso) that:
#   - adds an available ethernet@2,0 node with a fixed MAC + fixed-link, and
#   - drops "fsl,enetc-ptp-timer" from the timer node so enetc4_pf's
#     module_init (ntmp_init) does not -EPROBE_DEFER on the unmodelled PTP
#     timer and fails to register the PCI driver.
#
# Required environment (override as needed):
#   QEMU   - path to qemu-system-aarch64               (default: ../../build/...)
#   KBUILD - kernel build dir with Image + dtb + scripts/dtc/{dtc,fdtoverlay}
#   INITRD - busybox initramfs cpio.gz
#   SMELF  - System Manager m33_image.elf
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
INITRD=${INITRD:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
SMELF=${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}

IMAGE=$KBUILD/arch/arm64/boot/Image
DTB=$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb
DTC=$KBUILD/scripts/dtc/dtc
FDTOVERLAY=$KBUILD/scripts/dtc/fdtoverlay

need() { [ -e "$2" ] || { echo "missing $1: $2"; exit 1; }; }
need QEMU "$QEMU"; need IMAGE "$IMAGE"; need DTB "$DTB"
need DTC "$DTC"; need FDTOVERLAY "$FDTOVERLAY"; need INITRD "$INITRD"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$DTC" -@ -I dts -O dtb -o "$WORK/ov.dtbo" "$HERE/enetc-overlay.dtso" 2>/dev/null
"$FDTOVERLAY" -i "$DTB" -o "$WORK/netc.dtb" "$WORK/ov.dtbo"

echo "Booting (look for 'nxp_enetc4 0002:00:02.0')..."
timeout --signal=KILL 60 "$QEMU" -M imx95-19x19-evk -m 2G -display none \
	-kernel "$IMAGE" -dtb "$WORK/netc.dtb" -initrd "$INITRD" \
	-append "earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
	-device loader,file="$SMELF",cpu-num=6 \
	-serial file:"$WORK/serial.log" -serial null -monitor none >/dev/null 2>&1 || true

echo "--- ENETC PF probe lines ---"
grep -aE "nxp_enetc4|0002:00:02|enetc" "$WORK/serial.log" | grep -avi "clk:" || true
