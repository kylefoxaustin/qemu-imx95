#!/usr/bin/env bash
#
# Full boot: the REAL NXP System Manager (M33) serves the A55's SCMI, and
# Linux boots on top of it.
#
# Loads the SM firmware ELF onto the Cortex-M33 (cpu-num=6) AND a Linux
# kernel + DTB + initramfs on the A55. MU2 traffic flows A55 <-> real SM; the
# SM image is required (there is no built-in SCMI server - the M33 is the
# only thing that answers SCMI).
#
# icount is OFF by default. It is a debug-determinism tool (methodology
# "Pillar 3"), not a normal-boot config: on this heterogeneous A55+M33
# machine, where both sides can sit in WFI between SCMI/MU round-trips,
# icount delays interrupt/timer delivery to idle CPUs, so Linux's
# wait_for_completion_timeout() waits run their full 1 s of virtual time
# instead of completing on the IRQ - inflating the boot ~9x (114 s vs 13 s).
# Enable it only when debugging a race:  ICOUNT=1 tests/swap-boot/run.sh
#
# The four boot artifacts are supplied via env vars (build them per the
# README's "Required artifacts" section). The defaults below are only a
# convenience for the author's layout; on any other machine, set the vars:
#
#   QEMU=./build/qemu-system-aarch64 \
#   SM_ELF=/path/to/m33_image.elf \
#   KERNEL=/path/to/Image DTB=/path/to/imx95-19x19-evk.dtb \
#   INITRD=/path/to/initramfs.cpio.gz \
#       tests/swap-boot/run.sh
#
# Pass extra QEMU args (e.g. -monitor stdio) after the script name.
set -u

QEMU=${QEMU:-./build/qemu-system-aarch64}
SM_ELF=${SM_ELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}
KERNEL=${KERNEL:-$HOME/Documents/linux-imx95-build/arch/arm64/boot/Image}
DTB=${DTB:-$HOME/Documents/linux-imx95-build/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
INITRD=${INITRD:-/tmp/imx95-initramfs/initramfs.cpio.gz}

CMDLINE="earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init"

# Self-documenting artifact check: name the env var to set, not just the path.
need() {  # need VAR "path" "description"
    [ -e "$2" ] && return 0
    echo "error: $3 not found:" >&2
    echo "    $2" >&2
    echo "  set \$$1 to its location (build it per the README's" >&2
    echo "  'Required artifacts' section), e.g. $1=/path/... tests/swap-boot/run.sh" >&2
    exit 1
}
need QEMU   "$QEMU"   "qemu-system-aarch64 (build it: see README 'Building')"
need SM_ELF "$SM_ELF" "System Manager firmware m33_image.elf"
need KERNEL "$KERNEL" "kernel Image"
need DTB    "$DTB"    "device tree imx95-19x19-evk.dtb"
need INITRD "$INITRD" "initramfs cpio.gz"

ICOUNT_ARGS=()
if [ -n "${ICOUNT:-}" ]; then
    ICOUNT_ARGS=(-icount shift=auto)
fi

exec "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    "${ICOUNT_ARGS[@]}" \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$INITRD" \
    -append "$CMDLINE" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -serial mon:stdio -serial null \
    "$@"
