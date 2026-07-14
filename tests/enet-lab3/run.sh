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
#   (default) INTEROP SELF-TEST - three local i.MX 95 instances on a PRIVATE
#     mcast group. Ours (0x88B7) runs OUR node; BOTH STAND-INS RUN 91EMULATOR'S
#     OWN BEACON, built from their source, with BEACON_STRICT=1.
#
#     This used to boot three copies of our own tool, and that is precisely why
#     it was green for weeks while we were UNINTEROPERABLE: we emitted an ASCII
#     payload where the fleet had agreed a binary body, our own checker happily
#     accepted our own garbage, and both real peers correctly condemned us - so
#     NEITHER OF THEM COULD EVER PASS. A suite can be exhaustive within its own
#     model of the world and still be blind BY CONSTRUCTION to everything
#     outside it. The only local oracle that can see a body we got wrong is an
#     implementation WE DID NOT WRITE.
#
#     PASS requires all three: (1) 91's checker never calls our frames CORRUPT,
#     (2) both independent peers actually COUNT us, and (3) we re-arm (>= 2 PASS
#     beats) rather than latching.
#   JOIN=1  - launch a SINGLE imx95 node (0x88B7) on the real fleet segment
#     230.0.0.9:31337 to light up the wire for real, once mcx + rt1180 are up.
#
# The node HOLDS: it beacons forever and never exits on its own, because to a
# peer LEFT-EARLY and CRASHED are the same observation. Departure is the
# coordinator's decision - this harness kills it.
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
TMO=${TMO:-120}   # nodes HOLD; the coordinator kills them
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

# --- the CROSS-IMPLEMENTATION peer: 91emulator's OWN beacon, built from THEIR
# source and run as the stand-in nodes.  A self-test whose peers all run OUR
# code proves only that our emitter agrees with our checker - it is blind BY
# CONSTRUCTION to the bug that actually bit us (we emitted an ASCII body for
# weeks; every one of our own nodes accepted it, and both real peers correctly
# refused to).  An INDEPENDENT implementation of the agreed body is the only
# local oracle that can see that, so the stand-ins run 91's strict checker.
IMX91_SRC=${IMX91_SRC:-$HOME/Documents/GitHub/91emulator/tests/interconnect-imx91/enetbeacon.c}
[ -f "$IMX91_SRC" ] || skip "no cross-impl peer source ($IMX91_SRC)"
"$CC" -static -O2 -o "$WORK/imx91-beacon" "$IMX91_SRC" || skip "cross-impl peer failed to build"
# Name the artifact we actually built against: a path in a live worktree is not
# an artifact (holobench).  If this moves, the result must be re-derived.
PEER_REPO=$(cd "$(dirname "$IMX91_SRC")" && git rev-parse --short HEAD 2>/dev/null || echo "unknown")
PEER_MD5=$(md5sum "$IMX91_SRC" | cut -d' ' -f1)
echo "cross-impl peer: 91emulator enetbeacon.c @ $PEER_REPO (src md5 $PEER_MD5)"

# --- patched dtb: ENETC ports fixed-link so eth probes (shared with run-eth.sh) ---
"$DTC" -I dtb -O dts "$DTB" >"$WORK/base.dts" 2>/dev/null
python3 "$ROOT/tests/netc/patch-dtb.py" "$WORK/base.dts" >"$WORK/enetc.dts"
"$DTC" -I dts -O dtb -o "$WORK/enetc.dtb" "$WORK/enetc.dts" 2>/dev/null || skip "dtb recompile failed"

# --- initramfs: busybox + enet-lab3 + role-from-cmdline init ---
root="$WORK/root"; mkdir -p "$root"/{bin,proc,sys,dev}
( cd "$WORK" && zcat "$DEFAULT_CPIO" | cpio -idmu --quiet 'bin/busybox' )
cp "$WORK/bin/busybox" "$root/bin/busybox"
cp "$WORK/enet-lab3" "$root/enet-lab3"
cp "$WORK/imx91-beacon" "$root/imx91-beacon"
cat > "$root/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox --install -s /bin 2>/dev/null
ET=$(sed -n 's/.*lab_et=\([^ ]*\).*/\1/p' /proc/cmdline)
PEERS=$(sed -n 's/.*lab_peers=\([^ ]*\).*/\1/p' /proc/cmdline | tr ',' ' ')
DL=$(sed -n 's/.*lab_deadline=\([^ ]*\).*/\1/p' /proc/cmdline)
[ -n "$DL" ] && export LAB_DEADLINE_MS="$DL"
# Which implementation this node runs: ours, or 91emulator's independent one.
IMPL=$(sed -n 's/.*lab_impl=\([^ ]*\).*/\1/p' /proc/cmdline)
[ -n "$IMPL" ] || IMPL=imx95
if [ "$IMPL" = imx91 ]; then
    BIN=/imx91-beacon
    export BEACON_STRICT=1      # enforce the body: the whole point of the peer
else
    BIN=/enet-lab3
