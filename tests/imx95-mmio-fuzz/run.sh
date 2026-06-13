#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run the i.MX 95 MMIO register robustness fuzz (see fuzz.py).
#
# Prefers a sanitizer (ASan+UBSan) build of qemu-system-aarch64 for maximum
# catch (out-of-bounds / UB that a plain build wouldn't trap). Build one with:
#
#   ./configure --target-list=aarch64-softmmu --enable-sanitizers \
#               --disable-docs --disable-werror -- (in a fresh build dir, e.g.)
#   mkdir build-asan && cd build-asan && ../configure ... && make -j
#
# Then:  ASAN_BUILD=build-asan tests/imx95-mmio-fuzz/run.sh
# Falls back to the normal ./build if no sanitizer build is present (still a
# useful crash/assert sweep, just without sanitizer depth).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
cd "$ROOT"

ASAN_BUILD=${ASAN_BUILD:-build-asan}
if [ -x "$ASAN_BUILD/qemu-system-aarch64" ]; then
    QEMU="$ASAN_BUILD/qemu-system-aarch64"
    echo "using SANITIZER build: $QEMU"
elif [ -x "build/qemu-system-aarch64" ]; then
    QEMU="build/qemu-system-aarch64"
    echo "WARNING: no sanitizer build at $ASAN_BUILD; using plain $QEMU"
    echo "  (build a sanitizer qemu for deeper coverage - see header)"
else
    echo "SKIP: no qemu-system-aarch64 found"; exit 0
fi

QEMU="$QEMU" python3 "$HERE/fuzz.py"
