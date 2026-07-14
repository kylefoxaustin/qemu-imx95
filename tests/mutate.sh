#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Apply a mutation to a source file and REFUSE TO PROCEED unless it landed.
#
# WHY THIS EXISTS
#
# A mutation test proves an assertion can FAIL. It is the only thing standing
# between a green suite and a green suite that is green about nothing. But the
# mutation itself is applied by a `sed` or a `python3 -c`, and those can miss
# their pattern - a changed line, an escaping bug, a file that moved - and when
# they do they fail SILENTLY. The build still builds. The test still runs. It
# comes back GREEN.
#
#   A MUTATION THAT DID NOT ARM TESTS NOTHING - AND ITS GREEN IS
#   INDISTINGUISHABLE FROM A PASSING MODEL.          (rt1180emulator)
#
# Both failure directions have now bitten this fleet in one day:
#
#   - MINE: an impostor flag never reached the guest, so the "impostor" sent
#     ordinary frames, my receiver correctly accepted them, and my harness
#     reported that MY MODEL was broken. I was one commit from "fixing" code
#     that was already right.
#       A NEGATIVE TEST THAT DID NOT PRODUCE THE CONDITION IT NAMES DOES NOT
#       MERELY MISS A BUG - IT MANUFACTURES ONE, AND THE FIX YOU THEN APPLY IS
#       DAMAGE.
#
#   - rt1180's: a mangled escape meant a slave-mode mutation never applied. It
#     came back green - "the model tolerates slave mode!" - and flattered the
#     model. That is the worse direction, because it is the one you do not go
#     back and check.
#       THE INSTRUMENT FAILS SILENTLY FAR MORE OFTEN THAN IT FAILS LOUDLY, AND
#       IT FAILS IN THE DIRECTION THAT FLATTERS THE STORY YOU ARE ALREADY
#       TELLING.                                             (qualcomm)
#
# So: never mutate with a bare sed again. This applies the edit, PROVES the file
# changed and the marker is present, runs the command, and always restores.
#
#   usage:  tests/mutate.sh <file> <sed-expression> <marker> <command...>
#
#   <marker>  a string that MUST appear in the file after the edit. Put it in
#             the replacement text (e.g. a /* MUTATION */ comment) so its
#             presence is proof the edit landed, not a guess.
#
#   exit 2 = the mutation DID NOT ARM. Nothing was run. This is not a pass and
#            it is not a failure - it is a VOID, and it must never be reported
#            as either.
set -u

if [ $# -lt 4 ]; then
    echo "usage: $0 <file> <sed-expr> <marker> <command...>" >&2
    exit 2
fi
file=$1; expr=$2; marker=$3; shift 3

[ -f "$file" ] || { echo "MUTATE: no such file: $file" >&2; exit 2; }

before=$(md5sum "$file" | cut -d' ' -f1)
backup=$(mktemp)
cp "$file" "$backup"
# Always put the file back, whatever happens - including a kill. A mutation left
# in the tree is a landmine that the NEXT run reads as a regression.
trap 'cp "$backup" "$file"; rm -f "$backup"' EXIT INT TERM

sed -i "$expr" "$file"
after=$(md5sum "$file" | cut -d' ' -f1)

if [ "$before" = "$after" ]; then
    echo "MUTATE: VOID - the edit did not change $file."
    echo "MUTATE: the pattern did not match. Nothing was run, and NOTHING IS"
    echo "MUTATE: PROVEN. A mutation that did not arm tests nothing, and its"
    echo "MUTATE: green is indistinguishable from a passing model."
    exit 2
fi
# -F: the marker is a FIXED STRING, not a regex. The natural marker to use is a
# C comment like "/* MUTATION */", which is almost entirely regex metacharacters
# - and grep without -F silently failed to find it and reported the armed
# mutation as VOID. A guard whose own check is fragile is a guard that will one
# day tell you a real mutation never armed, and you will believe it.
if ! grep -qF -- "$marker" "$file"; then
    echo "MUTATE: VOID - $file changed but the marker '$marker' is absent."
    echo "MUTATE: the edit landed somewhere, but not where you think. Refusing"
    echo "MUTATE: to run: an edit you cannot point at is not a mutation."
    exit 2
fi

echo "MUTATE: ARMED - $file mutated, marker '$marker' present. Running:"
echo "MUTATE:   $*"
"$@"
rc=$?
echo "MUTATE: command exited $rc (a mutation test EXPECTS a non-zero here:"
echo "MUTATE: if the suite still passes, the assertion is decoration)."
exit $rc