fi
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
echo "ENET-LAB3 boot: if=$IF et=$ET impl=$IMPL carrier=$(cat /sys/class/net/$IF/carrier 2>/dev/null)"
# The node HOLDS. It re-arms and beacons forever and never exits on its own,
# because to a peer LEFT-EARLY and CRASHED are the same observation - departure
# is the coordinator's decision. The harness kills us.
"$BIN" "$IF" "$ET" $PEERS
echo "ENET-LAB3 rc=$?"
sleep 1; poweroff -f
INIT
chmod +x "$root/init"
( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/lab.cpio.gz"

boot() { # $1=impl $2=mcast-group $3=mac $4=my_et $5=peers-csv $6=logfile [$7=deadline_ms]
    timeout --signal=KILL "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
        -kernel "$IMAGE" -dtb "$WORK/enetc.dtb" -initrd "$WORK/lab.cpio.gz" \
        -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init lab_impl=$1 lab_et=$4 lab_peers=$5 lab_deadline=${7:-120000}" \
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
    echo "== NEG-TEST: imx95 + only ONE peer (0x88B5) up on $SELF_GROUP (must NOT pass) =="
    boot imx95 "$SELF_GROUP" 02:49:4d:58:95:01 0x88B7 0x88B5,0x88B6 "$WORK/imx95.log" 25000 &
    boot imx91 "$SELF_GROUP" 02:4d:43:58:00:01 0x88B5 0x88B7,0x88B6 "$WORK/peerA.log" 25000 &
    wait
    echo "--- imx95 ---"; grep -aE 'ENET-LAB3' "$WORK/imx95.log" | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'
    saw5=$(grep -ac 'rx: peer 0x88b5 body OK' "$WORK/imx95.log")
    if grep -aq 'ENET-LAB3 PASS:' "$WORK/imx95.log"; then
        echo "RESULT: FAIL (imx95 FALSE-PASSED with only one peer up - assertion is broken)"; exit 1
    fi
    if [ "$saw5" -ge 1 ] && grep -aq 'ENET-LAB3 FAIL: deadline, missing peers: 0x88b6' "$WORK/imx95.log"; then
        echo "RESULT: PASS (wire live - imx95 accepted 0x88B5's BODY - yet correctly did NOT pass, missing 0x88B6)"
        exit 0
    fi
    echo "RESULT: INCONCLUSIVE (expected imx95 to accept 0x88B5's body and fail on missing 0x88B6)"; exit 1
fi

echo "== INTEROP SELF-TEST on private mcast $SELF_GROUP =="
echo "   imx95 (0x88B7) runs OUR node.  Both stand-ins run 91emulator's OWN"
echo "   beacon (BEACON_STRICT=1) - an INDEPENDENT implementation of the agreed"
echo "   body.  If their checker counts us, our frames are interop-correct; if we"
echo "   were still emitting the old ASCII payload they would condemn us, which is"
echo "   exactly what happened on the real 4-node wire."
boot imx95 "$SELF_GROUP" 02:49:4d:58:95:01 0x88B7 0x88B5,0x88B6 "$WORK/imx95.log" &
boot imx91 "$SELF_GROUP" 02:4d:43:58:00:01 0x88B5 0x88B7,0x88B6 "$WORK/peerA.log" &
boot imx91 "$SELF_GROUP" 54:27:8d:00:00:00 0x88B6 0x88B7,0x88B5 "$WORK/peerB.log" &
wait

clean() { sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' "$1"; }
echo "--- imx95 (our node) ---"
clean "$WORK/imx95.log" | grep -aE 'ENET-LAB3' | head -8
echo "--- peer A (91's checker, et 0x88B5) ---"
clean "$WORK/peerA.log" | grep -aE 'ENET-LAB3 (PASS|CORRUPT|FAIL)' | head -4
echo "--- peer B (91's checker, et 0x88B6) ---"
clean "$WORK/peerB.log" | grep -aE 'ENET-LAB3 (PASS|CORRUPT|FAIL)' | head -4

fail=0
# 1. An INDEPENDENT implementation must never call our frames corrupt. This is
#    the assertion that would have caught the ASCII body, and nothing we could
#    have written on our own side would have.
for p in peerA peerB; do
    if clean "$WORK/$p.log" | grep -aq "ENET-LAB3 CORRUPT.*0x88b7"; then
        echo "FAIL: 91's checker condemned OUR frames ($p) - the body is wrong:"
        clean "$WORK/$p.log" | grep -a "ENET-LAB3 CORRUPT.*0x88b7" | head -2
        fail=1
    fi
done
# 2. Both independent peers must have actually COUNTED us (PASS = they saw all
#    their peers, and we are one of them).
for p in peerA peerB; do
    clean "$WORK/$p.log" | grep -aq 'ENET-LAB3 PASS' || {
        echo "FAIL: 91's checker ($p) never passed - it did not count us"; fail=1; }
done
# 3. We must RE-ARM, not latch: a second beat proves the oracle did not expire.
beats=$(clean "$WORK/imx95.log" | grep -ac 'ENET-LAB3 PASS:')
if [ "${beats:-0}" -lt 2 ]; then
    echo "FAIL: our node produced $beats PASS beat(s) - it latched instead of re-arming"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "RESULT: PASS - our body was accepted by an INDEPENDENT implementation"
    echo "  (91emulator's strict checker @ $PEER_REPO counted us; we re-armed $beats times)"
    exit 0
fi
echo "RESULT: FAIL - see logs"
cp "$WORK"/imx95.log "$WORK"/mcx.log "$WORK"/rt.log /tmp/ 2>/dev/null
exit 1
