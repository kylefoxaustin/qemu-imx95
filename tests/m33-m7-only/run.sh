#!/usr/bin/env bash
#
# v1.x boot-matrix coverage: M33 (SM firmware) + M7 (cm7-hello) with
# NO A55 cluster brought up. Fills the "is the M-side independently
# runnable without Linux" gap left by the existing tests:
#
#   tests/sm-banner      -> M33 only (no M7, no A55)
#   tests/m7-boot        -> M7 only  (no SM,  no A55) - via the
#                           machine-init-done BH path; the SM doesn't
#                           need to write SRC.SCR for the bit to come
#                           up in this config
#   tests/parallel-boot  -> M33 + A55 + M7
#   tests/m7-first       -> M33 + A55 + M7 + asserts SM-driven SRC.SCR
#                           release path fired
#   tests/m33-m7-only    -> M33 + M7, no A55 (THIS TEST)
#
# What this asserts:
#
#   1. SM banner shows up on the SM debug-monitor uart (LPUART2 -
#      serial_hd(1) per the SM firmware's BOARD_DEBUG_UART_INSTANCE=2).
#      Proves the M33 came up.
#   2. M7 fingerprint 0xC0FFEE07 at 0x20400000 via HMP.
#      Proves the M7 came up, released by the SM's CpuStart(M7) call
#      (the LM1 "M7" entry in mx95evk.cfg has boot=2 - the SM auto-
#      starts it during LMM_Boot).
#   3. The SRC_GEN.SCR.M7MIX_RELEASE latch bit is set (sticky).
#      Proves the silicon-faithful release path, not a host-side
#      cheat. Same Step-3 signal as tests/m7-first.
#   4. The A55 console (LPUART1, serial_hd(0)) stayed empty (no
#      Linux boot messages, no kernel panics from a half-started
#      A55 trying to execute PC=0). Sanity check that the config
#      really is "no A55".

set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
SM_ELF=${SM_ELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}
CM7_ELF=${CM7_ELF:-$REPO/tests/cm7-hello/cm7_image.elf}

need() {
    [ -e "$2" ] && return 0
    echo "error: $3 not found:" >&2
    echo "    $2" >&2
    echo "  set \$$1 (see tests/README) e.g.  $1=/path/... tests/m33-m7-only/run.sh" >&2
    exit 1
}
need QEMU    "$QEMU"    "qemu-system-aarch64 (build it: see README 'Building')"
need SM_ELF  "$SM_ELF"  "System Manager firmware m33_image.elf"
need CM7_ELF "$CM7_ELF" "M7 hello firmware (build: make -C tests/cm7-hello)"

A55_LOG=$(mktemp /tmp/imx95-m33m7-a55.XXX.log)    # LPUART1 (expected EMPTY)
SM_LOG=$(mktemp /tmp/imx95-m33m7-sm.XXX.log)      # LPUART2 (SM banner)
SOCK=$(mktemp -u /tmp/imx95-m33m7.XXX.sock)
PIDFILE=$(mktemp -u /tmp/imx95-m33m7.XXX.pid)

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

# No -kernel/-dtb/-initrd. Two -device loaders: SM on cpu-num=6,
# M7 on cpu-num=7. -serial #0 (LPUART1, A55 console) -> file we'll
# check stays empty. -serial #1 (LPUART2, SM debug console) ->
# the file we grep for the SM banner.
"$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -serial file:"$A55_LOG" -serial file:"$SM_LOG" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -device loader,file="$CM7_ELF",cpu-num=7 \
    -monitor unix:"$SOCK",server,nowait \
    -daemonize -pidfile "$PIDFILE"

# SM needs ~5 s to print its banner; give it 20 s with a poll.
echo "waiting for SM banner (up to 20 s)..."
deadline=$(( $(date +%s) + 20 ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    grep -qE "Hello from SM|SM Debug Monitor" "$SM_LOG" 2>/dev/null && break
    sleep 1
done

fail=0

if grep -qE "Hello from SM|SM Debug Monitor" "$SM_LOG"; then
    echo "PASS: SM banner detected on LPUART2 (M33 came up)"
else
    echo "FAIL: SM banner NOT detected on LPUART2"
    echo "  last 20 lines of SM log:"
    tail -20 "$SM_LOG" 2>/dev/null | sed 's/^/    /'
    fail=1
fi

# Peek M7 fingerprint + SRC.SCR. nc -q 2 closes after 2 s of idle so
# we don't tie up the monitor; do NOT send HMP "quit" (kills the VM).
HMP_RESULT=$( { echo "xp /1wx 0x20400000"
                echo "xp /1wx 0x44460010"
                sleep 1
              } | nc -U -q 2 "$SOCK" 2>&1 )

if echo "$HMP_RESULT" | grep -qE '20400000: 0x[cC]0[fF][fF][eE][eE]07'; then
    echo "PASS: M7 fingerprint 0xC0FFEE07 detected at 0x20400000"
else
    echo "FAIL: M7 fingerprint NOT detected at 0x20400000"
    echo "  HMP returned: $(echo "$HMP_RESULT" | grep '20400000:' || echo '<no match>')"
    fail=1
fi

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
            fail=1
        fi
        ;;
    *)
        echo "FAIL: SRC_GEN.SCR readback parse failed; got: '${SCR:-<no match>}'"
        fail=1
        ;;
esac

# A55 LPUART1 must stay empty in this config (no kernel loaded). A
# few bytes of stray output (e.g. a single newline) is acceptable -
# what we want to catch is the kernel actually running. Linux's first
# line on a real boot is always "Booting Linux on physical CPU ...",
# so use that as the negative-match anchor.
A55_SIZE=$(stat -c%s "$A55_LOG" 2>/dev/null || echo 0)
if grep -q "Booting Linux" "$A55_LOG"; then
    echo "FAIL: A55 console shows Linux boot output (expected empty in this config)"
    echo "  $A55_SIZE bytes on LPUART1; first 5 lines:"
    head -5 "$A55_LOG" 2>/dev/null | sed 's/^/    /'
    fail=1
else
    echo "PASS: A55 console empty of Linux boot output ($A55_SIZE bytes on LPUART1)"
fi

if [ "$fail" -eq 0 ]; then
    echo "=== M33+M7-only config verified: SM up, M7 up, A55 quiescent ==="
else
    echo "=== tests/m33-m7-only FAILED ==="
    exit 1
fi
