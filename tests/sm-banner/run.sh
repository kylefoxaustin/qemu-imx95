#!/usr/bin/env bash
#
# v0.6 System Manager (M33) bring-up smoke run.
#
# Loads the real NXP System Manager firmware (m33_image.elf) onto the
# emulated Cortex-M33 and lets it execute. As of v0.6 the SM runs through
# early init and faults on the first unmodelled peripheral (BBNSM at
# 0x44440000, reached from BBNSM_GprSetValue) - QEMU reports this as a
# HardFault lockup. That fault is the EXPECTED v0.6 endpoint; modelling
# the SM-side peripherals it touches is v0.7 work.
#
# Override paths via env: QEMU, SM_ELF.
set -u

QEMU=${QEMU:-./build/qemu-system-aarch64}
SM_ELF=${SM_ELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}

if [ ! -x "$QEMU" ]; then
    echo "qemu not found at $QEMU (build it, or set QEMU=)" >&2
    exit 1
fi
if [ ! -f "$SM_ELF" ]; then
    echo "SM firmware ELF not found at $SM_ELF (build imx-sm, or set SM_ELF=)" >&2
    exit 1
fi

# The M33 is CPU index 6 (after the 6-core A55 cluster). Loading the ELF
# with cpu-num=6 writes its sections into the M33 ITCM/DTCM and sets the
# M33 reset PC to the ELF entry. The SoC releases the M33 from reset only
# because firmware is present in ITCM (a plain boot leaves it halted).
#
# No A55 kernel is loaded here, so this exercises the SM core in isolation.
# The run is expected to end with:
#   qemu: fatal: Lockup: can't escalate 3 to HardFault ...
#   ... R15=1ffc34b6 ...   (PC inside BBNSM_GprSetValue)
exec "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -serial null \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -d unimp,guest_errors
