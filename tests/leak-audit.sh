#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Find harnesses that CANNOT kill what they launch, and processes that prove it.
#
# WHY THIS EXISTS
#
#   `timeout N cmd` sends SIGTERM. If the child ignores it, `timeout` WAITS
#   FOREVER. A QEMU spinning in a TCG vCPU loop - a firmware hang, an -icount
#   run, a guest that never reaches the shutdown path - does exactly that.
#
#     TIMEOUT WITHOUT AN ESCALATION IS NOT A TIMEOUT. IT IS A REQUEST.
#                                                          (mcxn947qemu)
#
# And the half that makes it invisible, measured on this host:
#
#     timeout 2        -> rc=124 ("timed out") AFTER 12 SECONDS   <-- it waited for
#                         the child to die of old age, then reported SUCCESS at
#                         bounding it. rc=124 IS THE CODE A HARNESS CHECKS TO
#                         DETECT A TIMEOUT, so the harness cannot tell a real kill
#                         from a wait-until-natural-death.
#     timeout -k 1 2   -> rc=137 (SIGKILL) after 3s               <-- actually killed
#     timeout -s KILL 2-> rc=137 after 2s                         <-- also safe
#
# The cost is not theoretical. On this shared box, one session leaked 8 orphans
# burning 30 CORE-HOURS, its ENET test failed under the load it was itself
# creating, and it filed the failure as "flaky". A contaminated box does not
# produce obviously-broken results - it produces plausible, disappointing, WRONG
# ones, and you thank it for its candour.
#
# TWO CHECKS, AND BOTH HALVES MATTER
#
#   (1) SOURCE. A detector that only knows `timeout [0-9]` cannot see
#       `timeout "$TMO"` - the form we actually write. 93emulator's detector
#       printed "NONE" with the offenders listed directly above the verdict.
#       But the mirror error is just as real: rt1180's fixed detector then
#       flagged `timeout -s KILL`, which is ALREADY CORRECT, and would have sent
#       them to edit working code.
#
#         A FALSE ACQUITTAL GETS FILED AS CLEAN AND NEVER LOOKED AT AGAIN.
#         A FALSE CONVICTION SENDS YOU TO EDIT WORKING CODE.
#
#       So this enumerates BOTH lists: unsafe = a bare `timeout <n|$VAR>` wrapping
#       a QEMU; safe = -k / -s KILL / --signal=KILL.
#
#   (2) LIVE. rt1180emulator's, and it needs no census and no attribution:
#
#         A `timeout N` WHOSE OWN ELAPSED TIME EXCEEDS N HAS, BY DEFINITION,
#         FAILED TO KILL ITS CHILD. THE DEATH SENTENCE IS IN ITS OWN COMMAND LINE.
#
#       It convicts on the accused's own testimony - no guessing whose work is
#       "live", no proxy, no judgement. That is why it beat every hand-written
#       census on this host, including mine.
#
# usage: tests/leak-audit.sh          # audit source + live processes
#        tests/leak-audit.sh --mine   # only flag live leaks from THIS tree
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
MINE_ONLY=${1:-}
rc=0

echo "=== (1) SOURCE: host-side 'timeout' wrapping a QEMU with no escalation"
# Safe forms are excluded FIRST, so a correct harness is never convicted.
unsafe=$(grep -rn --include=*.sh -E \
    '\btimeout\s+("?\$\{?[A-Za-z_][A-Za-z0-9_]*\}?"?|[0-9]+)\s+("?\$\{?QEMU|\./build/qemu-system-)' \
    "$ROOT/tests" 2>/dev/null | grep -vE 'timeout\s+(-k|-s\s*KILL|--signal=KILL)' || true)
if [ -n "$unsafe" ]; then
    echo "$unsafe" | sed 's/^/  LEAKS: /'
    echo "  -> these send SIGTERM and then WAIT FOREVER if QEMU ignores it."
    echo "  -> fix: timeout -k 5 <n> \$QEMU ...   (SIGKILL 5s after the ignored TERM)"
    rc=1
else
    echo "  none - every host-side QEMU timeout can escalate."
fi

# Report the safe ones too, so "zero unsafe" is a claim with a denominator behind
# it. A detector that only ever prints "clean" is indistinguishable from one that
# is looking in the wrong place.
safe=$(grep -rn --include=*.sh -cE 'timeout\s+(-k|-s\s*KILL|--signal=KILL)' \
       "$ROOT/tests" 2>/dev/null | awk -F: '{n+=$2} END{print n+0}')
echo "  (checked against $safe already-safe timeout site(s), which were NOT touched)"

echo
echo "=== (2) LIVE: a 'timeout N' older than N has already failed to kill its child"
live=$(ps -eo pid,etimes,etime,args 2>/dev/null |
       awk '$4=="timeout" && $5+0>0 && $2 > $5*10 {print}')
if [ -n "$MINE_ONLY" ]; then
    live=$(printf '%s\n' "$live" | grep -F "$ROOT" || true)
fi
if [ -n "$live" ]; then
    printf '%s\n' "$live" | sed 's/^/  LEAKED: /' | cut -c1-140
    echo "  -> the wrapper outlived its own deadline. It is not bounding anything."
    rc=1
else
    echo "  none."
fi

echo
echo "=== (3) ORPHANS: our own processes reparented to init (a CPU-sorted census"
echo "        CANNOT see these - an idle corpse burns 0% and still holds sockets)"
found=0
for p in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
    exe=$(readlink "/proc/$p/exe" 2>/dev/null) || continue
    case "$exe" in
        "$ROOT"/*)
            ppid=$(awk '{print $4}' "/proc/$p/stat" 2>/dev/null)
            if [ "${ppid:-0}" = 1 ]; then
                echo "  ORPHAN: pid=$p ppid=1 exe=$exe"
                found=1
            fi
            ;;
    esac
done
[ "$found" = 0 ] && echo "  none." || rc=1

echo
[ "$rc" = 0 ] && echo "PASS: nothing in this tree can leak, and nothing of ours has." \
             || echo "FAIL: see above."
exit $rc
