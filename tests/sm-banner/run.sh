#!/usr/bin/env bash
#
# System Manager (M33) bring-up smoke run.
#
# Loads the real NXP System Manager firmware (m33_image.elf) onto the
# emulated Cortex-M33 and lets it execute. As of v0.8 the SM runs through
# early init, PMIC, and power/clock bring-up, prints its banner, and drops
# to its interactive debug monitor prompt on its console (LPUART2).
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
#
# The SM debug console is LPUART2 (BOARD_DEBUG_UART_INSTANCE=2), wired to
# serial_hd(1) - so the FIRST -serial is the A55 console (LPUART1, unused
# here) and the SECOND -serial is the SM console. The SM prints its banner
# and drops to its debug monitor prompt:
#
#   Hello from SM (Build NNN, Commit ........, ...)
#   *** SM Debug Monitor ***
#   >$
exec "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -serial null -serial mon:stdio \
    -device loader,file="$SM_ELF",cpu-num=6
