#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Regression wrapper: runs the project's growing test suite in order
# and stops on the first failure (set -e). Each subtest is independent;
# this wrapper just sequences them and labels their output so it's easy
# to scan.
#
# Currently runs (in order):
#   1. v1.0 regression: tests/functional/aarch64/test_imx95_evk.py
#      (boot Linux to userspace on the real SM)
#   2. v1.x Step 2 unit: tests/m7-boot/run.sh
#      (M7 hello firmware standalone)
#   3. v1.x Step 2 integration: tests/parallel-boot/run.sh
#      (A55 + M33 + M7 all up in parallel)
#   4. v1.x Step 3 integration: tests/m7-first/run.sh
#      (parallel-boot + assertion that SRC_GEN.SCR.M7MIX_RELEASE was
#      written, proving the SM-driven release path also fired,
#      plus an M7-before-Linux-userspace ordering assertion)
#   5. v1.x boot-matrix: tests/m33-m7-only/run.sh
#      (SM + M7 with no A55 cluster - fills the "M-side runs
#      independently of Linux" matrix gap)
#   6. interconnect legs: tests/interconnect-imx95/run-{eth,uart,spi,can}.sh
#      (two i.MX95 guests joined over a socket bridge, application
#      bytes proven byte-exact across ENETC / LPUART / LPSPI / FlexCAN;
#      each skip()s cleanly if its extra artifacts are absent)
#
# Future v1.x steps will add tests/a-first/run.sh and lifecycle /
# cohabitation tests; each gets its own entry here.
#
# Required artifacts (paths set via env vars):
#   QEMU SM_ELF KERNEL DTB INITRD CM7_ELF
#   plus, for the interconnect legs: KBUILD (kernel build tree with the CAN
#   modules + scripts/dtc) and an aarch64 cross gcc on PATH.
# Defaults match the development host's layout; override as needed.

set -e

REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"

MESON=${MESON:-build/pyvenv/bin/meson}

# Map the env vars used by parallel-boot/m7-boot to the QEMU_TEST_*
# names the meson functional test reads, so a single set of overrides
# drives the whole wrapper.
_ARTIFACTS=${IMX95_ARTIFACTS:-$HOME/imx95-artifacts}
_KBUILD=$_ARTIFACTS/linux-build/arch/arm64/boot
_DTS=$_KBUILD/dts/freescale
: "${KERNEL:=$_KBUILD/Image}"
: "${DTB:=$_DTS/imx95-19x19-evk.dtb}"
: "${INITRD:=$REPO/tests/busybox-initramfs/busybox-initramfs.cpio.gz}"
: "${SM_ELF:=$_ARTIFACTS/m33_image.elf}"
export QEMU_TEST_IMX95_KERNEL=$KERNEL
export QEMU_TEST_IMX95_DTB=$DTB
export QEMU_TEST_IMX95_INITRD=$INITRD
export QEMU_TEST_IMX95_SM_FW=$SM_ELF

# The board-to-board interconnect legs use their own env names (QEMU/KBUILD/
# SM_ELF); map them here so one set of overrides drives the whole wrapper. They
# skip() cleanly when their extra artifacts (an aarch64 cross gcc, the CAN
# kernel modules) are absent, so this stays safe on a minimal host.
export QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
export KBUILD=${KBUILD:-$_ARTIFACTS/linux-build}
export SM_ELF

echo "============================================="
echo "=== matrix drift check (fast, boot-free)   ==="
echo "===   README table vs test-matrix.yaml     ==="
echo "============================================="
tests/check-matrix-drift.sh

echo "============================================="
echo "=== v1.0 regression: functional test       ==="
echo "===   (A55 + M33 + Linux to userspace)     ==="
echo "============================================="
"$MESON" test -C build --suite thorough func-aarch64-imx95_evk

echo
echo "============================================="
echo "=== v1.x Step 2 unit: tests/m7-boot        ==="
echo "===   (M7 hello firmware standalone)       ==="
echo "============================================="
tests/m7-boot/run.sh

echo
echo "============================================="
echo "=== v1.x Step 2 integration:                ==="
echo "===   tests/parallel-boot                   ==="
echo "===   (A55 + M33 + M7 all in parallel)     ==="
echo "============================================="
tests/parallel-boot/run.sh

echo
echo "============================================="
echo "=== v1.x Step 3 integration:                ==="
echo "===   tests/m7-first                        ==="
echo "===   (SM-driven SRC.SCR.M7MIX-release path) ==="
echo "============================================="
tests/m7-first/run.sh

echo
echo "============================================="
echo "=== v1.x boot-matrix:                       ==="
echo "===   tests/m33-m7-only                     ==="
echo "===   (SM + M7, no A55 cluster)            ==="
echo "============================================="
tests/m33-m7-only/run.sh

echo
echo "============================================="
echo "=== interconnect legs (two i.MX95 guests,   ==="
echo "===   byte-exact over a socket bridge):     ==="
echo "===   ENETC / LPUART / LPSPI / FlexCAN      ==="
echo "===   (skip cleanly if artifacts absent)   ==="
echo "============================================="
tests/interconnect-imx95/run-eth.sh
tests/interconnect-imx95/run-uart.sh
tests/interconnect-imx95/run-spi.sh
tests/interconnect-imx95/run-can.sh

echo
echo "============================================="
echo "All tests passed:"
echo "  - v1.0 regression (A55 + M33 + Linux to userspace)"
echo "  - v1.x Step 2 unit (M7 hello firmware)"
echo "  - v1.x Step 2 integration (A55 + M33 + M7 parallel)"
echo "  - v1.x Step 3 integration (SM-driven M7 release path)"
echo "  - v1.x boot-matrix (SM + M7, no A55)"
echo "  - interconnect legs: ENETC / LPUART / LPSPI / FlexCAN"
echo "    (two guests, byte-exact over a socket bridge)"
echo "============================================="
