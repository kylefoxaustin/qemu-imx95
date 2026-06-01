#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# FlexCAN end-to-end validation: boot Linux on the imx95 machine with two
# FlexCAN controllers (flexcan1 + flexcan2) wired to ONE emulated can-bus, so
# the guest sees can0 and can1 on the same bus. A self-contained SocketCAN
# self-test (cantest) brings both interfaces up and sends frames each way,
# proving a real Linux flexcan TX -> bus -> RX round-trip through the model.
#
# Everything is built on the fly; the stock NXP EVK DT (which disables CAN) is
# never modified - a DT overlay enables the two nodes into a temporary DTB.
#
# PASS = the guest prints "FLEXCAN SELFTEST: PASS".
#
# Artifacts (override via env):
#   QEMU       qemu-system-aarch64
#   KERNEL     Linux Image (CONFIG_CAN*/FLEXCAN as modules is fine)
#   DTB        stock imx95-19x19-evk.dtb (overlay is applied to a copy)
#   SM_ELF     System Manager firmware m33_image.elf
#   KBUILD     kernel build tree (for the CAN .ko modules + dtc/fdtoverlay)
#   XGCC       aarch64 cross gcc prefix (default aarch64-linux-gnu-)

set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
HERE="$REPO/tests/flexcan"
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
KBUILD=${KBUILD:-${IMX95_ARTIFACTS:-$HOME/imx95-artifacts}/linux-build}
KERNEL=${KERNEL:-$KBUILD/arch/arm64/boot/Image}
DTB=${DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
SM_ELF=${SM_ELF:-${IMX95_ARTIFACTS:-$HOME/imx95-artifacts}/m33_image.elf}
XGCC=${XGCC:-aarch64-linux-gnu-}
DTC=${DTC:-$KBUILD/scripts/dtc/dtc}
FDTOVERLAY=${FDTOVERLAY:-$KBUILD/scripts/dtc/fdtoverlay}
TMO=${TMO:-120}

need() {
    [ -e "$2" ] && return 0
    echo "error: $3 not found: ${2:-<unset>}" >&2
    echo "  set \$$1 (see tests/flexcan/README.md)" >&2
    exit 1
}
need QEMU       "$QEMU"       "qemu-system-aarch64"
need KERNEL     "$KERNEL"     "kernel Image"
need DTB        "$DTB"        "stock imx95-19x19-evk.dtb"
need SM_ELF     "$SM_ELF"     "System Manager firmware"
need KBUILD     "$KBUILD"     "kernel build tree"
need DTC        "$DTC"        "dtc (kernel scripts/dtc/dtc)"
need FDTOVERLAY "$FDTOVERLAY" "fdtoverlay (kernel scripts/dtc/fdtoverlay)"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# 1. cantest (static aarch64).
make -C "$HERE" TOOLCHAIN="$XGCC" >/dev/null || { echo "cantest build failed"; exit 1; }

# 2. CAN-enabled DTB: compile the overlay, merge onto a copy of the base DTB.
"$DTC" -@ -I dts -O dtb -o "$WORK/ov.dtbo" "$HERE/flexcan-overlay.dtso" 2>/dev/null \
    || { echo "overlay compile failed"; exit 1; }
"$FDTOVERLAY" -i "$DTB" -o "$WORK/can.dtb" "$WORK/ov.dtbo" \
    || { echo "fdtoverlay failed (base DTB lacks __symbols__? build with dtc -@)"; exit 1; }

# 3. Initramfs: busybox + the CAN modules + cantest + an /init that loads the
#    stack, runs the self-test, prints the verdict, and powers off.
ROOT="$WORK/root"
mkdir -p "$ROOT"/{bin,proc,sys,dev,mod}
zcat "$REPO/tests/busybox-initramfs/busybox-initramfs.cpio.gz" \
    | (cd "$ROOT" && cpio -idmu --quiet 'bin/busybox' 2>/dev/null)
[ -x "$ROOT/bin/busybox" ] || { echo "could not extract busybox"; exit 1; }
cp "$HERE/cantest" "$ROOT/bin/cantest"
for ko in net/can/can.ko drivers/net/can/dev/can-dev.ko net/can/can-raw.ko \
          drivers/net/can/flexcan/flexcan.ko; do
    cp "$KBUILD/$ko" "$ROOT/mod/$(basename "$ko")"
done

cat > "$ROOT/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
echo "=== FlexCAN end-to-end test ==="
for m in can can-dev can-raw flexcan; do
    insmod /mod/$m.ko && echo "insmod $m ok" || echo "insmod $m FAILED"
done
/bin/busybox mount -t debugfs none /sys/kernel/debug 2>/dev/null
echo "platform CAN devices:"; ls /sys/bus/platform/devices | grep -i can || echo "  (none)"
echo "deferred devices:"; cat /sys/kernel/debug/devices_deferred 2>/dev/null || echo "  (none)"
echo "CAN interfaces:"; ls /sys/class/net | grep -E '^can' || echo "  (none!)"
cantest can0 can1
echo "=== FlexCAN end-to-end test done ==="
/bin/busybox poweroff -f
INIT
chmod +x "$ROOT/init"
( cd "$ROOT" && find . | cpio -o -H newc --quiet | gzip ) > "$WORK/initrd.cpio.gz"

# 4. Boot: two FlexCANs (canbus0=flexcan1, canbus1=flexcan2) on one bus 'cb'.
SERIAL="$WORK/serial.log"
CMDLINE="earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init"
echo "booting (max ${TMO}s)..."
timeout --signal=KILL "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -kernel "$KERNEL" -dtb "$WORK/can.dtb" -initrd "$WORK/initrd.cpio.gz" \
    -append "$CMDLINE" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -object can-bus,id=cb \
    -machine canbus0=cb,canbus1=cb \
    -serial file:"$SERIAL" -serial null \
    -monitor none >/dev/null 2>&1

# 5. Verdict.
echo "--- guest CAN output ---"
grep -aiE "insmod|CAN interfaces|^can[0-9]|FLEXCAN SELFTEST|cantest:|flexcan|deferred|EPROBE|regulator.*can|clk.*can" "$SERIAL" | sed 's/^/    /'
if grep -aq "FLEXCAN SELFTEST: PASS" "$SERIAL"; then
    echo "=== tests/flexcan PASS ==="
else
    echo "=== tests/flexcan FAILED ==="
    echo "--- serial tail ---"; tail -25 "$SERIAL" | sed 's/^/    /'
    exit 1
fi
