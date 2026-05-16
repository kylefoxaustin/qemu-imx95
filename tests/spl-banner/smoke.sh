#!/usr/bin/env bash
# tests/spl-banner/smoke.sh
#
# Wallclock smoke regression for the imx95 SPL banner path. Runs SPL,
# waits up to ${TIMEOUT_S:-30} seconds for the U-Boot SPL banner to
# appear on the serial port, exits 0 if it does and non-zero on any
# of: build artifact missing, QEMU binary missing, banner not seen
# within the timeout, QEMU crash.
#
# Catches both functional regressions in any device on the path
# (LPUART, MU, SCMI server, ELE responder, GIC routing, OCRAM
# placement, ...) and any future `-device loader` / `arm_load_kernel`
# ordering break that silently swaps the reset PC.
#
# Intended usage:
#   ./tests/spl-banner/smoke.sh            # default 30s wallclock cap
#   TIMEOUT_S=120 ./tests/spl-banner/smoke.sh
#
# Exit codes:
#   0  banner observed
#   1  banner not observed within TIMEOUT_S
#   2  required input missing (SPL binary or QEMU)

set -u
cd "$(dirname "$0")/../.."

TIMEOUT_S=${TIMEOUT_S:-30}
SPL_BIN=tests/spl-banner/uboot-build/spl/u-boot-spl.bin
QEMU=build/qemu-system-aarch64
EXPECT='U-Boot SPL 2025.04'
LOG=$(mktemp -t imx95-smoke.XXXXXX.log)

if [ ! -x "$QEMU" ]; then
    echo "smoke: QEMU not built at $QEMU" >&2
    exit 2
fi
if [ ! -f "$SPL_BIN" ]; then
    echo "smoke: SPL binary not built at $SPL_BIN (see tests/spl-banner/README.md)" >&2
    exit 2
fi

echo "smoke: running SPL for up to ${TIMEOUT_S}s, log -> $LOG"
timeout --foreground "${TIMEOUT_S}" "$QEMU" \
    -M imx95-19x19-evk \
    -nographic \
    -m 2G \
    -device loader,file="$SPL_BIN",addr=0x20480000,cpu-num=0,force-raw=on \
    >"$LOG" 2>&1 || true

if grep -q "$EXPECT" "$LOG"; then
    echo "smoke: PASS (banner observed)"
    rm -f "$LOG"
    exit 0
fi

echo "smoke: FAIL (banner '$EXPECT' not seen within ${TIMEOUT_S}s)" >&2
echo "smoke: last 30 lines of log:" >&2
tail -n 30 "$LOG" >&2
echo "smoke: full log at $LOG" >&2
exit 1
