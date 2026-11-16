#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# A55 CPU hotplug under the EL3 firmware chain.
#
# Linux offlines a secondary and brings it back. With QEMU's own PSCI that
# works; with el3-firmware=on, PSCI is answered by TF-A BL31 instead, and it
# does not: the offline reports
#
#   psci: CPUn may not have shut down cleanly (AFFINITY_INFO reports 0)   -110
#
# and the matching online is then refused
#
#   psci: failed to boot CPUn (-22)
#
# because BL31 still believes the core is ON and so treats CPU_ON as
# ALREADY_ON. This harness asserts the behaviour we want, so IT CURRENTLY
# FAILS. That is deliberate: see README.md for the measurement that located the
# fault, and in particular for why it is NOT a gap in this model's SRC/GPC
# wiring - the one thing the machine is routinely blamed for here.
#
# PASS = every secondary offlines cleanly (no AFFINITY_INFO warning) and comes
#        back online, leaving the full complement of CPUs.
#
# EL3=off runs the SAME sweep on the SAME U-Boot chain with QEMU's own PSCI
# instead of BL31, and PASSES. That is this harness's control: it is what says
# the failure above is the EL3 firmware path and not the sweep, the initramfs,
# or this script - and what stops a permanently-red test from being mistaken
# for one that cannot go green.
#
# Required (override via env): QEMU, IMAGE, DTB, INITRD, SM_BIN, SPL_BIN,
# UBOOT_BIN, UBOOT_ELF, and for EL3=on also BL31_BIN and OPTEE_BIN. BL31 and
# OP-TEE are NXP BSP artifacts and are not redistributable, so the EL3 run
# skips without them.
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
UBOOT_ELF=${UBOOT_ELF:-$ROOT/tests/spl-banner/uboot-build/u-boot}
BL31_BIN=${BL31_BIN:-}
OPTEE_BIN=${OPTEE_BIN:-}
NCPU=${NCPU:-6}
TMO=${TMO:-600}
EL3=${EL3:-on}

skip() { echo "SKIP: $*"; exit 0; }
need() { [ -n "$2" ] || skip "set $1"; [ -e "$2" ] || skip "missing $1: $2"; }
need QEMU "$QEMU";         need IMAGE "$IMAGE";       need DTB "$DTB"
need INITRD "$INITRD";     need SM_BIN "$SM_BIN";     need SPL_BIN "$SPL_BIN"
need UBOOT_BIN "$UBOOT_BIN"; need UBOOT_ELF "$UBOOT_ELF"
if [ "$EL3" = on ]; then
    need BL31_BIN "$BL31_BIN"; need OPTEE_BIN "$OPTEE_BIN"
    MACH=imx95-19x19-evk,el3-firmware=on
    # The EL3 chain carries OP-TEE, so U-Boot's optee fixup is left alone.
    FW_ARGS="--bl31 $BL31_BIN --optee $OPTEE_BIN"
else
    MACH=imx95-19x19-evk
    # No BL32 in this image, so neutralise the optee reserved-memory fixup,
    # exactly as tests/uboot/run-linux.sh documents.
    FW_ARGS="--no-optee"
fi

W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
T="$W/tftp"; mkdir -p "$T"
cp "$IMAGE" "$T/Image"; cp "$DTB" "$T/imx95.dtb"
LOG="$W/ap.log"

rr="$W/rr"; mkdir -p "$rr"
zcat "$INITRD" | (cd "$rr" && cpio -idmu 2>/dev/null)
cat > "$rr/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc 2>/dev/null
/bin/busybox mount -t sysfs sysfs /sys 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "HOTPLUG-ALIVE"
# Each secondary in turn, so one core's behaviour is never mistaken for the
# machine's. A core that refuses to go offline is as much a failure as one
# that refuses to come back.
for c in 1 2 3 4 5; do
    echo 0 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null
    echo "HOTPLUG cpu$c offline online=$(cat /sys/devices/system/cpu/online)"
    echo 1 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null
    echo "HOTPLUG cpu$c online  online=$(cat /sys/devices/system/cpu/online)"
