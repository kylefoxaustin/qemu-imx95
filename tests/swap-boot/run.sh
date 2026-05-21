#!/usr/bin/env bash
#
# v0.9 SCMI-swap full boot: the REAL NXP System Manager (M33) serves the
# A55's SCMI, and Linux boots on top of it.
#
# Loads the SM firmware ELF onto the Cortex-M33 (cpu-num=6) AND a Linux
# kernel + DTB + initramfs on the A55, with the C-stub SCMI server disabled
# (`-global fsl-imx95.scmi-stub=false`) so MU2 traffic flows A55 <-> real SM.
#
# -icount shift=auto keeps MU/timer interaction deterministic (reflex #4).
#
# Override paths via env: QEMU, SM_ELF, KERNEL, DTB, INITRD.
# Pass extra args (e.g. -monitor stdio) after the script name.
set -u

QEMU=${QEMU:-./build/qemu-system-aarch64}
SM_ELF=${SM_ELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}
KERNEL=${KERNEL:-$HOME/Documents/linux-imx95-build/arch/arm64/boot/Image}
DTB=${DTB:-$HOME/Documents/linux-imx95-build/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
INITRD=${INITRD:-/tmp/imx95-initramfs/initramfs.cpio.gz}

CMDLINE="earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init"

for f in "$QEMU:exec" "$SM_ELF" "$KERNEL" "$DTB" "$INITRD"; do
    path=${f%:exec}
    [ -e "$path" ] || { echo "missing: $path" >&2; exit 1; }
done

exec "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -icount shift=auto \
    -global fsl-imx95.scmi-stub=false \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$INITRD" \
    -append "$CMDLINE" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -serial mon:stdio -serial null \
    "$@"
