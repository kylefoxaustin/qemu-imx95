#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Two-port NETC back-to-back traffic test.
#
# Brings up BOTH modelled ENETC PFs - ENETC0 (eth0, PCI 0002:00:00.0) and
# ENETC1 (eth1, 0002:00:08.0), the 1G port pair the 15x15 FRDM and 19x19 EVK
# boards pin out - and pumps real traffic between them. The two PFs are joined
# on one QEMU L2 hub (two `-nic hubport,hubid=0`), so a frame leaving eth0
# arrives at eth1 and vice versa.
#
# To prove the frame actually crosses the modelled hardware (and isn't
# short-circuited by the kernel's same-host local delivery), the guest /init
# moves eth1 into its own network namespace: eth0 stays in the root netns at
# 10.0.0.1, eth1 lives in netns 'p1' at 10.0.0.2, and we ping both ways. A PASS
# means frames really went eth0 -> hub -> eth1 and back through both device
# models' TX and RX BD-ring DMA paths.
#
# Runs on either board's device tree (same machine, different -dtb):
#   ./run-2port.sh            # 19x19 EVK dtb (default)
#   ./run-2port.sh frdm       # 15x15 FRDM dtb (its native 1G+1G config)
#   ./run-2port.sh evk
#
# patch-dtb.py enables ethernet@0,0 + ethernet@8,0 as fixed-link (no PHY/MDIO)
# and rewrites the NETC msi-map to ITS identity; see that script for why. The
# initramfs is assembled on the fly from the default busybox (which already has
# ip / ping / unshare / nsenter) - no extra binaries fetched.
#
# Required artifacts (override via env):
#   QEMU   - qemu-system-aarch64               (default: ../../build/...)
#   KBUILD - kernel build dir (Image + dtb + scripts/dtc/dtc)
#   SM_ELF - System Manager m33_image.elf      (legacy SMELF also accepted)
# The FRDM dtb may not ship in KBUILD by default; build it once with
#   make -C <linux-src> O=$KBUILD ARCH=arm64 freescale/imx95-15x15-frdm.dtb
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

BOARD=${1:-evk}
case "$BOARD" in
    evk|19x19)  DTB_NAME=imx95-19x19-evk ;;
    frdm|15x15) DTB_NAME=imx95-15x15-frdm ;;
    *) echo "usage: $0 [evk|frdm]" >&2; exit 2 ;;
esac

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
DEFAULT_CPIO=${DEFAULT_CPIO:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}

IMAGE=$KBUILD/arch/arm64/boot/Image
DTB=$KBUILD/arch/arm64/boot/dts/freescale/$DTB_NAME.dtb
DTC=$KBUILD/scripts/dtc/dtc

need() { [ -e "$2" ] || { echo "missing $1: $2"; exit 1; }; }
need QEMU "$QEMU"; need IMAGE "$IMAGE"; need DTC "$DTC"
need SM_ELF "$SM_ELF"; need DEFAULT_CPIO "$DEFAULT_CPIO"
if [ ! -e "$DTB" ]; then
    echo "missing $DTB_NAME dtb: $DTB"
    echo "  build it: make -C <linux-src> O=$KBUILD ARCH=arm64 freescale/$DTB_NAME.dtb"
    exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# --- patched DTB: both ports enabled (fixed-link) + identity msi-map ---
"$DTC" -I dtb -O dts "$DTB" >"$WORK/base.dts" 2>/dev/null
python3 "$HERE/patch-dtb.py" "$WORK/base.dts" >"$WORK/netc.dts"
"$DTC" -I dts -O dtb -o "$WORK/netc.dtb" "$WORK/netc.dts" 2>/dev/null

# --- initramfs: busybox + a two-port netns back-to-back /init ---
root="$WORK/root"
mkdir -p "$root"/{bin,proc,sys,dev}
( cd "$WORK" && zcat "$DEFAULT_CPIO" | cpio -idmu --quiet 'bin/busybox' )
cp "$WORK/bin/busybox" "$root/bin/busybox"

cat > "$root/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox --install -s /bin 2>/dev/null
echo "ZBOOT two-port back-to-back test"
# The machine has THREE ENETC PFs but only the first two (devfn 00.0 and 08.0)
# get a -nic hubport. Linux names netdevs in probe-completion order, and the
# three PFs probe concurrently, so eth0/eth1 do NOT reliably land on the two
# hub-backed PFs - about a third of boots named one of them after 0002:00:10.0,
# which has no peer, and the ping failed for reasons that had nothing to do with
# the model. Resolve the interfaces from their PCI addresses instead.
PCI0=0002:00:00.0
PCI1=0002:00:08.0
n=0
while [ $n -lt 60 ]; do
    IF0=$(ls /sys/bus/pci/devices/$PCI0/net 2>/dev/null | head -1)
    IF1=$(ls /sys/bus/pci/devices/$PCI1/net 2>/dev/null | head -1)
    [ -n "$IF0" ] && [ -n "$IF1" ] && break
    sleep 1; n=$((n+1))
