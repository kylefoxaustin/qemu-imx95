#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# code-sweep: build real third-party source and run it ON the i.MX 95 machine,
# checking each program against its own known-good oracle.
#
# For every recipe in recipes/ this:
#   1. fetches the upstream source tarball (cached under build/code-sweep/cache),
#   2. CROSS-COMPILES it statically on the host (aarch64-linux-gnu-gcc) into a
#      per-item staging dir, alongside a guest-side runtest.sh that carries the
#      program's built-in correctness oracle (self-test / round-trip / KAT),
#   3. exposes the whole staging tree to the guest over virtio-9p,
#   4. boots the busybox initramfs, mounts the share, and runs every item's
#      runtest.sh in-guest, writing pass/fail + output back over 9p,
#   5. parses the results and prints a scoreboard.
#
# This is the "cross-compile on host -> run in guest" tier: it proves the A55
# executes real software correctly. (An in-guest build tier is future work and
# needs a toolchain rootfs; the recipe format is designed to host it too.)
#
# Usage:
#   tests/code-sweep/run.sh                 # all recipes
#   tests/code-sweep/run.sh zlib bzip2      # a subset by NAME
#
# Required (override via env): QEMU, KBUILD (Image + dtb), SM_ELF, a busybox
# initramfs, and an aarch64 cross gcc. SKIPs (rc=0) if any is missing.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=${IMAGE:-$KBUILD/arch/arm64/boot/Image}
DTB=${DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
INITRD=${INITRD:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
CROSS=${CROSS:-aarch64-linux-gnu-}
CC=${CC:-${CROSS}gcc}
AR=${AR:-${CROSS}ar}
STRIP=${STRIP:-${CROSS}strip}
TMO=${TMO:-600}
CACHE=${CACHE:-$ROOT/build/code-sweep/cache}

skip() { echo "SKIP: $*"; exit 0; }
[ -x "$QEMU" ]            || skip "no QEMU ($QEMU)"
[ -e "$IMAGE" ]          || skip "no kernel Image ($IMAGE)"
[ -e "$DTB" ]            || skip "no dtb ($DTB)"
[ -e "$SM_ELF" ]         || skip "no SM firmware ($SM_ELF)"
[ -e "$INITRD" ]         || skip "no busybox initramfs ($INITRD)"
command -v "$CC" >/dev/null || skip "no cross compiler ($CC)"
command -v curl >/dev/null  || skip "curl needed to fetch sources"

# Which recipes to run.
if [ "$#" -gt 0 ]; then
    RECIPES=""
    for n in "$@"; do
        [ -f "$HERE/recipes/$n.recipe" ] || { echo "no such recipe: $n"; exit 2; }
        RECIPES="$RECIPES $HERE/recipes/$n.recipe"
    done
else
    RECIPES=$(ls "$HERE"/recipes/*.recipe 2>/dev/null)
fi
[ -n "${RECIPES// }" ] || skip "no recipes found"

mkdir -p "$CACHE"
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
SHARE="$WORK/share"; mkdir -p "$SHARE/items" "$SHARE/results"
SRCWORK="$WORK/src"; mkdir -p "$SRCWORK"

# ---- host: fetch + cross-build every recipe -------------------------------
echo "=== code-sweep: building corpus (cross: $CC) ==="
BUILT=0; FAILED_BUILD=""
for rfile in $RECIPES; do
    # Recipe variables/functions live in a subshell so they can't leak.
    (
        NAME=""; CATEGORY=""; SRC=""; REF=""
        # shellcheck disable=SC1090
        . "$rfile"
        [ -n "$NAME" ] && [ -n "$SRC" ] || { echo "  bad recipe: $rfile"; exit 3; }

        tarball="$CACHE/$NAME-$REF.tar.gz"
        if [ ! -s "$tarball" ]; then
            echo "  fetch  $NAME ($REF)"
            curl -fsSL "$SRC" -o "$tarball.tmp" && mv "$tarball.tmp" "$tarball" \
                || { echo "  FETCH-FAIL $NAME"; exit 4; }
        fi

        ex="$SRCWORK/$NAME"; mkdir -p "$ex"
        tar -xzf "$tarball" -C "$ex" --strip-components=1 \
            || { echo "  EXTRACT-FAIL $NAME"; exit 5; }

        OUT="$SHARE/items/$NAME"; mkdir -p "$OUT"
        export CC AR STRIP OUT
        echo "  build  $NAME"
        ( cd "$ex" && cs_build ) || { echo "  BUILD-FAIL $NAME"; exit 6; }
        [ -x "$OUT/runtest.sh" ] || { echo "  NO-RUNTEST $NAME"; exit 7; }
        echo "$NAME" > "$OUT/.name"
    ) && BUILT=$((BUILT+1)) || FAILED_BUILD="$FAILED_BUILD $(basename "$rfile" .recipe)"
done

ITEMS=$(cd "$SHARE/items" && ls 2>/dev/null)
[ -n "$ITEMS" ] || skip "no items built successfully"
echo "=== built $BUILT item(s):$( for i in $ITEMS; do printf ' %s' "$i"; done) ==="
[ -n "$FAILED_BUILD" ] && echo "=== host build failures:$FAILED_BUILD ==="

# ---- stage the busybox initramfs with our in-guest runner -----------------
STAGE="$WORK/root"; mkdir -p "$STAGE"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "=== CODE-SWEEP ==="
sleep 2
mkdir -p /mnt
mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt 2>&1
echo "mount rc=$?"
cd /mnt/items || { echo "no items dir"; /bin/busybox poweroff -f; }
for d in */; do
    item=${d%/}
    [ -x "$item/runtest.sh" ] || continue
    echo "----8<---- ITEM $item ----8<----"
    ( cd "$item" && ./runtest.sh ) > "/mnt/results/$item.log" 2>&1
    rc=$?
    echo "$rc" > "/mnt/results/$item.rc"
    cat "/mnt/results/$item.log"
    echo "----8<---- $item rc=$rc ----8<----"
done
sync
echo "=== CODE-SWEEP-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.gz"

# ---- boot: run the whole corpus in-guest ----------------------------------
LOG="$WORK/serial.log"
echo "=== booting i.MX 95; running corpus in-guest ==="
timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -fsdev local,id=fsdev0,path="$SHARE",security_model=none \
  -device virtio-9p-device,fsdev=fsdev0,mount_tag=hostshare \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

# ---- scoreboard ------------------------------------------------------------
echo
echo "================= CODE-SWEEP SCOREBOARD ================="
pass=0; total=0; failed=""
for i in $ITEMS; do
    total=$((total+1))
    rcf="$SHARE/results/$i.rc"
    if [ -f "$rcf" ] && [ "$(cat "$rcf")" = "0" ]; then
        printf "  PASS  %s\n" "$i"; pass=$((pass+1))
    else
        rc=$( [ -f "$rcf" ] && cat "$rcf" || echo "n/a" )
        printf "  FAIL  %s (rc=%s)\n" "$i" "$rc"; failed="$failed $i"
    fi
done
echo "--------------------------------------------------------"
echo "  $pass/$total passed"
echo "========================================================"

if ! grep -qa '=== CODE-SWEEP-DONE ===' "$LOG"; then
    echo "FAIL: guest did not finish (timeout/crash). Last serial lines:"
    tail -n 25 "$LOG"
    exit 1
fi
if [ -n "$failed" ]; then
    echo "FAIL: failing items:$failed"
    for i in $failed; do
        echo "----- $i log -----"; sed -n '1,40p' "$SHARE/results/$i.log" 2>/dev/null
    done
    exit 1
fi
echo "PASS: all $total corpus item(s) built on host and verified in-guest"
