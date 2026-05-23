#!/usr/bin/env bash
#
# v1.x Step 2 integration test: A55 + M33 + M7 in parallel.
#
# Boot all three CPU complements together under the default reset
# policy ("all CPUs released at machine-init-done if their respective
# firmware was loaded"): the A55 cluster runs Linux via the real NXP
# SM as in v1.0, and the Cortex-M7 runs the standalone hello firmware
# alongside. The boot does NOT serialise the cores - the M7 hello
# finishes in microseconds while Linux is still bringing the A55
# cluster up.
#
# Three independent assertions, each with its own PASS/FAIL line so
# the failure mode is visible without having to dig through the boot
# log:
#
#   1. SCMI handshake against the real SM
#      ("SCMI Protocol v2.1 'NXP:IMX' Firmware version 0x333")
#      proves the A55 <-> M33 mailbox cross-connect is working.
#
#   2. Linux PID 1 banner ("=== imx95 busybox userspace ===" from
#      the committed initramfs's /init) proves Linux reached
#      userspace.
#
#   3. M7 fingerprint at 0x20400000 (0xC0FFEE07 magic word, observed
#      via HMP) proves the M7 instance + loader + release path ran
#      end-to-end.
#
# Same artifacts as tests/swap-boot/run.sh, plus the cm7 hello ELF.

set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
SM_ELF=${SM_ELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}
KERNEL=${KERNEL:-$HOME/Documents/linux-imx95-build/arch/arm64/boot/Image}
DTB=${DTB:-$HOME/Documents/linux-imx95-build/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
INITRD=${INITRD:-$REPO/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
CM7_ELF=${CM7_ELF:-$REPO/tests/cm7-hello/cm7_image.elf}

need() {
    [ -e "$2" ] && return 0
    echo "error: $3 not found:" >&2
    echo "    $2" >&2
    echo "  set \$$1 to its location (see the README 'Required artifacts'" >&2
    echo "  section), e.g. $1=/path/... tests/parallel-boot/run.sh" >&2
    exit 1
}
need QEMU    "$QEMU"    "qemu-system-aarch64 (build it: see README 'Building')"
need SM_ELF  "$SM_ELF"  "System Manager firmware m33_image.elf"
need KERNEL  "$KERNEL"  "kernel Image"
need DTB     "$DTB"     "device tree imx95-19x19-evk.dtb"
need INITRD  "$INITRD"  "initramfs cpio.gz (build: tests/busybox-initramfs/build.sh)"
need CM7_ELF "$CM7_ELF" "M7 hello firmware (build: make -C tests/cm7-hello)"

SOCK=$(mktemp -u /tmp/imx95-parallel.XXX.sock)
LOG=$(mktemp /tmp/imx95-parallel.XXX.log)
PIDFILE=$(mktemp -u /tmp/imx95-parallel.XXX.pid)
cleanup() {
    local p
    if [ -e "$PIDFILE" ]; then
        p=$(cat "$PIDFILE" 2>/dev/null)
        if [ -n "$p" ]; then
            kill "$p" 2>/dev/null || true
            sleep 1
            kill -0 "$p" 2>/dev/null && kill -9 "$p" 2>/dev/null || true
        fi
    fi
    rm -f "$SOCK" "$PIDFILE"
}
trap cleanup EXIT

CMDLINE="earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init"

"$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$INITRD" \
    -append "$CMDLINE" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -device loader,file="$CM7_ELF",cpu-num=7 \
    -serial file:"$LOG" -serial null \
    -monitor unix:"$SOCK",server,nowait \
    -daemonize -pidfile "$PIDFILE"

# The M7 finishes its hello within microseconds of machine-init-done; the
# A55 path needs ~13-15s to bring Linux through to /init. Wait for the
# slowest signal (Linux userspace) up to 60s.
echo "waiting for Linux userspace (up to 60s)..."
deadline=$(( $(date +%s) + 60 ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    grep -q "=== imx95 busybox userspace ===" "$LOG" 2>/dev/null && break
    sleep 1
done

fail=0

if grep -q "'NXP:IMX' Firmware version 0x333" "$LOG"; then
    echo "PASS: SCMI banner detected (A55 <-> M33 mailbox working)"
else
    echo "FAIL: SCMI banner NOT detected"
    fail=1
fi

if grep -q "=== imx95 busybox userspace ===" "$LOG"; then
    echo "PASS: Linux reached userspace (A55 cluster + initramfs OK)"
else
    echo "FAIL: Linux did NOT reach userspace (busybox /init banner missing)"
    fail=1
fi

# HMP peek for the M7 fingerprint.
HMP_RESULT=$( { echo "xp /1wx 0x20400000"; sleep 1; echo "quit"
              } | nc -U -q 2 "$SOCK" 2>&1 )
if echo "$HMP_RESULT" | grep -qE '0x[cC]0[fF][fF][eE][eE]07'; then
    echo "PASS: M7 fingerprint 0xC0FFEE07 detected at 0x20400000"
else
    echo "FAIL: M7 fingerprint NOT detected at 0x20400000"
    echo "  HMP returned: $(echo "$HMP_RESULT" | grep '88200000:' || echo '<no match>')"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "=== All three CPU complements verified: A55+M33+M7 parallel boot OK ==="
else
    echo "=== tests/parallel-boot FAILED ==="
    exit 1
fi
