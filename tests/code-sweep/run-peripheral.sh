#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# code-sweep PERIPHERAL tier: run real third-party code that drives the i.MX 95's
# actual peripheral datapaths (not just the CPU). Same build/scoreboard model as
# run.sh, but the machine boots with extra devices attached and the guest wires
# them up before running the corpus:
#
#   - an eMMC card (ext4, pre-formatted host-side) on the uSDHC controller,
#     mounted at /data in the guest -> exercises the SD/eMMC write+read ADMA path
#   - user networking on the ENETC NIC (slirp; host reachable at 10.0.2.2)
#     -> exercises the from-scratch ENETC TX/RX datapath
#
# Recipes live in recipes-peripheral/ and use /data and eth0 from their
# runtest.sh. The rationale (per the MCXN947 sweep): real drivers + real apps
# surface model bugs that word-aligned synthetic tests never hit.
#
# Usage: tests/code-sweep/run-peripheral.sh [name ...]
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
CC=${CC:-${CROSS}gcc}; AR=${AR:-${CROSS}ar}; STRIP=${STRIP:-${CROSS}strip}
TMO=${TMO:-900}; CASE_TMO=${CASE_TMO:-300}
EMMC_MB=${EMMC_MB:-256}
CACHE=${CACHE:-$ROOT/build/code-sweep/cache}
RDIR="$HERE/recipes-peripheral"

skip() { echo "SKIP: $*"; exit 0; }
[ -x "$QEMU" ] || skip "no QEMU ($QEMU)"; [ -e "$IMAGE" ] || skip "no Image"
[ -e "$DTB" ] || skip "no dtb"; [ -e "$SM_ELF" ] || skip "no SM firmware"
[ -e "$INITRD" ] || skip "no busybox initramfs"
command -v "$CC" >/dev/null || skip "no cross compiler ($CC)"
command -v mke2fs >/dev/null || skip "mke2fs needed to format the eMMC image"

if [ "$#" -gt 0 ]; then
    RECIPES=""
    for n in "$@"; do
        [ -f "$RDIR/$n.recipe" ] || { echo "no such peripheral recipe: $n"; exit 2; }
        RECIPES="$RECIPES $RDIR/$n.recipe"
    done
else
    RECIPES=$(ls "$RDIR"/*.recipe 2>/dev/null)
fi
[ -n "${RECIPES// }" ] || skip "no peripheral recipes found"

mkdir -p "$CACHE"
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
SHARE="$WORK/share"; mkdir -p "$SHARE/items" "$SHARE/results"
SRCWORK="$WORK/src"; mkdir -p "$SRCWORK"

# ---- host: fetch + cross-build the peripheral recipes ----------------------
echo "=== code-sweep peripheral: building corpus (cross: $CC) ==="
FAILED_BUILD=""
for rfile in $RECIPES; do
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
        tar -xf "$tarball" -C "$ex" --strip-components=1 || { echo "  EXTRACT-FAIL $NAME"; exit 5; }
        OUT="$SHARE/items/$NAME"; mkdir -p "$OUT"; export CC AR STRIP OUT
        echo "  build  $NAME"
        ( cd "$ex" && cs_build ) || { echo "  BUILD-FAIL $NAME"; exit 6; }
        [ -x "$OUT/runtest.sh" ] || { echo "  NO-RUNTEST $NAME"; exit 7; }
    ) || FAILED_BUILD="$FAILED_BUILD $(basename "$rfile" .recipe)"
done
ITEMS=$(cd "$SHARE/items" && ls 2>/dev/null)
[ -n "$ITEMS" ] || skip "no peripheral items built"

# ---- prepare the eMMC card image (ext4, no root needed) --------------------
EMMC="$WORK/emmc.img"
truncate -s "${EMMC_MB}M" "$EMMC"
mke2fs -F -q -t ext4 "$EMMC" >/dev/null 2>&1 || skip "mke2fs failed"
echo "=== eMMC: ${EMMC_MB}M ext4 image ready; NIC: -nic user (host=10.0.2.2) ==="

# ---- stage the busybox initramfs runner -----------------------------------
STAGE="$WORK/root"; mkdir -p "$STAGE"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
{ echo "#!/bin/busybox sh"; echo "CASE_TMO=$CASE_TMO"; cat <<'INIT'
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "=== CODE-SWEEP-PERIPHERAL ==="
sleep 3
mkdir -p /mnt; mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt 2>&1
echo "9p rc=$?"
# Mount the eMMC ext4 card at /data (first mmcblk that mounts).
mkdir -p /data; mounted=0
i=0; while [ $i -lt 30 ]; do ls /dev/mmcblk* >/dev/null 2>&1 && break; sleep 1; i=$((i+1)); done
for d in /dev/mmcblk*; do
    [ -b "$d" ] || continue
    case "$d" in *p[0-9]*) continue;; esac     # whole-disk node only
    if mount -t ext4 "$d" /data 2>/dev/null; then echo "eMMC mounted: $d -> /data"; mounted=1; break; fi
done
[ "$mounted" = 1 ] || echo "WARN: no eMMC ext4 mounted (storage tests will skip)"
# Bring up eth0 over the ENETC NIC (best-effort DHCP via slirp).
ip link set eth0 up 2>/dev/null
udhcpc -i eth0 -n -q -t 5 -T 2 >/dev/null 2>&1 && echo "eth0 up: $(ip -4 addr show eth0 2>/dev/null | grep -o 'inet [0-9.]*')" || echo "WARN: eth0 DHCP failed (net tests will skip)"
if timeout 1 true 2>/dev/null; then TFORM=new; else TFORM=old; fi
run_to() { if [ "$TFORM" = new ]; then timeout "$CASE_TMO" "$@"; else timeout -t "$CASE_TMO" "$@"; fi; }
cd /mnt/items || { echo "no items"; /bin/busybox poweroff -f; }
for d in */; do
    item=${d%/}; [ -x "$item/runtest.sh" ] || continue
    echo "----8<---- ITEM $item ----8<----"
    ( cd "$item" && run_to ./runtest.sh ) > "/mnt/results/$item.log" 2>&1
    rc=$?; echo "$rc" > "/mnt/results/$item.rc"
    cat "/mnt/results/$item.log"; echo "----8<---- $item rc=$rc ----8<----"
done
sync
echo "=== CODE-SWEEP-PERIPHERAL-DONE ==="
/bin/busybox poweroff -f
INIT
} > "$STAGE/init"
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.gz"