done
echo "ZIF present: $(ls /sys/class/net 2>/dev/null | tr '\n' ' ')"
echo "ZIF hub pair: $PCI0=$IF0 $PCI1=$IF1"
if [ -z "$IF0" ] || [ -z "$IF1" ]; then
    echo "ZRESULT FAIL (hub-backed ENETC netdevs never appeared)"; poweroff -f
fi

# Hold a fresh net namespace open with a sleeper; move eth1 into it so a
# same-host ping must cross the modelled wire (no kernel loopback shortcut).
unshare -n /bin/busybox sleep 100000 &
NSPID=$!
sleep 1
echo "ZNS pid=$NSPID ns=$(readlink /proc/$NSPID/ns/net 2>/dev/null)"
ip link set $IF1 netns $NSPID || echo "ZERR move $IF1 failed"

ip addr add 10.0.0.1/24 dev $IF0; ip link set $IF0 up
nsenter -t $NSPID -n ip link set lo up
nsenter -t $NSPID -n ip addr add 10.0.0.2/24 dev $IF1
nsenter -t $NSPID -n ip link set $IF1 up
sleep 3
echo "ZLINK $IF0 operstate=$(cat /sys/class/net/$IF0/operstate) carrier=$(cat /sys/class/net/$IF0/carrier 2>/dev/null)"
echo "ZADDR $IF0: $(ip -o -4 addr show $IF0 2>/dev/null)"
echo "ZADDR $IF1: $(nsenter -t $NSPID -n ip -o -4 addr show $IF1 2>/dev/null)"

echo "--- ping $IF0(10.0.0.1) -> $IF1(10.0.0.2) ---"
ping -c 5 -I $IF0 10.0.0.2; R1=$?
echo "--- ping $IF1(10.0.0.2) -> $IF0(10.0.0.1) [from netns] ---"
nsenter -t $NSPID -n ping -c 5 -I $IF1 10.0.0.1; R2=$?

# eth0's counters plus the bidirectional ping above prove both ports' TX and
# RX datapaths (eth1, in the netns, both sent replies and received pings).
echo "ZSTATS $IF0 tx=$(cat /sys/class/net/$IF0/statistics/tx_packets) rx=$(cat /sys/class/net/$IF0/statistics/rx_packets)"
# Per-direction verdicts so a failure pins the broken path without a re-run.
[ "$R1" = 0 ] && echo "ZDIR $IF0->$IF1: OK" || echo "ZDIR $IF0->$IF1: FAIL (TX on $IF0 / RX on $IF1)"
[ "$R2" = 0 ] && echo "ZDIR $IF1->$IF0: OK" || echo "ZDIR $IF1->$IF0: FAIL (TX on $IF1 / RX on $IF0)"
if [ "$R1" = 0 ] && [ "$R2" = 0 ]; then echo "ZRESULT PASS"; else echo "ZRESULT FAIL ($IF0->$IF1=$R1 $IF1->$IF0=$R2)"; fi
poweroff -f
INIT
chmod +x "$root/init"
( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/2port.cpio.gz"

echo "Booting $DTB_NAME with two ENETC ports on one hub (look for ZRESULT)..."
timeout --signal=KILL 150 "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -kernel "$IMAGE" -dtb "$WORK/netc.dtb" -initrd "$WORK/2port.cpio.gz" \
    -append "earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -nic hubport,hubid=0,model=fsl-enetc \
    -nic hubport,hubid=0,model=fsl-enetc \
    -serial file:"$WORK/serial.log" -serial null -monitor none >/dev/null 2>&1 || true

echo "--- ENETC ports enumerated ---"
grep -aE "0002:00:00.0: \[1131|0002:00:08.0: \[1131" "$WORK/serial.log" || echo "(ports not enumerated!)"
echo "--- two-port back-to-back result ---"
grep -aE "ZIF|ZNS|ZLINK|ZADDR|ZSTATS|ZDIR|packets transmitted|ZRESULT" "$WORK/serial.log" | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'

if grep -aq "ZRESULT PASS" "$WORK/serial.log"; then
    echo "RESULT: PASS ($DTB_NAME, both ports, back-to-back)"
    exit 0
else
    echo "RESULT: FAIL ($DTB_NAME) - see log"
    cp "$WORK/serial.log" "/tmp/netc-2port-$BOARD.log" 2>/dev/null && echo "  log: /tmp/netc-2port-$BOARD.log"
    exit 1
fi
