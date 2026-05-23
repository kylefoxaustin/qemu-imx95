#!/usr/bin/env bash
#
# v1.x Step 2 M7 unit test.
#
# Boot the imx95-19x19-evk machine with ONLY the Cortex-M7 hello firmware
# loaded onto cpu-num=7 - no Linux, no SM. The M7 should execute its
# reset handler + main, write a 32-bit magic word (0xC0FFEE07) plus the
# ASCII string "CM7 RUNNING\n" to the start of its own DTCM (M7-view
# 0x20000000), observable from the A55/system side via the architectural
# DTCM alias at 0x20400000 - per the upstream Linux imx_rproc_att_imx95_m7
# attribute table. Verify via the QEMU HMP monitor that the writes landed.
#
# The A55 cluster has no kernel loaded here; the cores fault to an
# undefined state. Their behaviour is irrelevant to this test - the
# only assertion is the M7's fingerprint, observed independently
# through HMP.
#
# Override QEMU / CM7_ELF via env vars if installed elsewhere.

set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
CM7_ELF=${CM7_ELF:-$REPO/tests/cm7-hello/cm7_image.elf}

need() {  # need VAR "path" "description"
    [ -e "$2" ] && return 0
    echo "error: $3 not found:" >&2
    echo "    $2" >&2
    echo "  set \$$1, or build it (see the listed README)." >&2
    exit 1
}
need QEMU    "$QEMU"    "qemu-system-aarch64 (build it: see README 'Building')"
need CM7_ELF "$CM7_ELF" "M7 hello firmware (build it: make -C tests/cm7-hello)"

SOCK=$(mktemp -u /tmp/imx95-m7-boot.XXX.sock)
LOG=$(mktemp /tmp/imx95-m7-boot.XXX.log)
PIDFILE=$(mktemp -u /tmp/imx95-m7-boot.XXX.pid)
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

# Launch QEMU daemonized with HMP on a unix socket. No -kernel - this is
# a deliberate M7-only test; the A55 cluster reset state is irrelevant.
"$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -device loader,file="$CM7_ELF",cpu-num=7 \
    -serial file:"$LOG" -serial null \
    -monitor unix:"$SOCK",server,nowait \
    -daemonize -pidfile "$PIDFILE"

# The M7 hello firmware writes its fingerprint within its first few
# hundred instructions after the machine-init-done BH releases it. A
# 3-second wall-clock delay is substantially longer than the M7 needs
# and provides margin against host scheduling jitter / cache warm-up.
sleep 3

# Peek the fingerprint via HMP: one 32-bit word for the magic, then 16
# bytes for the ASCII string. nc closes when the monitor processes the
# quit command.
RESULT=$( { echo "xp /1wx 0x20400000"
            echo "xp /16cb 0x20400004"
            sleep 1
            echo "quit"
          } | nc -U -q 2 "$SOCK" 2>&1 )

if echo "$RESULT" | grep -qE '0x[cC]0[fF][fF][eE][eE]07'; then
    echo "PASS: M7 fingerprint 0xC0FFEE07 detected at 0x20400000"
    # Show the ASCII string for the human reader.
    echo "$RESULT" | grep -E "^[0-9a-f]+: *'" | head -2
else
    echo "FAIL: M7 fingerprint NOT detected at 0x20400000"
    echo "--- HMP response (debug) ---"
    echo "$RESULT" | tail -30
    exit 1
fi
echo "=== tests/m7-boot OK ==="
