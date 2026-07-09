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
DTC=${DTC:-$KBUILD/scripts/dtc/dtc}
INITRD=${INITRD:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
CROSS=${CROSS:-aarch64-linux-gnu-}
CC=${CC:-${CROSS}gcc}; AR=${AR:-${CROSS}ar}; STRIP=${STRIP:-${CROSS}strip}
TMO=${TMO:-900}; CASE_TMO=${CASE_TMO:-300}
EMMC_MB=${EMMC_MB:-256}
ITERS=${ITERS:-1}             # soak: build once, boot+run this many times
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
WORK=$(mktemp -d)
# KEEPWORK=1 preserves the guest serial logs for post-mortem.
if [ -z "${KEEPWORK:-}" ]; then trap 'rm -rf "$WORK"' EXIT; else echo "KEEPWORK: $WORK"; fi
SHARE="$WORK/share"; mkdir -p "$SHARE/items" "$SHARE/results"
SRCWORK="$WORK/src"; mkdir -p "$SRCWORK"

# ---- host: fetch + cross-build the peripheral recipes ----------------------
echo "=== code-sweep peripheral: building corpus (cross: $CC) ==="
FAILED_BUILD=""
for rfile in $RECIPES; do
    (
        NAME=""; CATEGORY=""; SRC=""; REF=""; CS_KEY=""
        # shellcheck disable=SC1090
        . "$rfile"
        [ -n "$NAME" ] || { echo "  bad recipe: $rfile"; exit 3; }
        ex="$SRCWORK/$NAME"; mkdir -p "$ex"
        # SRC is optional: a no-source recipe (e.g. a small kernel-UABI datapath
        # test) compiles an inline C program in cs_build instead of fetching.
        if [ -n "$SRC" ]; then
            tarball="$CACHE/$NAME-$REF.tar.gz"
            if [ ! -s "$tarball" ]; then
                echo "  fetch  $NAME ($REF)"
                curl -fsSL "$SRC" -o "$tarball.tmp" && mv "$tarball.tmp" "$tarball" \
                    || { echo "  FETCH-FAIL $NAME"; exit 4; }
            fi
            tar -xf "$tarball" -C "$ex" --strip-components=1 || { echo "  EXTRACT-FAIL $NAME"; exit 5; }
        fi
        OUT="$SHARE/items/$NAME"; mkdir -p "$OUT"; export CC AR STRIP OUT KBUILD ROOT
        echo "  build  $NAME"
        ( cd "$ex" && cs_build ) || { echo "  BUILD-FAIL $NAME"; exit 6; }
        [ -x "$OUT/runtest.sh" ] || { echo "  NO-RUNTEST $NAME"; exit 7; }
        echo "${CS_KEY:-$NAME}" > "$OUT/.key"   # shared cross-machine dashboard key
    ) || FAILED_BUILD="$FAILED_BUILD $(basename "$rfile" .recipe)"
done
ITEMS=$(cd "$SHARE/items" && ls 2>/dev/null)
[ -n "$ITEMS" ] || skip "no peripheral items built"

# ---- prepare the eMMC card image (ext4, no root needed) --------------------
EMMC="$WORK/emmc.img"
truncate -s "${EMMC_MB}M" "$EMMC"
mke2fs -F -q -t ext4 "$EMMC" >/dev/null 2>&1 || skip "mke2fs failed"

# ---- a blank USB mass-storage disk (for the usb-storage datapath test) -----
USBIMG="$WORK/usb.img"; truncate -s 64M "$USBIMG"

# ---- TFTP root served by slirp at 10.0.2.2 (for net datapath tests) --------
TFTPDIR="$WORK/tftp"; mkdir -p "$TFTPDIR"
head -c 262144 /dev/urandom > "$TFTPDIR/netpayload.bin" 2>/dev/null \
    || dd if=/dev/zero bs=1024 count=256 2>/dev/null | tr '\0' 'N' > "$TFTPDIR/netpayload.bin"
sha256sum "$TFTPDIR/netpayload.bin" | awk '{print $1}' > "$TFTPDIR/netpayload.sha"
echo "=== eMMC: ${EMMC_MB}M ext4 ready; NIC: -nic user (host=10.0.2.2, tftp on) ==="

# ---- patched dtb so the ENETC ports actually probe (fixed MAC + identity
#      msi-map; the stock nvmem MAC never resolves under emulation). Reuse the
#      netc test's splice; fall back to the stock dtb if dtc/patch are absent. --
DTB_USE="$DTB"
NETC_PATCH="$ROOT/tests/netc/patch-dtb.py"
if [ -x "$DTC" ] && [ -f "$NETC_PATCH" ] && command -v python3 >/dev/null; then
    if "$DTC" -I dtb -O dts "$DTB" > "$WORK/base.dts" 2>/dev/null \
       && python3 "$NETC_PATCH" "$WORK/base.dts" > "$WORK/netc.dts" 2>/dev/null \
       && "$DTC" -I dts -O dtb -o "$WORK/netc.dtb" "$WORK/netc.dts" 2>/dev/null; then
        DTB_USE="$WORK/netc.dtb"; echo "=== dtb: ENETC ports patched (fixed MAC + identity msi-map) ==="
    fi
fi

# ---- stage the busybox initramfs runner -----------------------------------
STAGE="$WORK/root"; mkdir -p "$STAGE"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
# ethN must mean a fixed ENETC port: Linux names netdevs in probe order, so a
# bare "eth0" is not reliably the PF the -nic backend is attached to. A miss
# here shows up as "eth0 DHCP failed (net tests will skip)" - a silent skip.
install -m 0755 "$ROOT/tests/lib/guest-enetc-names.sh" "$STAGE/bin/enetc-names"
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
/bin/enetc-names
ip link set eth0 up 2>/dev/null
udhcpc -i eth0 -n -q -t 5 -T 2 >/dev/null 2>&1 && echo "eth0 up: $(ip -4 addr show eth0 2>/dev/null | grep -o 'inet [0-9.]*')" || echo "WARN: eth0 DHCP failed (net tests will skip)"
if timeout 1 true 2>/dev/null; then TFORM=new; else TFORM=old; fi
run_to() { if [ "$TFORM" = new ]; then timeout "$CASE_TMO" "$@"; else timeout -t "$CASE_TMO" "$@"; fi; }
cd /mnt/items || { echo "no items"; /bin/busybox poweroff -f; }
for d in */; do
    item=${d%/}; [ -x "$item/runtest.sh" ] || continue
    key=$(cat "$item/.key" 2>/dev/null || echo "$item")
    echo "----8<---- ITEM $item ($key) ----8<----"
    ( cd "$item" && run_to ./runtest.sh ) > "/mnt/results/$item.log" 2>&1
    rc=$?; echo "$rc" > "/mnt/results/$item.rc"
    cat "/mnt/results/$item.log"
    # Cross-machine scorer marker (91/95 shared format): SOAK:<R>:<name>:<detail>
    if   [ "$rc" -eq 0 ];  then R=PASS
    elif [ "$rc" -eq 77 ]; then R=SKIP
    else R=FAIL; fi
    echo "SOAK:$R:$key:rc=$rc"
    echo "----8<---- $item rc=$rc ----8<----"
done
sync
echo "SOAK:BATTERY:DONE"
echo "=== CODE-SWEEP-PERIPHERAL-DONE ==="
/bin/busybox poweroff -f
INIT
} > "$STAGE/init"
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.gz"

