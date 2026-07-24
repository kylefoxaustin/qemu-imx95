#!/usr/bin/env bash
#
# Cross-executor fidelity oracle: run each int8 model through the operator-
# supplied eIQ neutron-runner (i.MX 95 Neutron golden kernels) on the SAME int8
# input a sibling ran through its Arm Ethos-U executor, and diff the outputs.
#
# This is an INDEPENDENT numeric check (Ethos-U vs Neutron on the same graph),
# complementary to run-corpus.sh (which proves our QEMU backend is bit-exact vs
# neutron-runner). Meaningful only where BOTH executors produce a valid output:
# the imx93 nnsuite scorecard marks densenet/inception/nasnet as Ethos CRASH,
# and the Neutron runner tool SIGILLs on mobilenet_v2/v3/resnet/efficientnet -
# so the clean intersection is mobilenet_v1 + the simple micro-ops.
#
# Inputs/goldens come from a sibling's testkit (operator-local); this script
# ships/commits NOTHING and prints only functional agreement, no perf.
#
# Usage:
#   tests/neutron/run-cross-executor.sh
#   CB=~/imx93-npu-testkit/nnsuite/cb_share MODELS_DIR=... run-cross-executor.sh
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -uo pipefail

SDK="${NEUTRON_SDK:-$HOME/Documents/tools/nxp/eIQ/eiq-neutron-sdk-linux-3.1.3}"
CONV="${NEUTRON_CONVERTER:-$SDK/bin/neutron-converter}"
RUNNER="${NEUTRON_RUNNER_PATH:-$SDK/bin/neutron-runner}"
CB="${CB:-$HOME/imx93-npu-testkit/nnsuite/cb_share}"
MODELS_DIR="${MODELS_DIR:-$HOME/imx93-npu-testkit/nnsuite/models}"
SCORECARD="${SCORECARD:-$HOME/imx93-npu-testkit/nnsuite/FINAL_scorecard.tsv}"
WORK="${WORK:-$(mktemp -d "${TMPDIR:-/tmp}/neutron-xexec-XXXXXX")}"

for t in "$CONV" "$RUNNER"; do
    [ -x "$t" ] || { echo "ERROR: not executable: $t (bring your own eIQ SDK)"; exit 1; }
done
[ -d "$CB/in" ] && [ -d "$CB/out" ] || { echo "ERROR: no cb_share in/out at $CB"; exit 1; }

# scorecard: models the sibling's Ethos executor ran cleanly (RESULT=OK).
declare -A ETHOS_OK
if [ -f "$SCORECARD" ]; then
    while IFS=$'\t' read -r name kind npu cpu sram result; do
        [ "$result" = "OK" ] && ETHOS_OK["$name"]=1
    done < <(tail -n +2 "$SCORECARD")
fi

row() { printf '%-30s %-10s %-9s %-10s\n' "$@"; }

echo "cross-executor (Neutron vs Ethos-U) -> $WORK"
row "MODEL" "MAXΔ(LSB)" "MISMATCH" "VERDICT"
row "-----" "---------" "--------" "-------"

exact=0; near=0; delta=0; skip=0

diff_int8() {   # neutron.bin  ethos.bin  -> "maxabs mismatch total"
    python3 - "$1" "$2" <<'PY'
import sys
a=open(sys.argv[1],'rb').read(); b=open(sys.argv[2],'rb').read()
n=min(len(a),len(b))
if n==0 or len(a)!=len(b):
    print(f"-1 {abs(len(a)-len(b))} {max(len(a),len(b))}"); sys.exit()
mx=mm=0
for i in range(n):
    d=abs((a[i]-256 if a[i]>127 else a[i])-(b[i]-256 if b[i]>127 else b[i]))
    if d: mm+=1
    if d>mx: mx=d
print(f"{mx} {mm} {n}")
PY
}

for inbin in "$CB"/in/*.bin; do
    name="$(basename "${inbin%.bin}")"
    gold="$CB/out/$name.bin"
    src="$MODELS_DIR/$name.tflite"
    [ -f "$gold" ] && [ -f "$src" ] || continue
    # only where the sibling's Ethos run was clean (else its golden is a crash)
    [ -n "${ETHOS_OK[$name]:-}" ] || { row "$name" "-" "-" "ETHOS-CRASH"; skip=$((skip+1)); continue; }

    drop="$WORK/$name"; mkdir -p "$drop/inputs" "$drop/out"
    cp "$inbin" "$drop/inputs/0000.bin"
    "$CONV" --input "$src" --target imx95 --keep-graphs --output "$drop/m.tflite" >/dev/null 2>&1
    if ! "$RUNNER" --input "$drop/m.tflite" --dataset "$drop/inputs" \
            --use-neutron-runtime=true --target imx95 \
            --output-results "$drop/out/r.bin" >/dev/null 2>&1; then
        row "$name" "-" "-" "NEUTRON-CRASH"; skip=$((skip+1)); continue
    fi
    nout="$(ls "$drop"/out/r_*.bin 2>/dev/null | head -1)"
    [ -n "$nout" ] || { row "$name" "-" "-" "NO-OUTPUT"; skip=$((skip+1)); continue; }

    read -r mx mm tot < <(diff_int8 "$nout" "$gold")
    if   [ "$mx" = "-1" ]; then v="SIZE-DIFF"; skip=$((skip+1))
    elif [ "$mx" = "0" ];  then v="EXACT";     exact=$((exact+1))
    elif [ "$mx" -le 1 ];  then v="±1 LSB";    near=$((near+1))
    else                         v="DELTA";    delta=$((delta+1)); fi
    row "$name" "$mx" "$mm/$tot" "$v"
done

echo "-------------------------------------------------------------"
echo "EXACT=$exact  NEAR(±1LSB)=$near  DELTA=$delta  SKIP=$skip"
echo "(Neutron vs Ethos-U agreement on the same int8 graph; SKIP = one executor crashed)"
