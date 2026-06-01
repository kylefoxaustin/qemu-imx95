#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# v1.x Step 3 integration test: M7-first reset sequencing (Scenario 1).
#
# Same boot artifacts as parallel-boot (A55 + M33 + M7), but additionally
# verifies the SM-driven M7 release path. Step 2's parallel-boot proves
# only that the M7 ran (via the machine-init-done BH that releases M7 if
# its ITCM has firmware staged); it cannot tell you whether the silicon-
# faithful path - the SM's DEV_SM_CpuStart(M7) writing
# SRC_GEN.SCR.BOOT_RESET_RELEASE_M7MIX (bit 12 at 0x44460010) - also
# fired. Both paths reach the same end state by design; Step 3's job is
# to assert both fired, proving the SM-driven wiring works.
#
# Asserted signals (each its own PASS/FAIL line):
#
#   1. SCMI handshake (SM came up; A55 <-> M33 mailbox working).
#   2. Linux PID-1 banner (A55 cluster + initramfs reached userspace).
#   3. M7 fingerprint at 0x20400000 (M7 actually executed cm7_image.elf).
#   4. SRC_GEN.SCR bit 12 = 1 at 0x44460010 (the SM's CpuStart(M7) wrote
#      the M7MIX-release latch; sticky/locked per the i.MX 95 RM, so it
#      will read 1 forever after the first SM-side write). THIS IS THE
#      NEW STEP-3 SIGNAL - parallel-boot doesn't check it.
#   5. Ordering: M7 fingerprint is visible BEFORE Linux reaches its
#      PID-1 banner. Polled in parallel during boot, wall-clock-timed
#      on the host. Caveat: with both release paths active (the
#      machine-init-done BH AND the SM-driven SRC.SCR write), the BH
#      path likely fires first - so this assertion really proves "M7
#      came up well before A55", which is what Scenario 1 wants. To
#      isolate the SM-driven path you'd disable the BH (future work).
#
# A "Scenario 1 vs Step 2" failure surfaces as #4 missing (the SM never
# wrote the bit, even though M7 ran via the BH fallback). A regression
# in v1.0 surfaces as #1 or #2 missing. A regression in v1.x Step 2
# surfaces as #3 missing. A boot-ordering regression surfaces as #5.