# ---- boot + run, ITERS times (soak); accumulate into one STATE dashboard ----
# STATE persists across iterations so the 91/95 dashboard shows N-deep tallies
# (e.g. gpio 20/0/0 after a 20-boot soak). Any iteration with a FAIL fails it.
STATE="$WORK/state"; mkdir -p "$STATE"
bump() { local key="$1.$2" n; n=$(cat "$STATE/$key" 2>/dev/null || echo 0); echo $((n+1)) > "$STATE/$key"; }
soak_fail=""; iter=1
[ "$ITERS" -gt 1 ] && echo "=== SOAK: $ITERS iterations of the peripheral corpus ==="
while [ "$iter" -le "$ITERS" ]; do
    LOG="$WORK/serial-$iter.log"
    echo "=== [iter $iter/$ITERS] booting i.MX 95 (eMMC + USB + ENETC NIC) ==="
    timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
      -kernel "$IMAGE" -dtb "$DTB_USE" -initrd "$WORK/initrd.gz" \
      -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
      -device loader,file="$SM_ELF",cpu-num=6 \
      -fsdev local,id=fsdev0,path="$SHARE",security_model=none \
      -device virtio-9p-device,fsdev=fsdev0,mount_tag=hostshare \
      -drive if=none,format=raw,file="$EMMC",id=mmc0 -device emmc,drive=mmc0 \
      -drive if=none,format=raw,file="$USBIMG",id=usbdisk \
      -device usb-storage,bus=usb-bus.0,drive=usbdisk \
      -nic user,model=fsl-enetc,tftp="$TFTPDIR" \
      -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

    ipass=0; iskp=0; ifail=0; ifailed=""
    for i in $ITEMS; do
        rcf="$SHARE/results/$i.rc"; rc=$( [ -f "$rcf" ] && cat "$rcf" || echo n/a )
        case "$rc" in
            0)   ipass=$((ipass+1)) ;;
            77)  iskp=$((iskp+1)) ;;
            *)   ifail=$((ifail+1)); ifailed="$ifailed $i" ;;
        esac
    done
    # accumulate the in-guest SOAK markers (per-key) into STATE
    while IFS= read -r line; do
        case "$line" in
            SOAK:PASS:*) n="${line#SOAK:PASS:}"; bump "${n%%:*}" pass ;;
            SOAK:FAIL:*) n="${line#SOAK:FAIL:}"; bump "${n%%:*}" fail ;;
            SOAK:SKIP:*) n="${line#SOAK:SKIP:}"; bump "${n%%:*}" skip ;;
        esac
    done < <(grep -a '^SOAK:' "$LOG")
    if ! grep -qa 'CODE-SWEEP-PERIPHERAL-DONE' "$LOG"; then
        echo "  [iter $iter] FAIL: guest did not finish"; tail -n 15 "$LOG" | sed 's/^/    /'
        soak_fail="$soak_fail iter$iter(nofinish)"
    elif [ -n "$ifailed" ]; then
        echo "  [iter $iter] FAIL:$ifailed"
        for i in $ifailed; do echo "    --- $i ---"; sed -n '1,30p' "$SHARE/results/$i.log" | sed 's/^/    /'; done
        soak_fail="$soak_fail iter$iter($ifailed )"
    else
        echo "  [iter $iter] PASS $ipass  SKIP $iskp  FAIL 0"
    fi
    iter=$((iter+1))
