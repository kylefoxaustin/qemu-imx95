#!/usr/bin/env bash
# Runs INSIDE the ubuntu:22.04 container launched by run.sh. Host mounts (ro):
#   /src        - the qemu-imx95 repo (clone source; never built in place)
#   /artifacts  - (optional) m33_image.elf / Image / imx95-19x19-evk.dtb /
#                 initramfs.cpio.gz for the boot step
set -u
STEP() { echo; echo "============================================================"; echo "== $*"; echo "============================================================"; }
FAIL() { echo "XX FAIL: $*"; FAILED=1; }
FAILED=0
BRANCH=${BRANCH:-imx95-scaffold}

STEP "0. host identity (pristine container)"
. /etc/os-release; echo "OS: $PRETTY_NAME"; uname -m; echo "cores: $(nproc)"
echo "pre-existing tools (expect all 'absent'):"
for t in meson ninja aarch64-linux-gnu-gcc qemu-system-aarch64 arm-none-eabi-gcc; do
    command -v "$t" >/dev/null 2>&1 && echo "  PRESENT(!): $t" || echo "  absent: $t"
done

STEP "0b. install ONLY the README-listed host packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq \
    git \
    meson ninja-build python3 python3-venv python3-tomli \
    gcc libc6-dev pkg-config libglib2.0-dev libpixman-1-dev \
    binutils-aarch64-linux-gnu gcc-aarch64-linux-gnu \
    gcc-arm-none-eabi \
    netcat-openbsd \
    bison flex libssl-dev libgnutls28-dev efitools \
    >/tmp/apt.log 2>&1 || { FAIL "apt install"; tail -20 /tmp/apt.log; }
echo "apt install: done"

STEP "1. fresh local clone (clean checkout, no build state) + build"
# /src is bind-mounted from a different-UID host; let git read it as a source.
git config --global --add safe.directory /src
git config --global --add safe.directory /src/.git
cd /root
git clone --quiet /src qemu-imx95 || FAIL "git clone"
cd qemu-imx95 || { FAIL "clone produced no dir"; echo ">>> FAILURES"; exit 1; }
git checkout --quiet "$BRANCH" || FAIL "checkout $BRANCH"
echo "commit under test: $(git log -1 --oneline)"
mkdir build && cd build
t0=$(date +%s)
../configure --target-list=aarch64-softmmu >/tmp/configure.log 2>&1 \
    || { FAIL "configure"; tail -25 /tmp/configure.log; }
ninja qemu-system-aarch64 >/tmp/ninja.log 2>&1 \
    || { FAIL "ninja"; tail -25 /tmp/ninja.log; }
echo "build wall time: $(( $(date +%s) - t0 ))s"
cd /root/qemu-imx95
./build/qemu-system-aarch64 -M help | grep -q imx95-19x19-evk \
    && echo "OK: machine registered" || FAIL "machine not registered"

STEP "2a. no-artifact smoke: hello-imx95"
make -C tests/hello-imx95 >/tmp/hello.log 2>&1 || { FAIL "hello build"; tail -10 /tmp/hello.log; }
timeout 15 ./build/qemu-system-aarch64 -M imx95-19x19-evk -nographic -m 2G \
    -kernel tests/hello-imx95/hello.bin 2>/dev/null | grep -m1 "Hello from i.MX 95" \
    && echo "OK: hello printed" || FAIL "hello did not print"

STEP "2b. no-artifact smoke: M7 fingerprint"
make -C tests/cm7-hello TOOLCHAIN=arm-none-eabi- >/tmp/cm7.log 2>&1 \
    || { FAIL "cm7 build"; tail -10 /tmp/cm7.log; }
QEMU=./build/qemu-system-aarch64 timeout 30 tests/m7-boot/run.sh 2>&1 | grep -m1 "0xC0FFEE07" \
    && echo "OK: M7 fingerprint" || FAIL "M7 fingerprint missing"

STEP "3. Tier-1.2 path-cleanliness grep"
HITS=$(git grep -nE '/home/[a-z]+|Documents/' -- '*.sh' '*.py' '*.md' || true)
echo "$HITS"
OTHER=$(echo "$HITS" | grep -v 'validation-todo.md:.*git grep' | grep -c . || true)
[ "$OTHER" = "0" ] && echo "OK: only the self-referential regex line" \
    || FAIL "stray hardcoded path(s): $OTHER"

if [ -e /artifacts/m33_image.elf ]; then
    STEP "4. full Linux boot to userspace (mounted artifacts)"
    export SM_ELF=/artifacts/m33_image.elf KERNEL=/artifacts/Image \
           DTB=/artifacts/imx95-19x19-evk.dtb INITRD=/artifacts/initramfs.cpio.gz
    QEMU=./build/qemu-system-aarch64 timeout 90 tests/swap-boot/run.sh >/tmp/boot.log 2>&1 || true
    grep -aE "SCMI Protocol|NXP:IMX|Run /init|USERSPACE OK|Kernel panic" /tmp/boot.log | head -8
    grep -qaE "Run /init as init process|USERSPACE OK" /tmp/boot.log \
        && echo "OK: reached userspace" \
        || { FAIL "did not reach userspace"; tail -25 /tmp/boot.log; }
else
    STEP "4. full Linux boot - SKIPPED (no /artifacts mounted)"
fi

STEP "VERDICT"
[ "$FAILED" = "0" ] && echo ">>> ALL PASS (pristine container)" || echo ">>> FAILURES ABOVE"
exit $FAILED
