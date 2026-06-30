#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# In-guest build tier, clang variant: a native LLVM/clang toolchain on the A55,
# compiling AND running C and C++ in the guest. Complements run-gcc.sh (gcc) so
# the board farm proves both major toolchains self-host. Same rootfs/boot
# mechanism as run-gcc.sh (an arm64 docker image -> ext4 -> eMMC root).
#
# clang notes baked in:
#   - the silkeh/clang image ships lld but not GNU ld, so link with -fuse-ld=lld;
#   - PATH must include the LLVM bindir so clang finds lld at link;
#   - link -static? no - the rootfs has a dynamic loader (real root fs), so plain
#     dynamic linking is fine here (unlike the busybox-initramfs tcc tier).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=${IMAGE:-$KBUILD/arch/arm64/boot/Image}
DTB=${DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
CLANG_IMAGE=${CLANG_IMAGE:-silkeh/clang:19}
CACHE=${CACHE:-$ROOT/build/in-guest-build}
MEM=${MEM:-4G}
TMO=${TMO:-420}

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }
for f in "$QEMU" "$IMAGE" "$DTB" "$SM_ELF"; do [ -e "$f" ] || skip "missing $f"; done
command -v docker >/dev/null || skip "no docker (needed to fetch the arm64 clang rootfs)"
docker info >/dev/null 2>&1 || skip "docker daemon not usable"
command -v mke2fs >/dev/null || skip "no mke2fs"
mkdir -p "$CACHE"

# ---- native-arm64 clang rootfs (docker pull+export, cached) -----------------
RF=$CACHE/clang-rootfs
if [ ! -x "$RF/usr/bin/clang" ]; then
    echo "== fetching arm64 rootfs from $CLANG_IMAGE =="
    docker pull --platform linux/arm64 "$CLANG_IMAGE" >/dev/null 2>&1 || skip "docker pull failed (offline?)"
    cid=$(docker create --platform linux/arm64 "$CLANG_IMAGE") || die "docker create failed"
    docker export "$cid" -o "$CACHE/clang-rootfs.tar" && docker rm "$cid" >/dev/null || die "docker export failed"
    rm -rf "$RF"; mkdir -p "$RF"; tar -C "$RF" -xf "$CACHE/clang-rootfs.tar" 2>/dev/null
fi
[ -x "$RF/usr/bin/clang" ] || skip "rootfs has no native clang"

# ---- stage tests + the in-guest runner --------------------------------------
mkdir -p "$RF/opt/igtests"; rm -f "$RF/opt/igtests/"*
cp "$HERE"/clang-tests/*.c "$HERE"/clang-tests/*.cpp "$RF/opt/igtests/" 2>/dev/null || true
cat > "$RF/igtest.sh" <<'GUEST'
#!/bin/bash
set +e
mount -t proc proc /proc 2>/dev/null; mount -t devtmpfs dev /dev 2>/dev/null; mount -t sysfs sys /sys 2>/dev/null
exec > /dev/console 2>&1
export PATH=/usr/lib/llvm-19/bin:/usr/bin:/bin:/usr/sbin:/sbin
echo "=== CLANG IN-GUEST BUILD ==="
clang --version | head -1
pass=0; fail=0
for c in /opt/igtests/*.c /opt/igtests/*.cpp; do
  [ -e "$c" ] || continue
  b=$(basename "$c"); exp=$(sed -n 's#^// EXPECT: ##p' "$c"); out=/tmp/${b%.*}
  case "$c" in *.cpp) CC="clang++ -O2 -fuse-ld=lld";; *) CC="clang -O2 -fuse-ld=lld -lm";; esac
  if $CC "$c" -o "$out" 2>/tmp/cc.err; then
    got=$("$out" 2>&1)
    if [ "$got" = "$exp" ]; then echo "PASS $b -> $got"; pass=$((pass+1));
    else echo "RUN-FAIL $b: got [$got] want [$exp]"; fail=$((fail+1)); fi
  else echo "BUILD-FAIL $b"; head -4 /tmp/cc.err; fail=$((fail+1)); fi
done
echo "=== IG-RESULT pass=$pass fail=$fail ==="
sync; sleep 1; poweroff -f 2>/dev/null; halt -f 2>/dev/null
echo 1 > /proc/sys/kernel/sysrq 2>/dev/null; echo o > /proc/sysrq-trigger 2>/dev/null
while : ; do sleep 60; done
GUEST
chmod +x "$RF/igtest.sh"

# ---- build the ext4 root image + boot ---------------------------------------
DISK=$CACHE/clang-disk.ext4
echo "== building ext4 root image (mke2fs -d) =="
rm -f "$DISK"; truncate -s 3G "$DISK"
mke2fs -F -q -t ext4 -L igclang -d "$RF" "$DISK" >/dev/null 2>&1 || die "mke2fs -d failed"

LOG=$(mktemp); trap 'rm -f "$LOG"' EXIT
echo "== booting i.MX 95 with the clang rootfs as root (/dev/mmcblk0) =="
timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m "$MEM" -display none \
  -kernel "$IMAGE" -dtb "$DTB" \
  -append "console=ttyLP0,115200 root=/dev/mmcblk0 rootwait rootfstype=ext4 rw init=/igtest.sh cpuidle.off=1" \
  -drive if=none,format=raw,file="$DISK",id=mmc0 -device emmc,drive=mmc0 \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "============ CLANG IN-GUEST BUILD ============"
sed -n '/CLANG IN-GUEST BUILD/,/IG-RESULT/p' "$LOG" | grep -avE '^\[ *[0-9]+\.[0-9]+\]'
res=$(grep -aoE 'IG-RESULT pass=[0-9]+ fail=[0-9]+' "$LOG" | tail -1)
[ -n "$res" ] || die "no result marker (boot/build did not complete)"
echo "$res"
echo "$res" | grep -q 'fail=0' && { echo "PASS: A55 built + ran all clang tests"; exit 0; } || die "$res"
