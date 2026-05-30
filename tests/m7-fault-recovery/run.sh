#!/usr/bin/env bash
#
# v1.x Step 5: SM-orchestrated M7 fault recovery (CPU stop/start cycle).
#
# Boots the M33 System Manager + the M7 fault fixture (cm7_fault.elf), no
# A55 cluster. The M7 deliberately faults itself on its first boot by
# asserting SYSRESETREQ. On silicon (and now in this model) the SRC routes
# that to the SM as CM7_SYSRESETREQ_IRQn; the SM, for the M7 LM
# (reaction=lm_reset on mx95evk), cold-resets the M7 LM - stopping then
# starting the M7 via the SRC M7 mix-slice. Our SRC model turns the
# slice power-down/up into a real halt + reset + resume of the M7 core.
#
# The fixture keeps a boot counter in DTCM (survives the core reset). The
# A55-side observer reads it via the system-view DTCM alias:
#
#   0x20400000  magic 0xC0FFEE07   (M7 ran at all)
#   0x20400040  boot counter       (2 == M7 faulted AND the SM restarted it)
#
# PASS = counter reaches 2. With no SM the M7 would fault once and never
# come back (counter stuck at 1), so a 2 proves the real SM drove the
# recovery, not a host-side cheat.
#
# Related tests:
#   tests/m33-m7-only -> M33 + M7 first boot (no fault)
#   tests/m7-first    -> M33 + A55 + M7, asserts SM-driven SRC.SCR release

set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
SM_ELF=${SM_ELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}
CM7_ELF=${CM7_ELF:-$REPO/tests/cm7-hello/cm7_fault.elf}

need() {
    [ -e "$2" ] && return 0
    echo "error: $3 not found:" >&2
    echo "    $2" >&2
    echo "  set \$$1 (see tests/README) e.g.  $1=/path/... tests/m7-fault-recovery/run.sh" >&2
    exit 1
}
need QEMU    "$QEMU"    "qemu-system-aarch64 (build it: see README 'Building')"
need SM_ELF  "$SM_ELF"  "System Manager firmware m33_image.elf"
need CM7_ELF "$CM7_ELF" "M7 fault fixture (build: make -C tests/cm7-hello cm7_fault.elf)"

A55_LOG=$(mktemp /tmp/imx95-m7fr-a55.XXX.log)
SM_LOG=$(mktemp /tmp/imx95-m7fr-sm.XXX.log)
SOCK=$(mktemp -u /tmp/imx95-m7fr.XXX.sock)
PIDFILE=$(mktemp -u /tmp/imx95-m7fr.XXX.pid)

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
    rm -f "$SOCK" "$PIDFILE" "$A55_LOG" "$SM_LOG"
}
trap cleanup EXIT

"$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -serial file:"$A55_LOG" -serial file:"$SM_LOG" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -device loader,file="$CM7_ELF",cpu-num=7 \
    -monitor unix:"$SOCK",server,nowait \
    -daemonize -pidfile "$PIDFILE"

# Read the DTCM boot counter at the A55 system-view alias.
read_bootcount() {
    { echo "xp /1wx 0x20400040"; sleep 1; } | nc -U -q 2 "$SOCK" 2>&1 \
        | grep -oE '20400040: 0x[0-9a-fA-F]+' | tail -1 | awk '{print $NF}'
}

# The M7 needs the SM up (~5 s), then a short delay, then SYSRESETREQ, then
# the SM's cold-reset cycle. Poll the counter up to 40 s for it to reach 2.
echo "waiting for M7 fault + SM recovery (counter -> 2, up to 40 s)..."
deadline=$(( $(date +%s) + 40 ))
count=""
while [ "$(date +%s)" -lt "$deadline" ]; do
    count=$(read_bootcount)
    case "$count" in
        0x[0-9a-fA-F]*) [ $(( count )) -ge 2 ] && break ;;
    esac
    sleep 2
done

fail=0

if grep -qE "Hello from SM|SM Debug Monitor" "$SM_LOG"; then
    echo "PASS: SM banner detected on LPUART2 (M33 came up)"
else
    echo "FAIL: SM banner NOT detected on LPUART2"
    tail -20 "$SM_LOG" 2>/dev/null | sed 's/^/    /'
    fail=1
fi

# Magic confirms the M7 executed at least once.
MAGIC=$( { echo "xp /1wx 0x20400000"; sleep 1; } | nc -U -q 2 "$SOCK" 2>&1 \
         | grep -oE '20400000: 0x[0-9a-fA-F]+' | tail -1 | awk '{print $NF}')
if echo "$MAGIC" | grep -qiE '0xc0ffee07'; then
    echo "PASS: M7 fingerprint 0xC0FFEE07 present (M7 executed)"
else
    echo "FAIL: M7 fingerprint missing; got '${MAGIC:-<no match>}'"
    fail=1
fi

# The headline assertion: the counter reached 2.
case "$count" in
    0x[0-9a-fA-F]*)
        if [ $(( count )) -ge 2 ]; then
            echo "PASS: M7 boot counter = $count (M7 faulted, SM cold-restarted it)"
        elif [ $(( count )) -eq 1 ]; then
            echo "FAIL: M7 boot counter stuck at 1 - the M7 faulted but was NOT"
            echo "      restarted (SM fault-recovery / SRC M7 mix-slice cycle did"
            echo "      not complete). See hw/misc/imx95_src.c (m7mix-power) and"
            echo "      the SYSRESETREQ->M33 wiring in hw/arm/fsl-imx95.c."
            fail=1
        else
            echo "FAIL: unexpected M7 boot counter $count"
            fail=1
        fi
        ;;
    *)
        echo "FAIL: could not read M7 boot counter; got '${count:-<no match>}'"
        fail=1
        ;;
esac

if [ "$fail" -eq 0 ]; then
    echo "=== M7 fault recovery verified: M7 faulted, SM cold-reset the M7 LM ==="
else
    echo "=== tests/m7-fault-recovery FAILED ==="
    exit 1
fi
