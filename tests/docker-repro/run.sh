#!/usr/bin/env bash
# Clean-room reproducibility check: build qemu-imx95 and run the test suite
# inside a pristine ubuntu:22.04 container, installing ONLY the packages the
# README lists. This is the strongest form of the "fresh-clone, different
# machine" gate (validation-todo Tier 1.1/1.2): a container starts with nothing
# pre-installed, so it catches build/runtime dependencies that are invisible on
# any machine with prior dev work.
#
# It found three such gaps the first time it ran (now in the README):
# python3-venv (ensurepip), python3-tomli (meson TOML), netcat-openbsd (nc -U
# for the M7 HMP monitor checks).
#
# Steps inside the container:
#   0  prove the relevant toolchain is absent (truly pristine)
#   0b apt install the README package set
#   1  fresh local clone + configure + ninja
#   2  no-artifact smoke tests (hello-imx95, m7-boot)
#   3  the Tier-1.2 hardcoded-path grep
#   4  (optional) full Linux boot to userspace, if boot artifacts are provided
#
# Usage (from the repo root):
#   tests/docker-repro/run.sh
#
# Step 4 needs the four NXP BSP boot artifacts (not redistributable, so not in
# the repo). Point the script at a directory holding them and it will mount it
# read-only and run the boot:
#   IMX95_ARTIFACTS=/path/to/dir tests/docker-repro/run.sh
# where the dir contains: m33_image.elf, Image, imx95-19x19-evk.dtb, and an
# initramfs.cpio.gz. Without it, steps 0-3 still run (build + smoke + grep).
set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE=${IMAGE:-ubuntu:22.04}
BRANCH=${BRANCH:-imx95-scaffold}
HERE=$(cd "$(dirname "$0")" && pwd)

command -v docker >/dev/null 2>&1 || { echo "error: docker not found"; exit 1; }

MOUNTS=(-v "$REPO":/src:ro -v "$HERE/in-container.sh":/test.sh:ro)
ART=${IMX95_ARTIFACTS:-}
if [ -n "$ART" ] && [ -d "$ART" ]; then
    MOUNTS+=(-v "$ART":/artifacts:ro)
    echo "boot step: artifacts from $ART"
else
    echo "boot step: skipped (set IMX95_ARTIFACTS=/dir with m33_image.elf +"
    echo "           Image + imx95-19x19-evk.dtb + initramfs.cpio.gz to enable)"
fi

exec docker run --rm -e BRANCH="$BRANCH" "${MOUNTS[@]}" "$IMAGE" bash /test.sh
