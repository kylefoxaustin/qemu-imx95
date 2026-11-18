#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Anti-drift guard (fleet pattern, from i.MX91 446062f980). The README condensed
# capability table and the detailed docs/validation/test-result-matrix.md both
# render from ONE source of truth, docs/validation/test-matrix.yaml. This fails
# the build if:
#   * verify_groups() finds a block un-grouped, double-grouped, or mis-tiered
#     (a group's tier disagreeing with the block's) -- itself a silent fail,
#   * verify_broken() finds a known-broken record that is incomplete, owned by
#     nobody in the closed set, or pointing at a measurement that no longer
#     exists -- a dangling defect record still reads as evidence, and
#   * the committed README capability table is stale vs a fresh regeneration.
# Fast + boot-free: run it first in CI.
#
# The RUNTIME half of the known-broken gate needs a built QEMU (and, for the
# harnesses, the operator's artifacts), so it is deliberately not here:
#     tests/gen-test-matrix.py --check-broken
# fails if a capability recorded as broken now passes its own test.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/.." && pwd); cd "$ROOT"

# --inject-readme runs verify_groups() + verify_broken(), rewrites the table,
# and EXITS NON-ZERO if it had to change anything. Branch on that rc, and keep
# `|| rc=$?` so `set -e` cannot kill the script before the diagnosis prints.
#
# Do NOT test staleness with `git diff -- README.md`: that asks a different
# question. It is true for ANY uncommitted README edit, so a prose fix elsewhere
# in the file reports as a stale capability table (a different defect with a
# different fix) -- and in the genuinely-stale case `set -e` had already exited
# on the generator's rc, so the git branch was only ever reached by that false
# positive.
rc=0
python3 tests/gen-test-matrix.py --inject-readme || rc=$?

if [ "$rc" -ne 0 ]; then
    echo "MATRIX GATE FAILED (tests/gen-test-matrix.py --inject-readme rc=$rc)."
    echo "Either the README capability table was stale -- in which case it has"
    echo "just been regenerated from docs/validation/test-matrix.yaml and the"
    echo "diff below is what to review and commit -- or the matrix failed"
    echo "validation, in which case the generator's message above names the"
    echo "record and NOTHING was rewritten (so the diff shows no table change)."
    git --no-pager diff -- README.md | sed -n '1,40p' || true
    exit 1
fi
# The detailed matrix is generated too, but regenerating all of it runs the
# qtests; its Known-broken section renders from the YAML alone, so that part
# can be gated here. Without this, deleting a record regenerates the README
# and leaves the row sitting in the detailed matrix.
python3 tests/gen-test-matrix.py --check-doc-section

echo "PASS: README capability table + Known-broken section are in sync with"
echo "      docs/validation/test-matrix.yaml"
