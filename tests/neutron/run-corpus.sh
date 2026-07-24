#!/usr/bin/env bash
#
# Run a corpus of int8 .tflite CNNs through the QEMU i.MX 95 Neutron *runner*
# backend and assert each is BIT-EXACT vs the operator-supplied neutron-runner.
#
# For each model: neutron-converter --target imx95 --keep-graphs, generate N
# random inputs, run neutron-runner to get goldens, then drive the runner
# backend IN QEMU (the generalized runner-bitexact qtest) and compare. This
# validates the backend's DMA staging/writeback across every model shape - both
# sides use the same golden kernels, so a mismatch is our plumbing, not numerics.
#
# OPERATOR-SUPPLIES: needs the eIQ Neutron SDK (converter + runner), obtained by
# you from NXP under LA_OPT. This script ships/commits NOTHING from it; all
# artifacts (converted models, inputs, goldens) live in a scratch dir OUTSIDE
# the repo. Per LA_OPT 3.8 it prints only functional PASS/FAIL, no perf numbers.
#
# Usage:
#   tests/neutron/run-corpus.sh                 # default: the imx93 nnsuite
#   MODELS_DIR=... MANIFEST=... tests/neutron/run-corpus.sh
#   MODELS="a.tflite:150528:1001 b.tflite:1228800:705600" tests/neutron/run-corpus.sh
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -uo pipefail

SDK="${NEUTRON_SDK:-$HOME/Documents/tools/nxp/eIQ/eiq-neutron-sdk-linux-3.1.3}"
CONV="${NEUTRON_CONVERTER:-$SDK/bin/neutron-converter}"
RUNNER="${NEUTRON_RUNNER_PATH:-$SDK/bin/neutron-runner}"
REPO="$(git rev-parse --show-toplevel)"
QEMU="${QEMU:-$REPO/build/qemu-system-aarch64}"
QTEST="${QTEST:-$REPO/build/tests/qtest/imx95-neutron-test}"
NVEC="${NVEC:-5}"
WORK="${WORK:-$(mktemp -d "${TMPDIR:-/tmp}/neutron-corpus-XXXXXX")}"
MANIFEST="${MANIFEST:-$HOME/imx93-npu-testkit/nnsuite/manifest.tsv}"
MODELS_DIR="${MODELS_DIR:-$HOME/imx93-npu-testkit/nnsuite/models}"

for t in "$CONV" "$RUNNER"; do
    [ -x "$t" ] || { echo "ERROR: not executable: $t (bring your own eIQ SDK)"; exit 1; }
done
[ -x "$QEMU" ]  || { echo "ERROR: no qemu at $QEMU (ninja -C build first)"; exit 1; }
[ -x "$QTEST" ] || { echo "ERROR: no qtest at $QTEST"; exit 1; }
case "$(cd "$WORK" && pwd -P)/" in "$REPO"/*)
    echo "ERROR: WORK ($WORK) is inside the repo; keep artifacts outside."; exit 1;; esac

echo "corpus run -> $WORK   (NVEC=$NVEC per model)"
printf '%-34s %-8s %-8s\n' "MODEL" "FUSE" "BITEXACT"
printf '%-34s %-8s %-8s\n' "-----" "----" "--------"

pass=0; fail=0; skip=0; nofuse=0

# product of the integer dims in an imx93-manifest shape column
# (format: "[np.int32(1), np.int32(224), np.int32(224), np.int32(3)]")
shape_bytes() {
    python3 - "$1" <<'PY'
import re, sys, math
d = re.findall(r'int32\((\d+)\)', sys.argv[1]) or re.findall(r'\d+', sys.argv[1])
print(math.prod(int(x) for x in d) if d else 0)
PY
}

run_one() {   # name  tflite_path  in_size  out_size
    local name="$1" src="$2" in_size="$3" out_size="$4"
    local drop="$WORK/$name"
    local conv="$drop/${name}_neutron.tflite"
    local stem="${name}_neutron"
    mkdir -p "$drop/inputs" "$drop/neutron-runner-outputs"

    # convert (already-converted models pass through)
    local fuse
    fuse=$("$CONV" --input "$src" --target imx95 --keep-graphs \
             --output "$conv" 2>&1 | grep -oiE "Number of Neutron operators = [0-9]+" \
             | grep -oE "[0-9]+$" | head -1)
    fuse="${fuse:-0}"
    if [ ! -s "$conv" ]; then
        printf '%-34s %-8s %-8s\n' "$name" "-" "CONV-ERR"; skip=$((skip+1)); return
    fi
    [ "$fuse" = "0" ] && nofuse=$((nofuse+1))

    # inputs + goldens
    local i
    for i in $(seq 0 $((NVEC-1))); do
        head -c "$in_size" /dev/urandom > "$drop/inputs/$(printf '%04d' "$i").bin"
    done
    if ! "$RUNNER" --input "$conv" --dataset "$drop/inputs" \
            --use-neutron-runtime=true --target imx95 \
            --output-results "$drop/neutron-runner-outputs/${stem}_results.bin" \
            >/dev/null 2>&1; then
        printf '%-34s %-8s %-8s\n' "$name" "$fuse" "RUNNER-ERR"; skip=$((skip+1)); return
    fi

    # drive the runner backend IN QEMU, bit-exact vs the goldens
    if NEUTRON_RUNNER_PATH="$RUNNER" NEUTRON_RUNNER_DROP="$drop" \
       NEUTRON_BR_TFLITE="${stem}.tflite" \
       NEUTRON_BR_INPUT_SIZE="$in_size" NEUTRON_BR_OUTPUT_SIZE="$out_size" \
       NEUTRON_BR_NUM_VECTORS="$NVEC" \
       QTEST_QEMU_BINARY="$QEMU" \
       "$QTEST" -p /aarch64/imx95/neutron/runner-bitexact >/dev/null 2>&1; then
        printf '%-34s %-8s %-8s\n' "$name" "$fuse" "PASS"; pass=$((pass+1))
    else
        printf '%-34s %-8s %-8s\n' "$name" "$fuse" "FAIL"; fail=$((fail+1))
    fi
}

if [ -n "${MODELS:-}" ]; then
    # explicit "path:in_size:out_size ..." list
    for spec in $MODELS; do
        IFS=: read -r p in_size out_size <<<"$spec"
        run_one "$(basename "${p%.tflite}")" "$p" "$in_size" "$out_size"
    done
else
    [ -f "$MANIFEST" ] || { echo "ERROR: no manifest at $MANIFEST"; exit 1; }
    # process substitution (not a pipe) so pass/fail counters persist.
    while IFS=$'\t' read -r name kind in_shape out_shape bytes; do
        [ -n "$name" ] || continue
        src="$MODELS_DIR/${name}.tflite"
        [ -f "$src" ] || { printf '%-34s %-8s %-8s\n' "$name" "-" "NO-FILE"; continue; }
        in_size=$(shape_bytes "$in_shape"); out_size=$(shape_bytes "$out_shape")
        if [ "$in_size" -le 0 ] || [ "$out_size" -le 0 ]; then
            printf '%-34s %-8s %-8s\n' "$name" "-" "NO-SHAPE"; continue
        fi
        run_one "$name" "$src" "$in_size" "$out_size"
    done < <(tail -n +2 "$MANIFEST")
fi

echo "-------------------------------------------------"
echo "PASS=$pass FAIL=$fail SKIP=$skip  (no-fuse models still run on the runner)"
[ "$fail" -eq 0 ]
