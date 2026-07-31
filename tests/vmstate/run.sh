#!/usr/bin/env bash
#
# qemu-imx95 VMState round-trip test.
#
# Boots a bare-metal "counter" guest that emits an incrementing letter over
# LPUART1, migrates the whole machine to a file mid-stream, restores it into a
# second QEMU with -incoming, and asserts the restored guest RESUMED rather than
# reset: no banner reprint (PC/CPU state round-tripped) and the letter sequence
# continues past 'A' (GPR/RAM round-tripped). This exercises the VMState of the
# CPU plus every device the machine instantiates in one save/restore cycle.
#
# Pass:  migration completes AND the restored guest resumes mid-loop.
# Catches: a device with missing/incorrect VMState (migration aborts), or a
#          post_load that fails to restore state (guest misbehaves after load).
#
# No external artifacts: the guest is built here from counter.S.
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$DIR/../../build/qemu-system-aarch64}"
CROSS="${CROSS:-aarch64-linux-gnu-}"
WORK="$(mktemp -d)"
trap 'kill ${PIDA:-} ${PIDB:-} 2>/dev/null; rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*"; exit 1; }

# --- build the guest ---
if [ ! -f "$DIR/counter.bin" ] || [ "$DIR/counter.S" -nt "$DIR/counter.bin" ]; then
    "${CROSS}as" -mcpu=cortex-a55 -o "$WORK/counter.o" "$DIR/counter.S" || fail "assemble"
    "${CROSS}ld" -T "$DIR/link.ld" -o "$WORK/counter.elf" "$WORK/counter.o" || fail "link"
    "${CROSS}objcopy" -O binary "$WORK/counter.elf" "$DIR/counter.bin" || fail "objcopy"
fi
BIN="$DIR/counter.bin"
[ -x "$QEMU" ] || fail "qemu not built at $QEMU"

STATE="$WORK/state.bin"
ALOG="$WORK/a.log"; BLOG="$WORK/b.log"
ASOCK="$WORK/qmp-a.sock"; BSOCK="$WORK/qmp-b.sock"

MACHINE=(-M imx95-19x19-evk -m 2G -kernel "$BIN" -display none)

# --- QMP one-shot driver: handshake, run commands, print last response ---
qmp() {  # args: sock, then JSON command objects
    local sock="$1"; shift
    python3 - "$sock" "$@" <<'PY'
import socket, sys, json, time
sock = sys.argv[1]; cmds = sys.argv[2:]
s = None
for _ in range(100):
    try:
        s = socket.socket(socket.AF_UNIX); s.connect(sock); break
    except OSError:
        time.sleep(0.1)
if s is None:
    print('{"error":"connect-timeout"}'); sys.exit(2)
f = s.makefile('rw')
f.readline()                                   # greeting
f.write('{"execute":"qmp_capabilities"}\n'); f.flush(); f.readline()
last = "{}"
for c in cmds:
    f.write(c + "\n"); f.flush()
    while True:
        line = f.readline()
        if not line:
            break
        o = json.loads(line)
        if "event" in o:                        # skip async events
            continue
        last = line.strip(); break
print(last)
PY
}

# wait until a log has >= N letters (guest counting past the banner)
wait_letters() {  # args: logfile, count, timeoutsec
    local log="$1" n="$2" t="$3" i=0
    while [ "$i" -lt "$((t*5))" ]; do
        [ -f "$log" ] && [ "$(grep -oE '[A-Z]' "$log" 2>/dev/null | wc -l)" -ge "$n" ] && return 0
        sleep 0.2; i=$((i+1))
    done
    return 1
}

echo "== qemu-imx95 VMState round-trip =="

# --- 1) source QEMU: boot + start counting ---
"$QEMU" "${MACHINE[@]}" -serial file:"$ALOG" \
        -qmp unix:"$ASOCK",server,nowait >/dev/null 2>&1 &
PIDA=$!
wait_letters "$ALOG" 3 20 || fail "source guest never started counting (log: $(cat "$ALOG" 2>/dev/null))"
ALAST=$(grep -oE '[A-Z]' "$ALOG" | tail -1)
echo "  source counting, last letter so far: $ALAST"

# --- 2) migrate the whole machine to a file ---
qmp "$ASOCK" "{\"execute\":\"migrate\",\"arguments\":{\"uri\":\"exec:cat > $STATE\"}}" >/dev/null
for i in $(seq 1 100); do
    st=$(qmp "$ASOCK" '{"execute":"query-migrate"}')
    echo "$st" | grep -q '"status": *"completed"' && { echo "  migrate: completed"; break; }
    echo "$st" | grep -q '"status": *"failed"'    && fail "migration FAILED: $st"
    sleep 0.2
done
echo "$st" | grep -q '"status": *"completed"' || fail "migration did not complete: $st"
[ -s "$STATE" ] || fail "state file empty"
echo "  state file: $(stat -c%s "$STATE") bytes"
kill "$PIDA" 2>/dev/null; wait "$PIDA" 2>/dev/null; PIDA=

# --- 3) destination QEMU: restore + resume ---
"$QEMU" "${MACHINE[@]}" -serial file:"$BLOG" \
        -qmp unix:"$BSOCK",server,nowait \
        -incoming "exec:cat $STATE" >/dev/null 2>&1 &
PIDB=$!
# wait until incoming load leaves the inmigrate state, then continue the VM
for i in $(seq 1 100); do
    st=$(qmp "$BSOCK" '{"execute":"query-status"}')
    echo "$st" | grep -qE '"status": *"(paused|postmigrate|prelaunch)"' && break
    echo "$st" | grep -q '"status": *"running"' && break
    sleep 0.2
done
qmp "$BSOCK" '{"execute":"cont"}' >/dev/null
wait_letters "$BLOG" 2 20 || fail "restored guest produced no output (did not resume). log: [$(cat "$BLOG" 2>/dev/null)]"
kill "$PIDB" 2>/dev/null; wait "$PIDB" 2>/dev/null; PIDB=

# --- 4) assertions ---
echo "  restored guest output: [$(tr -d '\n' < "$BLOG")]"
if grep -q "vmstate-counter boot" "$BLOG"; then
    fail "restored guest REPRINTED the banner -> it reset instead of resuming (CPU PC not restored)"
fi
BFIRST=$(grep -oE '[A-Z]' "$BLOG" | head -1)
[ -n "$BFIRST" ] || fail "no counter letters after restore"
if [ "$BFIRST" = "A" ]; then
    fail "counter restarted at 'A' -> register/RAM state not restored (source was at '$ALAST')"
fi

echo "PASS: machine migrated and the guest RESUMED mid-loop (banner not reprinted; counter continued at '$BFIRST', source was near '$ALAST')"
exit 0