set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
SM_ELF=${SM_ELF:-${IMX95_ARTIFACTS:-$HOME/imx95-artifacts}/m33_image.elf}
KERNEL=${KERNEL:-${IMX95_ARTIFACTS:-$HOME/imx95-artifacts}/linux-build/arch/arm64/boot/Image}
DTB=${DTB:-${IMX95_ARTIFACTS:-$HOME/imx95-artifacts}/linux-build/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
INITRD=${INITRD:-$REPO/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
CM7_ELF=${CM7_ELF:-$REPO/tests/cm7-hello/cm7_image.elf}

need() {
    [ -e "$2" ] && return 0
    echo "error: $3 not found:" >&2
    echo "    $2" >&2
    echo "  set \$$1 (see README 'Required artifacts'), e.g." >&2
    echo "    $1=/path/... tests/m7-first/run.sh" >&2
    exit 1
}
need QEMU    "$QEMU"    "qemu-system-aarch64 (build it: see README 'Building')"
need SM_ELF  "$SM_ELF"  "System Manager firmware m33_image.elf"
need KERNEL  "$KERNEL"  "kernel Image"
need DTB     "$DTB"     "device tree imx95-19x19-evk.dtb"
need INITRD  "$INITRD"  "initramfs cpio.gz (build: tests/busybox-initramfs/build.sh)"
need CM7_ELF "$CM7_ELF" "M7 hello firmware (build: make -C tests/cm7-hello)"

SOCK=$(mktemp -u /tmp/imx95-m7-first.XXX.sock)
LOG=$(mktemp /tmp/imx95-m7-first.XXX.log)
PIDFILE=$(mktemp -u /tmp/imx95-m7-first.XXX.pid)
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

# Parallel poll for the two ordering events: M7 fingerprint visible
# via HMP `xp`, and Linux userspace banner visible in the serial log.
# Both deadlines run on the same 60 s window; record the first-seen
# wall-clock timestamp for each (high-resolution via date +%s.%N).
#
# nc -q 1 closes the client end after 1 s of stdin idle; we don't
# send HMP "quit" here because we still need the monitor after this
# loop for the final fingerprint + SCR readback.
echo "polling for M7 fingerprint + Linux userspace (up to 60s)..."
M7_TS=
USERSPACE_TS=
deadline=$(( $(date +%s) + 60 ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if [ -z "$M7_TS" ]; then
        out=$(echo "xp /1wx 0x20400000" \
              | timeout 2 nc -U -q 1 "$SOCK" 2>&1 || true)
        if echo "$out" | grep -qE '20400000: 0x[cC]0[fF][fF][eE][eE]07'; then
            M7_TS=$(date +%s.%N)
        fi
    fi
    if [ -z "$USERSPACE_TS" ] && grep -q "=== imx95 busybox userspace ===" "$LOG" 2>/dev/null; then
        USERSPACE_TS=$(date +%s.%N)
    fi
    [ -n "$M7_TS" ] && [ -n "$USERSPACE_TS" ] && break
    sleep 0.5
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
    echo "FAIL: Linux did NOT reach userspace"
    fail=1
fi

# HMP peeks: M7 fingerprint + the SRC_GEN.SCR latch.
HMP_RESULT=$( { echo "xp /1wx 0x20400000"
                echo "xp /1wx 0x44460010"
                sleep 1
                echo "quit"
              } | nc -U -q 2 "$SOCK" 2>&1 )

if echo "$HMP_RESULT" | grep -qE '20400000: 0x[cC]0[fF][fF][eE][eE]07'; then
    echo "PASS: M7 fingerprint 0xC0FFEE07 detected at 0x20400000"
else
    echo "FAIL: M7 fingerprint NOT detected at 0x20400000"
    echo "  HMP returned: $(echo "$HMP_RESULT" | grep '20400000:' || echo '<no match>')"
    fail=1
fi

# Step-3 signal: SRC_GEN.SCR (0x44460010) bit 12 must be set if the SM
# actually called DEV_SM_CpuStart(M7) and our SRC model honored the
# write. Bit 12 = 0x1000 in the register value; the bit is sticky per
# the i.MX 95 RM so any "is it set now?" check is sufficient.
#
# The HMP monitor echoes the typed command back with terminal-control
# bytes, and that echoed text contains the address string we're looking
# for. The actual data line is "44460010: 0xVALUE" (no control bytes
# before "44460010"). Use a regex that anchors on that exact pattern,
# take the LAST match in case multiple show, and validate it's a hex
# literal before doing arithmetic.
SCR=$(echo "$HMP_RESULT" \
      | grep -oE '44460010: 0x[0-9a-fA-F]+' \
      | tail -1 \
      | awk '{print $NF}')
case "$SCR" in
    0x[0-9a-fA-F]*)
        if [ $(( SCR & 0x1000 )) -ne 0 ]; then
            echo "PASS: SRC_GEN.SCR.M7MIX_RELEASE bit set (SM-driven M7 release fired); SCR=$SCR"
        else
            echo "FAIL: SRC_GEN.SCR.M7MIX_RELEASE bit NOT set; SCR=$SCR"
            echo "      (SM's DEV_SM_CpuStart(M7) did not write the M7MIX release latch,"
            echo "       or our SRC model isn't honoring the write — see hw/misc/imx95_src.c"
            echo "       and the SRC<->M7 wiring in hw/arm/fsl-imx95.c.)"
            fail=1
        fi
        ;;
    *)
        echo "FAIL: SRC_GEN.SCR readback parse failed; got: '${SCR:-<no match>}'"
        fail=1
        ;;
esac

# Ordering assertion (Scenario 1): M7 must come up before Linux PID-1.
# Both timestamps captured by the parallel-poll loop above. awk -v
# arithmetic handles the sub-second float comparison portably.
if [ -z "$M7_TS" ]; then
    echo "FAIL: M7 fingerprint was never observed during the 60 s ordering poll"
    fail=1
elif [ -z "$USERSPACE_TS" ]; then
    echo "FAIL: Linux userspace banner was never observed during the 60 s ordering poll"
    fail=1
else
    delta=$(awk -v u="$USERSPACE_TS" -v m="$M7_TS" 'BEGIN{printf "%.2f", u - m}')
    if awk -v u="$USERSPACE_TS" -v m="$M7_TS" 'BEGIN{exit !(m < u)}'; then
        echo "PASS: M7 came up before Linux userspace (delta=${delta}s; m7=$M7_TS, userspace=$USERSPACE_TS)"
    else
        echo "FAIL: ordering violated - M7 fingerprint observed AFTER Linux userspace"
        echo "      m7_ts=$M7_TS userspace_ts=$USERSPACE_TS delta=${delta}s"
        fail=1
    fi
fi

if [ "$fail" -eq 0 ]; then
    echo "=== Scenario 1 (M7-first) verified: both M7 release paths fired, M7-before-A55 ordering held ==="
else
    echo "=== tests/m7-first FAILED ==="
    exit 1
fi
