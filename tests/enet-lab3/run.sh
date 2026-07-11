#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The i.MX 95 node for the fleet 3-node raw-L2 segment (mcxn947 + rt1180 + imx95,
# a Cortex-M33 + Cortex-M7 + Cortex-A55 mix sharing ONE wire, each running its
# own vendor firmware). QEMU socket-MCAST is what lets 3+ instances share an L2
# segment (listen/connect is 2-node only). Each node broadcasts a distinct
# ethertype and must OBSERVE both others before it declares PASS.
#
#   Fleet segment : mcast 230.0.0.9:31337
#   ethertypes    : mcx 0x88B5 / rt1180 0x88B6 / imx95 0x88B7
#
# TWO MODES:
#   (default) SELF-TEST - launch THREE local i.MX 95 instances on a PRIVATE mcast
#     group, one as the real imx95 node (0x88B7) and two standing in for the mcx
#     and rt1180 ethertypes. Proves the 95's ENETC socket-mcast datapath AND the
#     lab logic end to end, with no dependency on the other sessions being live -
#     exactly how mcxn proved the segment with three MCX stand-ins before anyone
#     joined. PASS = the imx95 node saw BOTH peer ethertypes.
#   JOIN=1  - launch a SINGLE imx95 node (0x88B7) on the real fleet segment
#     230.0.0.9:31337 to light up the wire for real, once mcx + rt1180 are up.
#
# Required (env): QEMU, KBUILD (Image + dtb + scripts/dtc/dtc), SM_ELF.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=$KBUILD/arch/arm64/boot/Image
DTB=$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb
DTC=$KBUILD/scripts/dtc/dtc
DEFAULT_CPIO=${DEFAULT_CPIO:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
CC=${CC:-aarch64-linux-gnu-gcc}
TMO=${TMO:-240}
JOIN=${JOIN:-0}
FLEET_GROUP=${FLEET_GROUP:-230.0.0.9:31337}
SELF_GROUP=${SELF_GROUP:-230.0.0.95:31395}

skip() { echo "SKIP: $*"; exit 0; }
for f in "$QEMU" "$IMAGE" "$DTC" "$SM_ELF" "$DEFAULT_CPIO" "$DTB"; do
    [ -e "$f" ] || skip "missing $f"
done
command -v "$CC" >/dev/null || skip "no aarch64 cross-compiler ($CC)"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

# --- build the raw-L2 lab tool (static aarch64) ---
"$CC" -static -O2 -Wall "$HERE/enet-lab3.c" -o "$WORK/enet-lab3" || skip "compile failed"

# --- patched dtb: ENETC ports fixed-link so eth probes (shared with run-eth.sh) ---
"$DTC" -I dtb -O dts "$DTB" >"$WORK/base.dts" 2>/dev/null
python3 "$ROOT/tests/netc/patch-dtb.py" "$WORK/base.dts" >"$WORK/enetc.dts"
"$DTC" -I dts -O dtb -o "$WORK/enetc.dtb" "$WORK/enetc.dts" 2>/dev/null || skip "dtb recompile failed"

# --- initramfs: busybox + enet-lab3 + role-from-cmdline init ---
root="$WORK/root"; mkdir -p "$root"/{bin,proc,sys,dev}
( cd "$WORK" && zcat "$DEFAULT_CPIO" | cpio -idmu --quiet 'bin/busybox' )
cp "$WORK/bin/busybox" "$root/bin/busybox"
cp "$WORK/enet-lab3" "$root/enet-lab3"
cat > "$root/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox --install -s /bin 2>/dev/null
ET=$(sed -n 's/.*lab_et=\([^ ]*\).*/\1/p' /proc/cmdline)
PEERS=$(sed -n 's/.*lab_peers=\([^ ]*\).*/\1/p' /proc/cmdline | tr ',' ' ')
DL=$(sed -n 's/.*lab_deadline=\([^ ]*\).*/\1/p' /proc/cmdline)
[ -n "$DL" ] && export LAB_DEADLINE_MS="$DL"
# ENETC PF0 (devfn 00.0) carries the -nic backend; resolve its netdev by PCI
# address (probe-order makes "eth0" unreliable, per run-eth.sh).
n=0; while [ $n -lt 60 ]; do
    IF=$(ls /sys/bus/pci/devices/0002:00:00.0/net 2>/dev/null | head -1)
    [ -n "$IF" ] && break
    sleep 1; n=$((n+1))
done
[ -n "$IF" ] || { echo "ENET-LAB3 FAIL (no netdev on 0002:00:00.0)"; poweroff -f; }
ip link set "$IF" up
sleep 1
echo "ENET-LAB3 boot: if=$IF et=$ET peers=[$PEERS] carrier=$(cat /sys/class/net/$IF/carrier 2>/dev/null)"
/enet-lab3 "$IF" "$ET" $PEERS
echo "ENET-LAB3 rc=$?"
sleep 1; poweroff -f
INIT
chmod +x "$root/init"
( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/lab.cpio.gz"

boot() { # $1=name $2=mcast-group $3=mac $4=my_et $5=peers-csv $6=logfile [$7=deadline_ms]
    timeout --signal=KILL "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
        -kernel "$IMAGE" -dtb "$WORK/enetc.dtb" -initrd "$WORK/lab.cpio.gz" \
        -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init lab_et=$4 lab_peers=$5 lab_deadline=${7:-120000}" \
        -device loader,file="$SM_ELF",cpu-num=6 \
        -nic "socket,mcast=$2,model=fsl-enetc,mac=$3" \
        -serial file:"$6" -serial null -monitor none >/dev/null 2>&1 || true
}

if [ "$JOIN" = 1 ]; then
    echo "== JOIN: i.MX 95 node on the fleet segment $FLEET_GROUP (et 0x88B7) =="
    boot imx95 "$FLEET_GROUP" 02:49:4d:58:95:01 0x88B7 0x88B5,0x88B6 "$WORK/imx95.log"
    echo "--- imx95 ---"; grep -aE 'ENET-LAB3' "$WORK/imx95.log" | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'
    grep -aq 'ENET-LAB3 PASS' "$WORK/imx95.log" && { echo "RESULT: PASS (imx95 saw both fleet peers)"; exit 0; }
    echo "RESULT: FAIL/incomplete - were mcx(0x88B5) and rt1180(0x88B6) both live on $FLEET_GROUP?"; exit 1
fi

if [ "${NEG:-0}" = 1 ]; then
    # Integrity check (mcxn's discipline): with only ONE peer up, the wire is
    # plainly live (imx95 sees 0x88B5 traffic) yet imx95 must NOT declare PASS,
    # because it never saw 0x88B6. "Seeing traffic" is not "seeing both peers";
    # an assertion that cannot fail is worth nothing. Short deadline so it's fast.
    echo "== NEG-TEST: imx95 + only mcx(0x88B5) up on $SELF_GROUP (must NOT pass) =="
    boot imx95   "$SELF_GROUP" 02:49:4d:58:95:01 0x88B7 0x88B5,0x88B6 "$WORK/imx95.log" 25000 &
    boot mcxstub "$SELF_GROUP" 02:4d:43:58:00:01 0x88B5 0x88B7,0x88B6 "$WORK/mcx.log"   25000 &
    wait
    echo "--- imx95 ---"; grep -aE 'ENET-LAB3' "$WORK/imx95.log" | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'
    saw5=$(grep -c 'rx: peer ethertype 0x88b5' "$WORK/imx95.log")
    if grep -aq 'ENET-LAB3 PASS' "$WORK/imx95.log"; then
        echo "RESULT: FAIL (imx95 FALSE-PASSED with only one peer up - assertion is broken)"; exit 1
    fi
    if [ "$saw5" -ge 1 ] && grep -aq 'ENET-LAB3 FAIL: deadline, missing peers: 0x88b6' "$WORK/imx95.log"; then
        echo "RESULT: PASS (wire live - imx95 saw 0x88B5 traffic - yet correctly did NOT pass, missing 0x88B6)"
        exit 0
    fi
    echo "RESULT: INCONCLUSIVE (expected imx95 to see 0x88B5 traffic and fail on missing 0x88B6)"; exit 1
fi

echo "== SELF-TEST: three i.MX 95 instances on private mcast $SELF_GROUP =="
echo "   imx95=0x88B7 (asserted) + stand-ins mcx=0x88B5, rt1180=0x88B6"
boot imx95   "$SELF_GROUP" 02:49:4d:58:95:01 0x88B7 0x88B5,0x88B6 "$WORK/imx95.log"  &
boot mcxstub "$SELF_GROUP" 02:4d:43:58:00:01 0x88B5 0x88B7,0x88B6 "$WORK/mcx.log"    &
boot rtstub  "$SELF_GROUP" 54:27:8d:00:00:00 0x88B6 0x88B7,0x88B5 "$WORK/rt.log"     &
wait

echo "--- imx95 (asserted node) ---"; grep -aE 'ENET-LAB3' "$WORK/imx95.log" | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'
echo "--- mcx stand-in ---";  grep -aE 'ENET-LAB3 (PASS|FAIL)' "$WORK/mcx.log" | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'
echo "--- rt1180 stand-in ---"; grep -aE 'ENET-LAB3 (PASS|FAIL)' "$WORK/rt.log" | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'

if grep -aq 'ENET-LAB3 PASS' "$WORK/imx95.log"; then
    echo "RESULT: PASS (the i.MX 95 ENETC node broadcast 0x88B7 and observed both peer ethertypes on a shared mcast L2 segment)"
    exit 0
fi
echo "RESULT: FAIL - see logs"
cp "$WORK"/imx95.log "$WORK"/mcx.log "$WORK"/rt.log /tmp/ 2>/dev/null
exit 1
