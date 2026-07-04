#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Anti-drift guard (fleet pattern, from i.MX91 446062f980). The README condensed
# capability table and the detailed docs/validation/test-result-matrix.md both
# render from ONE source of truth, docs/validation/test-matrix.yaml. This fails
# the build if:
#   * verify_groups() finds a block un-grouped, double-grouped, or mis-tiered
#     (a group's tier disagreeing with the block's) -- itself a silent fail, and
#   * the committed README capability table is stale vs a fresh regeneration.
# Fast + boot-free: run it first in CI.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/.." && pwd); cd "$ROOT"

# --inject-readme runs verify_groups() and rewrites the table between the markers.
python3 tests/gen-test-matrix.py --inject-readme

if ! git diff --quiet -- README.md; then
    echo "MATRIX DRIFT: the README capability table is stale."
    echo "Edit docs/validation/test-matrix.yaml, then run:"
    echo "    tests/gen-test-matrix.py --inject-readme"
    git --no-pager diff -- README.md | sed -n '1,40p'
    exit 1
fi
echo "PASS: README capability table is in sync with docs/validation/test-matrix.yaml"
