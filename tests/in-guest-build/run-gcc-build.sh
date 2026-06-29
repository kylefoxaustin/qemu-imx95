#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# In-guest build CORPUS: build REAL upstream projects natively on the A55 with the
# gcc rootfs and run each project's OWN test suite. This goes beyond run-gcc.sh's
# toy programs - it proves the emulated i.MX 95 is a working development host:
# download -> ./configure / make -> `make test` passes, all in the guest.
#
# Corpus (classic, self-testing, modest build time under TCG):
#   - bzip2 1.0.8  : make && make test          (upstream compress/decompress self-test)
#   - zlib  1.3.1  : ./configure && make && make test
#   - lua   5.4.7  : make posix, then run a Lua script with a known result
#
# Same rootfs/boot mechanism as run-gcc.sh (arm64 `gcc` docker image -> ext4 ->
# eMMC root). Tarballs are fetched host-side (cached) and staged into the rootfs,
# so the guest needs no network. SKIPs if docker / mke2fs / boot assets / curl are
# missing. Real builds under TCG are slow - the default timeout is generous.
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
TMO=${TMO:-1200}

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }
for f in "$QEMU" "$IMAGE" "$DTB" "$SM_ELF"; do [ -e "$f" ] || skip "missing $f"; done
command -v docker >/dev/null || skip "no docker (needed to fetch the arm64 gcc rootfs)"
docker info >/dev/null 2>&1 || skip "docker daemon not usable"
command -v mke2fs >/dev/null || skip "no mke2fs"
command -v curl >/dev/null || skip "no curl"
mkdir -p "$CACHE"

# ---- native-arm64 gcc rootfs (docker pull+export, cached) -------------------
RF=$CACHE/gcc-rootfs
if [ ! -x "$RF/usr/local/bin/gcc" ]; then
    echo "== fetching arm64 rootfs from $GCC_IMAGE =="
    docker pull --platform linux/arm64 "$GCC_IMAGE" >/dev/null 2>&1 || skip "docker pull failed (offline?)"
    cid=$(docker create --platform linux/arm64 "$GCC_IMAGE") || die "docker create failed"
    docker export "$cid" -o "$CACHE/gcc-rootfs.tar" && docker rm "$cid" >/dev/null || die "docker export failed"
    rm -rf "$RF"; mkdir -p "$RF"; tar -C "$RF" -xf "$CACHE/gcc-rootfs.tar" 2>/dev/null
fi
[ -x "$RF/usr/local/bin/gcc" ] || skip "rootfs has no native gcc"

# ---- fetch + stage the project tarballs (host-side, cached) -----------------
SRC=$CACHE/igsrc; mkdir -p "$SRC"
fetch() { [ -f "$SRC/$1" ] || curl -fsSL "$2" -o "$SRC/$1" || skip "download failed: $1"; tar tzf "$SRC/$1" >/dev/null 2>&1 || skip "bad tarball: $1"; }
fetch bzip2-1.0.8.tar.gz https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz
fetch zlib-1.3.1.tar.gz  https://zlib.net/fossils/zlib-1.3.1.tar.gz
fetch lua-5.4.7.tar.gz   https://www.lua.org/ftp/lua-5.4.7.tar.gz
mkdir -p "$RF/opt/igsrc"; rm -f "$RF/opt/igsrc"/*.tar.gz; cp "$SRC"/*.tar.gz "$RF/opt/igsrc/"

# ---- the in-guest build+test runner (real builds, project self-tests) -------
cat > "$RF/igtest.sh" <<'GUEST'
#!/bin/bash
set +e
mount -t proc proc /proc 2>/dev/null; mount -t devtmpfs dev /dev 2>/dev/null; mount -t sysfs sys /sys 2>/dev/null
exec > /dev/console 2>&1
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
J=$(nproc); GCC=/usr/local/bin/gcc
echo "=== IN-GUEST REAL BUILDS (native gcc $($GCC -dumpversion), -j$J) ==="
pass=0; fail=0; cd /opt/igsrc

echo "--- bzip2: make && make test ---"
( set -e; rm -rf bzip2-1.0.8; tar xf bzip2-1.0.8.tar.gz; cd bzip2-1.0.8; make -j$J CC=$GCC; make test ) >/tmp/bzip2.log 2>&1
if [ $? -eq 0 ]; then echo "PASS bzip2 (built + self-test)"; pass=$((pass+1)); else echo "FAIL bzip2"; tail -6 /tmp/bzip2.log; fail=$((fail+1)); fi

echo "--- zlib: ./configure && make && make test ---"
( set -e; rm -rf zlib-1.3.1; tar xf zlib-1.3.1.tar.gz; cd zlib-1.3.1; CC=$GCC ./configure; make -j$J; make test ) >/tmp/zlib.log 2>&1
if [ $? -eq 0 ]; then echo "PASS zlib (built + self-test)"; pass=$((pass+1)); else echo "FAIL zlib"; tail -6 /tmp/zlib.log; fail=$((fail+1)); fi

echo "--- lua: make posix && run a script ---"
( set -e; rm -rf lua-5.4.7; tar xf lua-5.4.7.tar.gz; cd lua-5.4.7; make -j$J posix CC=$GCC
  echo 'local s=0 for i=1,100 do s=s+i end assert(s==5050) print("LUA-OK "..s)' | ./src/lua ) >/tmp/lua.log 2>&1
if grep -q 'LUA-OK 5050' /tmp/lua.log; then echo "PASS lua (built + ran script)"; pass=$((pass+1)); else echo "FAIL lua"; tail -6 /tmp/lua.log; fail=$((fail+1)); fi

echo "=== IG-RESULT pass=$pass fail=$fail ==="
sync; sleep 1; poweroff -f 2>/dev/null; halt -f 2>/dev/null
echo 1 > /proc/sys/kernel/sysrq 2>/dev/null; echo o > /proc/sysrq-trigger 2>/dev/null
while : ; do sleep 60; done
GUEST
chmod +x "$RF/igtest.sh"

# ---- build the ext4 root image + boot ---------------------------------------
DISK=$CACHE/gcc-build-disk.ext4
echo "== building ext4 root image (mke2fs -d) =="
rm -f "$DISK"; truncate -s 4G "$DISK"
mke2fs -F -q -t ext4 -L igbuild -d "$RF" "$DISK" >/dev/null 2>&1 || die "mke2fs -d failed"

LOG=$(mktemp); trap 'rm -f "$LOG"' EXIT
echo "== booting i.MX 95; building real projects in-guest (slow under TCG) =="
timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m "$MEM" -display none \
  -kernel "$IMAGE" -dtb "$DTB" \
  -append "console=ttyLP0,115200 root=/dev/mmcblk0 rootwait rootfstype=ext4 rw init=/igtest.sh cpuidle.off=1" \
  -drive if=none,format=raw,file="$DISK",id=mmc0 -device emmc,drive=mmc0 \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "============ IN-GUEST REAL BUILDS ============"
sed -n '/IN-GUEST REAL BUILDS/,/IG-RESULT/p' "$LOG" | grep -avE '^\[ *[0-9]+\.[0-9]+\]'
res=$(grep -aoE 'IG-RESULT pass=[0-9]+ fail=[0-9]+' "$LOG" | tail -1)
[ -n "$res" ] || die "no result marker (boot/builds did not complete - try a larger TMO)"
echo "$res"
echo "$res" | grep -q 'fail=0' && { echo "PASS: A55 natively built + tested all corpus projects"; exit 0; } || die "$res"
