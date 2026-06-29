#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# In-guest build tier, REPRESENTATIVE variant: a real native GCC toolchain
# (glibc gcc/g++ + libstdc++ + libm) on a real Debian rootfs booted off eMMC,
# compiling AND running C and C++ on the A55 - i.e. exactly what a board-farm
# developer does. (run.sh is the lightweight tcc+musl bring-up proof; this is
# the heavyweight "real gcc" proof.)
#
# The aarch64 rootfs comes from the upstream multi-arch `gcc` Docker image:
# `docker pull --platform linux/arm64` fetches a foreign-arch image WITHOUT
# running it, and `docker export` gives its filesystem - a complete native arm64
# gcc with no cross/emulation needed on the host. We stage tests + a tiny init
# into it, build an ext4 image with `mke2fs -d` (no root), and boot it as the
# eMMC root filesystem.
#
# Gotchas baked in:
#   - gcc must be invoked by FULL PATH (/usr/local/bin/gcc): invoked bare it
#     computes a relative exec-prefix and can't find cc1 ("cannot execute 'cc1'").
#   - PATH must include /usr/bin so collect2 finds `ld` at link time.
#   - the rootfs has no init system; we boot init=/igtest.sh directly and the
#     script must never exit (else "Attempted to kill init" panic) - it loops
#     after issuing poweroff.
#   - inject the script via mke2fs -d (rebuild the image), NOT `debugfs write`,
#     which silently truncated the file here.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=${IMAGE:-$KBUILD/arch/arm64/boot/Image}
DTB=${DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
GCC_IMAGE=${GCC_IMAGE:-gcc:14-bookworm}
CACHE=${CACHE:-$ROOT/build/in-guest-build}
MEM=${MEM:-4G}
TMO=${TMO:-420}

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }
for f in "$QEMU" "$IMAGE" "$DTB" "$SM_ELF"; do [ -e "$f" ] || skip "missing $f"; done
command -v docker >/dev/null || skip "no docker (needed to fetch the arm64 gcc rootfs)"
docker info >/dev/null 2>&1 || skip "docker daemon not usable"
command -v mke2fs >/dev/null || skip "no mke2fs"
mkdir -p "$CACHE"

# ---- fetch the native-arm64 gcc rootfs (docker pull+export, cached) ---------
RF=$CACHE/gcc-rootfs
if [ ! -x "$RF/usr/local/bin/gcc" ]; then
    echo "== fetching arm64 rootfs from $GCC_IMAGE (pull does not execute it) =="
    docker pull --platform linux/arm64 "$GCC_IMAGE" >/dev/null 2>&1 || skip "docker pull failed (offline?)"
    cid=$(docker create --platform linux/arm64 "$GCC_IMAGE") || die "docker create failed"
    docker export "$cid" -o "$CACHE/gcc-rootfs.tar" && docker rm "$cid" >/dev/null || die "docker export failed"
    rm -rf "$RF"; mkdir -p "$RF"; tar -C "$RF" -xf "$CACHE/gcc-rootfs.tar" 2>/dev/null
fi
[ -x "$RF/usr/local/bin/gcc" ] || skip "rootfs has no native gcc"

# ---- stage tests + the in-guest runner --------------------------------------
mkdir -p "$RF/opt/igtests"
rm -f "$RF/opt/igtests/"*
cp "$HERE"/gcc-tests/*.c "$HERE"/gcc-tests/*.cpp "$RF/opt/igtests/" 2>/dev/null || true
cat > "$RF/igtest.sh" <<'GUEST'
#!/bin/bash
set +e
mount -t proc proc /proc 2>/dev/null; mount -t devtmpfs dev /dev 2>/dev/null; mount -t sysfs sys /sys 2>/dev/null
exec > /dev/console 2>&1
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
echo "=== NATIVE-GCC IN-GUEST BUILD ==="
/usr/local/bin/gcc --version | head -1; /usr/local/bin/g++ --version | head -1
pass=0; fail=0
for c in /opt/igtests/*.c /opt/igtests/*.cpp; do
  [ -e "$c" ] || continue
  b=$(basename "$c"); exp=$(sed -n 's#^// EXPECT: ##p' "$c"); out=/tmp/${b%.*}
  case "$c" in *.cpp) CC="/usr/local/bin/g++ -O2";; *) CC="/usr/local/bin/gcc -O2";; esac
  if $CC "$c" -o "$out" -lm 2>/tmp/cc.err; then
    got=$("$out" 2>&1)
    if [ "$got" = "$exp" ]; then echo "PASS $b -> $got"; pass=$((pass+1));
    else echo "RUN-FAIL $b: got [$got] want [$exp]"; fail=$((fail+1)); fi
  else echo "BUILD-FAIL $b"; head -3 /tmp/cc.err; fail=$((fail+1)); fi
done
echo "=== IG-RESULT pass=$pass fail=$fail ==="
sync; sleep 1
poweroff -f 2>/dev/null; halt -f 2>/dev/null
echo 1 > /proc/sys/kernel/sysrq 2>/dev/null; echo o > /proc/sysrq-trigger 2>/dev/null
while : ; do sleep 60; done
GUEST
chmod +x "$RF/igtest.sh"

# ---- build the ext4 root image (mke2fs -d, no root needed) ------------------
DISK=$CACHE/gcc-disk.ext4
echo "== building ext4 root image (mke2fs -d) =="
rm -f "$DISK"; truncate -s 3G "$DISK"
mke2fs -F -q -t ext4 -L igroot -d "$RF" "$DISK" >/dev/null 2>&1 || die "mke2fs -d failed"

# ---- boot the rootfs off eMMC; igtest.sh is init ---------------------------
LOG=$(mktemp); trap 'rm -f "$LOG"' EXIT
echo "== booting i.MX 95 with the gcc rootfs as root (/dev/mmcblk0) =="
timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m "$MEM" -display none \
  -kernel "$IMAGE" -dtb "$DTB" \
  -append "console=ttyLP0,115200 root=/dev/mmcblk0 rootwait rootfstype=ext4 rw init=/igtest.sh cpuidle.off=1" \
  -drive if=none,format=raw,file="$DISK",id=mmc0 -device emmc,drive=mmc0 \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "============ NATIVE GCC IN-GUEST BUILD ============"
sed -n '/NATIVE-GCC IN-GUEST BUILD/,/IG-RESULT/p' "$LOG" | grep -avE '^\[ *[0-9]+\.[0-9]+\]'
res=$(grep -aoE 'IG-RESULT pass=[0-9]+ fail=[0-9]+' "$LOG" | tail -1)
[ -n "$res" ] || die "no result marker (boot/build did not complete)"
echo "$res"
echo "$res" | grep -q 'fail=0' && { echo "PASS: A55 built + ran all native-gcc tests"; exit 0; } || die "$res"