done

# ---- accumulated cross-machine dashboard (91emulator's SOAK scorer) ---------
echo
echo "======= peripheral dashboard (91/95 keys, $ITERS iter) ======="
printf "  %-20s %6s %6s %6s\n" function PASS FAIL SKIP
for f in $(ls "$STATE" 2>/dev/null | sed -E 's/\.(pass|fail|skip)$//' | sort -u); do
    p=$(cat "$STATE/$f.pass" 2>/dev/null || echo 0)
    fl=$(cat "$STATE/$f.fail" 2>/dev/null || echo 0)
    s=$(cat "$STATE/$f.skip" 2>/dev/null || echo 0)
    printf "  %-20s %6s %6s %6s\n" "$f" "$p" "$fl" "$s"
done
echo "============================================================="
cat "$STATE"/*.* >/dev/null 2>&1 && cat <(for f in $(ls "$STATE"|sed -E 's/\.(pass|fail|skip)$//'|sort -u); do echo "SOAK:$f:pass=$(cat $STATE/$f.pass 2>/dev/null||echo 0):fail=$(cat $STATE/$f.fail 2>/dev/null||echo 0)"; done) > "$CACHE/../peripheral-soak.log" 2>/dev/null || true

for b in $FAILED_BUILD; do echo "  BLDFAIL $b"; done
if [ -n "$soak_fail" ] || [ -n "$FAILED_BUILD" ]; then
    echo "FAIL (soak):$soak_fail${FAILED_BUILD:+ build:$FAILED_BUILD}"
    exit 1
fi
echo "PASS: peripheral corpus green across $ITERS iteration(s)"
