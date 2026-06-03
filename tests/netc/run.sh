#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Boot the QEMU i.MX 95 EVK and show how far Linux's enetc4_pf (nxp_enetc4)
# driver gets binding to the modelled NETC ENETC PF. Single-port slirp demo;
# for the two-port back-to-back traffic test on either board see run-2port.sh.
#
# The machine models two PFs - ENETC0 (ethernet@0,0, devfn 0x0) and ENETC1
# (ethernet@8,0, devfn 0x40); patch-dtb.py enables both with a fixed MAC +
# fixed-link (the base nodes carry an efuse-nvmem MAC that never resolves under
# emulation). With the single "-nic user" below, the first PF (ENETC0 = eth0,
# 0002:00:00.0) gets the slirp backend; the second probes but has no backend.
# We hand-patch the decompiled DTS and recompile, rather than using fdtoverlay,
# because fdtoverlay cannot delete the nvmem properties.
#
# Required environment (override as needed):
#   QEMU   - path to qemu-system-aarch64               (default: ../../build/...)
#   KBUILD - kernel build dir with Image + dtb + scripts/dtc/dtc
#   INITRD - busybox initramfs cpio.gz
#   SM_ELF - System Manager m33_image.elf  (legacy SMELF also accepted)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
INITRD=${INITRD:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}

IMAGE=$KBUILD/arch/arm64/boot/Image
DTB=$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb
DTC=$KBUILD/scripts/dtc/dtc

need() { [ -e "$2" ] || { echo "missing $1: $2"; exit 1; }; }
need QEMU "$QEMU"; need IMAGE "$IMAGE"; need DTB "$DTB"
need DTC "$DTC"; need INITRD "$INITRD"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Decompile the base DTB, enable+rewrite both enetc ports, recompile.
"$DTC" -I dtb -O dts "$DTB" >"$WORK/base.dts" 2>/dev/null
python3 "$HERE/patch-dtb.py" "$WORK/base.dts" >"$WORK/netc.dts"
"$DTC" -I dts -O dtb -o "$WORK/netc.dtb" "$WORK/netc.dts" 2>/dev/null

# A backend is attached with "-nic user,model=fsl-enetc"; the SoC wires the PF
# with qemu_configure_nic_device(match_default=true), which matches a -nic (not
# a bare -netdev). slirp gives the guest 10.0.2.15 and a gateway at 10.0.2.2.
echo "Booting (look for 'fsl_enetc4 0002:00:00.0' + ping)..."
timeout --signal=KILL 110 "$QEMU" -M imx95-19x19-evk -m 2G -display none \
	-kernel "$IMAGE" -dtb "$WORK/netc.dtb" -initrd "$INITRD" \
	-append "earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
	-device loader,file="$SM_ELF",cpu-num=6 \
	-nic user,model=fsl-enetc \
	-serial file:"$WORK/serial.log" -serial null -monitor none >/dev/null 2>&1 || true

echo "--- ENETC PF probe + link ---"
grep -aE "fsl_enetc4|0002:00:0[08]|NETCMIX|Link is Up" "$WORK/serial.log" | grep -avi "clk:" || true
echo "--- ping (if the initramfs runs the ping test) ---"
grep -aE "packets transmitted|bytes from 10.0.2" "$WORK/serial.log" || \
	echo "(no ping output - the default busybox initramfs only opens a shell;" \
	     "bring eth0 up with a non-zero MAC and ping 10.0.2.2 to test traffic)"
