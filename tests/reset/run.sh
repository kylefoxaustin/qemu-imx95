#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# A reset re-runs the boot sequence, including one the guest asks for itself.
#
# Everything that brings this machine up - the emulated boot ROM's walk of the
# image set, the SM going into M33 ITCM, the release of the A55s from CPUWAIT -
# has to happen again on a reset, because a reset powers every core off and
# re-applies its reset vector. When that work was tied to machine-init-done it
# happened exactly once, so the machine came back from a reset with six halted
# cores and no output at all: a guest "reboot" hung the board.
#
# Two resets are checked, because they reach the machine differently:
#
#   guest    Linux asks for it. rdinit=/bin/false exits immediately, which
#            panics the kernel ("Attempted to kill init"), and panic=1 turns
#            that into emergency_restart() -> PSCI SYSTEM_RESET. Nothing is
#            typed at a console, so this cannot pass by accident of input.
#   monitor  QMP system_reset, on the ROM boot chain, which has far more to
#            redo: the ROM read, the SM, SPL, and U-Boot proper.
#
# PASS = the guest's own reset is honoured at least twice (so it is repeatable,
#        not a one-shot), AND a QMP system_reset on the ROM chain comes back
#        through the SM and SPL to a U-Boot prompt.
#
# Required (override via env): QEMU, IMAGE, DTB, INITRD for the guest half;
# SM_BIN, SPL_BIN, UBOOT_BIN for the monitor half. Each half skips without its
# own inputs.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
IMAGE=${IMAGE:-$KBUILD/arch/arm64/boot/Image}
DTB=${DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
INITRD=${INITRD:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
SM_BIN=${SM_BIN:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.bin}
SPL_BIN=${SPL_BIN:-$ROOT/tests/spl-banner/uboot-build/spl/u-boot-spl.bin}
UBOOT_BIN=${UBOOT_BIN:-$ROOT/tests/spl-banner/uboot-build/u-boot.bin}
# Enough wall time for three kernel boots under TCG, and for ROM -> U-Boot.
GUEST_TMO=${GUEST_TMO:-200}
ROM_TMO=${ROM_TMO:-240}
WANT_BOOTS=${WANT_BOOTS:-3}

skip() { echo "SKIP: $*"; exit 0; }
need() { [ -e "$2" ] || skip "missing $1: $2"; }
need QEMU "$QEMU"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
pass=1

# ---------------------------------------------------------------- guest reset
need IMAGE "$IMAGE"; need DTB "$DTB"; need INITRD "$INITRD"
GLOG=$WORK/guest.log
timeout -k 5 "$GUEST_TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -kernel "$IMAGE" -dtb "$DTB" -initrd "$INITRD" \
    -append "earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 \
cpuidle.off=1 rdinit=/bin/false panic=1 mem=1920M" \
    -serial stdio -serial null 2>&1 |
    sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' > "$GLOG" || true

boots=$(grep -ac 'Booting Linux on physical CPU' "$GLOG" || true)
resets=$(grep -ac 'Rebooting in 1 seconds' "$GLOG" || true)
echo "=== guest-requested reset: $resets asked for, $boots kernel boots"
if [ "$boots" -ge "$WANT_BOOTS" ]; then
    echo "  ok   a guest PSCI SYSTEM_RESET reboots the machine, repeatably"
else
    echo "  MISS guest reset ignored (wanted >= $WANT_BOOTS boots, got $boots)"
    pass=0
fi

# -------------------------------------------------------------- monitor reset
need SM_BIN "$SM_BIN"; need SPL_BIN "$SPL_BIN"; need UBOOT_BIN "$UBOOT_BIN"
python3 "$ROOT/tests/uboot/mkbootimg.py" "$UBOOT_BIN" "$WORK/boot.img" \
    --sm "$SM_BIN" --spl "$SPL_BIN" >/dev/null
RLOG=$WORK/rom.log

# Wait for the first U-Boot, reset, then watch the chain come back. The reset
# goes in over QMP, so nothing is typed at the guest's console and the machine
# is not being nudged into life by its own input.
(
    # Give the first boot time to reach U-Boot, then ask for the reset.
    sleep 100
    printf '%s\n' '{"execute":"qmp_capabilities"}' '{"execute":"system_reset"}'
    sleep 3
) | timeout -k 5 "$ROM_TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -drive if=none,format=raw,file="$WORK/boot.img",id=mmc0 \
    -device emmc,drive=mmc0 \
    -qmp stdio -serial "file:$RLOG" -serial null >/dev/null 2>&1 || true

spls=$(grep -ac 'U-Boot SPL' "$RLOG" || true)
ubs=$(grep -ac '^U-Boot 20' "$RLOG" || true)
echo "=== QMP system_reset on the ROM chain: $spls SPL banners, $ubs U-Boot"
if [ "$spls" -ge 2 ] && [ "$ubs" -ge 2 ]; then
    echo "  ok   the ROM walk, the SM, SPL and U-Boot all ran again"
else
    echo "  MISS the chain did not come back (wanted >= 2 of each)"
    pass=0
fi

if [ "$pass" = 1 ]; then
    echo "PASS: reset re-runs the boot sequence, guest-requested and monitor"
    exit 0
fi
echo "FAIL: a reset did not bring the machine back"
exit 1
