#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Boot the QEMU i.MX 95 EVK with the modelled NETC ENETC PF and show how far
# Linux's enetc4_pf (nxp_enetc4) driver gets binding to it.
#
# The modelled PF is placed at PCI devfn 08.0 (= ENETC1, ethernet@8,0 in the
# BSP DT). That slot avoids gpex's own root bridge at 00.0 and is accepted by
# the netc-blk-ctrl NETCMIX init (which only handles ENETC0=0x0 / ENETC1=0x40).
# patch-dtb.py rewrites that node to be enabled with a fixed MAC + fixed-link
# (the base node is disabled and carries an efuse-nvmem MAC that never resolves
# under emulation). We hand-patch the decompiled DTS and recompile, rather than
# using fdtoverlay, because fdtoverlay cannot delete the nvmem properties.
#
# Required environment (override as needed):
#   QEMU   - path to qemu-system-aarch64               (default: ../../build/...)
#   KBUILD - kernel build dir with Image + dtb + scripts/dtc/dtc
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

need() { [ -e "$2" ] || { echo "missing $1: $2"; exit 1; }; }
need QEMU "$QEMU"; need IMAGE "$IMAGE"; need DTB "$DTB"
need DTC "$DTC"; need INITRD "$INITRD"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Decompile the base DTB, enable+rewrite ethernet@8,0, recompile.
"$DTC" -I dtb -O dts "$DTB" >"$WORK/base.dts" 2>/dev/null
python3 "$HERE/patch-dtb.py" "$WORK/base.dts" >"$WORK/netc.dts"
"$DTC" -I dts -O dtb -o "$WORK/netc.dtb" "$WORK/netc.dts" 2>/dev/null

echo "Booting (look for 'fsl_enetc4 0002:00:08.0')..."
timeout --signal=KILL 110 "$QEMU" -M imx95-19x19-evk -m 2G -display none \
	-kernel "$IMAGE" -dtb "$WORK/netc.dtb" -initrd "$INITRD" \
	-append "earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
	-device loader,file="$SMELF",cpu-num=6 \
	-serial file:"$WORK/serial.log" -serial null -monitor none >/dev/null 2>&1 || true

echo "--- ENETC PF probe lines ---"
grep -aE "fsl_enetc4|0002:00:08|enetc|NETCMIX" "$WORK/serial.log" | grep -avi "clk:" || true