# ---- boot with eMMC + NIC attached ----------------------------------------
LOG="$WORK/serial.log"
echo "=== booting i.MX 95 (eMMC + ENETC NIC); running peripheral corpus ==="
timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -fsdev local,id=fsdev0,path="$SHARE",security_model=none \
  -device virtio-9p-device,fsdev=fsdev0,mount_tag=hostshare \
  -drive if=none,format=raw,file="$EMMC",id=mmc0 -device emmc,drive=mmc0 \
  -nic user,model=fsl-enetc \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

# ---- scoreboard ------------------------------------------------------------
echo
echo "============ CODE-SWEEP PERIPHERAL SCOREBOARD ============"
pass=0; skp=0; fail=0; bld=0; failed=""
for i in $ITEMS; do
    rcf="$SHARE/results/$i.rc"; rc=$( [ -f "$rcf" ] && cat "$rcf" || echo n/a )
    case "$rc" in
        0)   printf "  PASS  %s\n" "$i"; pass=$((pass+1)) ;;
        77)  printf "  SKIP  %s\n" "$i"; skp=$((skp+1)) ;;
        124) printf "  FAIL  %s (timeout)\n" "$i"; fail=$((fail+1)); failed="$failed $i" ;;
        *)   printf "  FAIL  %s (rc=%s)\n" "$i" "$rc"; fail=$((fail+1)); failed="$failed $i" ;;
    esac
done
for b in $FAILED_BUILD; do printf "  BLDFAIL %s\n" "$b"; bld=$((bld+1)); done
echo "---------------------------------------------------------"
echo "  PASS $pass  SKIP $skp  FAIL $fail  BLDFAIL $bld"
echo "========================================================="
grep -qa 'CODE-SWEEP-PERIPHERAL-DONE' "$LOG" || { echo "FAIL: guest did not finish"; tail -n 25 "$LOG"; exit 1; }
if [ -n "$failed" ]; then
    echo "FAIL:$failed"
    for i in $failed; do echo "----- $i -----"; sed -n '1,40p' "$SHARE/results/$i.log"; done
    exit 1
fi
[ "$bld" -eq 0 ] || { echo "FAIL: build failures:$FAILED_BUILD"; exit 1; }
echo "PASS: $pass peripheral item(s) verified on real datapaths${skp:+ ($skp skipped)}"
