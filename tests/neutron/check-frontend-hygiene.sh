#!/bin/bash
# check-frontend-hygiene.sh
#
# Phase 0 separation-hygiene test.
#
# Track B upstreamable front end (hw/misc/imx95_neutron.c) must not contain
# any Track A subprocess-runner concepts: no subprocess spawning, no scratch
# directories, no fixture-file references. Those live only in the runner
# backend (imx95_neutron_backend_runner.c) and the runner stub.
#
# The front end should also not use #ifdef CONFIG_IMX95_NEUTRON_RUNNER,
# because CONFIG_* macros are poisoned in QEMU common code.
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FE="$ROOT/hw/misc/imx95_neutron.c"

if [ ! -f "$FE" ]; then
    echo "FAIL: $FE not found"
    exit 1
fi

fail=0

# Track A operational tokens that must not appear in the front end.
FORBIDDEN_RE='g_subprocess|GSubprocess|g_spawn|mkdtemp|mkstemp|/tmp/|fopen|g_file_get_contents'
if grep -nE "$FORBIDDEN_RE" "$FE" >/dev/null 2>&1; then
    echo "FAIL: forbidden Track A operations in front end:"
    grep -nE "$FORBIDDEN_RE" "$FE" | sed 's/^/    /'
    fail=1
fi

# Track A QOM property string literals must not appear in the front end. The
# neutron-runner-* / neutron-keep-scratch property names are registered by the
# runner backend (real or stub) via neutron_backend_runner_add_props(), so no
# such string literal should exist in imx95_neutron.c code. We only look for
# these tokens as string literals (leading double-quote) to avoid matching
# occurrences inside comments that describe the architecture.
PROP_RE='"neutron-runner-|"neutron-keep-scratch"'
if grep -nE "$PROP_RE" "$FE" >/dev/null 2>&1; then
    echo "FAIL: front end still declares runner QOM property strings:"
    grep -nE "$PROP_RE" "$FE" | sed 's/^/    /'
    fail=1
fi

# DEFINE_PROP_* macros for runner fields must not appear in the front end.
if grep -nE 'DEFINE_PROP_[A-Z0-9_]+\("neutron-(runner-|keep-scratch)' "$FE" >/dev/null 2>&1; then
    echo "FAIL: front end still uses DEFINE_PROP_* for runner properties:"
    grep -nE 'DEFINE_PROP_[A-Z0-9_]+\("neutron-(runner-|keep-scratch)' "$FE" | sed 's/^/    /'
    fail=1
fi

# No poisoned CONFIG_* ifdefs referencing the runner.
if grep -nE '#[[:space:]]*(if|ifdef|ifndef).*CONFIG_IMX95_NEUTRON_RUNNER' "$FE" >/dev/null 2>&1; then
    echo "FAIL: front end still uses #ifdef CONFIG_IMX95_NEUTRON_RUNNER"
    grep -nE '#[[:space:]]*(if|ifdef|ifndef).*CONFIG_IMX95_NEUTRON_RUNNER' "$FE" | sed 's/^/    /'
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "OK: $FE is clean of Track A tokens and CONFIG_IMX95_NEUTRON_RUNNER ifdefs."
fi

exit $fail