done
echo "HOTPLUG-COUNT: $(/bin/busybox grep -c ^processor /proc/cpuinfo)"
echo "HOTPLUG-DONE"
/bin/busybox poweroff -f
INIT
chmod +x "$rr/init"
( cd "$rr" && find . | cpio -o -H newc 2>/dev/null | gzip ) \
    > "$T/rootfs.cpio.gz"
RD_SZ=$(printf '%x' "$(stat -c%s "$T/rootfs.cpio.gz")")

python3 "$ROOT/tests/uboot/mkbootimg.py" "$UBOOT_BIN" "$W/boot.img" \
    --sm "$SM_BIN" --spl "$SPL_BIN" $FW_ARGS \
    --uboot-elf "$UBOOT_ELF" >/dev/null

drive() {
    for _ in $(seq 1 40); do printf '\n'; sleep 0.25; done
    sleep 2
    printf 'setenv ipaddr 10.0.2.15\n';          sleep 1
    printf 'setenv serverip 10.0.2.2\n';         sleep 1
    printf 'setenv ethaddr 00:04:9f:06:11:22\n'; sleep 1
    printf 'setenv bootargs earlycon=lpuart32,mmio32,0x44380010 '\
'console=ttyLP0,115200 cpuidle.off=1 rdinit=/init mem=1600M\n'; sleep 1
    printf 'tftpboot 0x93000000 imx95.dtb\n';    sleep 6
    printf 'tftpboot 0x94000000 rootfs.cpio.gz\n'; sleep 8
    printf 'tftpboot 0x90400000 Image\n';        sleep 45
    printf 'booti 0x90400000 0x94000000:%s 0x93000000\n' "$RD_SZ"; sleep 90
}

drive | timeout -k 5 "$TMO" "$QEMU" -M "$MACH" \
    -m 2G -display none \
    -drive if=none,format=raw,file="$W/boot.img",id=mmc0 \
    -device emmc,drive=mmc0 \
    -nic user,model=fsl-enetc,tftp="$T" \
    -serial stdio -serial null 2>&1 \
    | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' > "$LOG" || true

echo "=== hotplug report (EL3=$EL3) ==="
grep -aE "HOTPLUG|psci:|may not have shut down" "$LOG" | head -40

fail=0
note() { echo "  FAIL $*"; fail=1; }
ok()   { echo "  ok   $*"; }

if ! grep -aq "HOTPLUG-ALIVE" "$LOG"; then
    echo "FAIL: Linux never reached userspace"
    exit 1
fi
grep -aq "SMP: Total of $NCPU processors activated" "$LOG" \
    && ok "all $NCPU A55s online at boot" \
    || note "boot did not bring up $NCPU A55s"

for c in 1 2 3 4 5; do
    if grep -aq "CPU$c may not have shut down cleanly" "$LOG"; then
        note "cpu$c: never reported OFF (AFFINITY_INFO stayed ON)"
    else
        ok "cpu$c offlined cleanly"
    fi
    if grep -aq "failed to boot CPU$c" "$LOG"; then
        note "cpu$c: CPU_ON refused after the offline"
    else
        ok "cpu$c came back online"
    fi
done

if grep -aq "HOTPLUG-COUNT: $NCPU" "$LOG"; then
    ok "userspace still sees $NCPU CPUs afterwards"
else
    seen=$(grep -a HOTPLUG-COUNT "$LOG" || true)
    note "CPU count after the sweep is not $NCPU ($seen)"
fi

if [ "$fail" = 0 ]; then
    echo "PASS: every A55 secondary offlines and comes back (EL3=$EL3)"
elif [ "$EL3" = on ]; then
    echo "FAIL: A55 hotplug is broken under the EL3 chain (see README.md);" \
         "re-run with EL3=off for the control that this sweep can pass"
    exit 1
else
    echo "FAIL: A55 hotplug is broken even with QEMU's own PSCI - that is a" \
         "regression in this model, not the EL3 firmware gap in README.md"
    exit 1
fi
