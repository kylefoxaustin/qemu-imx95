#!/usr/bin/env bash
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
#
# Future v1.x steps will add tests/a-first/run.sh and lifecycle /
# cohabitation tests; each gets its own entry here.
#
# Required artifacts (paths set via env vars):
#   QEMU SM_ELF KERNEL DTB INITRD CM7_ELF
# Defaults match the development host's layout; override as needed.

set -e

REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"

MESON=${MESON:-build/pyvenv/bin/meson}

# Map the env vars used by parallel-boot/m7-boot to the QEMU_TEST_*
# names the meson functional test reads, so a single set of overrides
# drives the whole wrapper.
_KBUILD=$HOME/Documents/linux-imx95-build/arch/arm64/boot
_DTS=$_KBUILD/dts/freescale
: "${KERNEL:=$_KBUILD/Image}"
: "${DTB:=$_DTS/imx95-19x19-evk.dtb}"
: "${INITRD:=$REPO/tests/busybox-initramfs/busybox-initramfs.cpio.gz}"
: "${SM_ELF:=$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}"
export QEMU_TEST_IMX95_KERNEL=$KERNEL
export QEMU_TEST_IMX95_DTB=$DTB
export QEMU_TEST_IMX95_INITRD=$INITRD
export QEMU_TEST_IMX95_SM_FW=$SM_ELF

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
echo "All tests passed:"
echo "  - v1.0 regression (A55 + M33 + Linux to userspace)"
echo "  - v1.x Step 2 unit (M7 hello firmware)"
echo "  - v1.x Step 2 integration (A55 + M33 + M7 parallel)"
echo "  - v1.x Step 3 integration (SM-driven M7 release path)"
echo "  - v1.x boot-matrix (SM + M7, no A55)"
echo "============================================="
