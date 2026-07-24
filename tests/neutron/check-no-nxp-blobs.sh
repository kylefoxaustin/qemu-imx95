#!/usr/bin/env bash
#
# Redistribution-clean guard: fail if any NXP-licensed eIQ Neutron SDK artifact
# (the runner ELF, golden kernels, driver libs, compiled model blobs) is present
# in the repository tree. Our repo must contain ZERO NXP-licensed content; the
# runner is operator-supplied (see docs/neutron-runner-operator-supplies.md).
#
# Run in CI and as a pre-commit check. Exit 0 = clean, 1 = a blob was found.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

REPO="$(git rev-parse --show-toplevel)"
cd "$REPO"

# Names/patterns that would indicate NXP-licensed SDK content in the tree.
# (These are the SDK's own filenames; our own GPL glue/fixtures never match.)
BLOB_NAMES=(
    'neutron-runner'          # the runner ELF
    'NeutronKernels.bin'      # golden kernel payload
    'libNeutronDriver.so'
    'libNeutronDriver.a'
    'libNeutronConverter.a'
    'NeutronFirmware.elf'     # runner-side firmware (guest fw is built, not shipped)
)
# Compiled-model / golden-output extensions that should never be committed.
BLOB_GLOBS=(
    '*.dvm'                   # ARA240/compiled model
    '*_neutron.tflite'        # converted models (operator-supplied)
    '*_results.bin'           # runner golden outputs
)

# Consider tracked + staged files (what could actually land in a commit).
mapfile -t FILES < <(git ls-files; git diff --cached --name-only)

fail=0
report() { echo "  FORBIDDEN: $1" >&2; fail=1; }

for f in "${FILES[@]}"; do
    [ -n "$f" ] || continue
    base="$(basename "$f")"
    for n in "${BLOB_NAMES[@]}"; do
        [ "$base" = "$n" ] && report "$f (NXP SDK artifact: $n)"
    done
    for g in "${BLOB_GLOBS[@]}"; do
        # shellcheck disable=SC2053
        [[ "$base" == $g ]] && report "$f (looks like a compiled/converted model or runner output: $g)"
    done
done

# Also refuse an actual ELF that self-identifies as neutron-runner, even renamed.
for f in "${FILES[@]}"; do
    [ -f "$f" ] || continue
    if head -c 4 "$f" 2>/dev/null | grep -q $'\x7fELF' 2>/dev/null; then
        if strings "$f" 2>/dev/null | grep -qiE 'neutron.?runner|NeutronKernels'; then
            report "$f (ELF referencing neutron-runner / NeutronKernels)"
        fi
    fi
done

if [ "$fail" -ne 0 ]; then
    echo >&2
    echo "check-no-nxp-blobs: NXP-licensed content must NOT be committed." >&2
    echo "  The eIQ Neutron SDK is operator-supplied; keep it OUTSIDE the repo." >&2
    echo "  See docs/neutron-runner-operator-supplies.md." >&2
    exit 1
fi

echo "check-no-nxp-blobs: clean (no NXP SDK artifacts in the tree)."
