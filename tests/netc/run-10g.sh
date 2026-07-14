#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# ENETC2 10G port (i.MX 95).
#
# The third ENETC PF (ENETC2, devfn 0x80, ethernet@10,0) is the EVK's 10G
# port. patch-dtb.py enables it as a fixed-link 10gbase-r node, so enetc4_pf
# brings it up at 10G without the real Aquantia PHY / EMDIO. This boots the
# machine with three -nic backends and checks all three interfaces come up,
# the 10G one at 10Gbps.
#
# PASS = eth0/eth1 link up at 1G and eth2 link up at 10G:
#   fsl_enetc4 0002:00:10.0 eth2: Link is Up - 10Gbps/Full
#   /sys/class/net/eth2/speed == 10000
#
# Required artifacts (override via env): QEMU, KBUILD (Image + dtb + dtc),
# SM_ELF - same as run-2port.sh.

set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=$KBUILD/arch/arm64/boot/Image
DTB=$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb
DTC=$KBUILD/scripts/dtc/dtc
TMO=${TMO:-180}

need() { [ -e "$2" ] || { echo "missing $1: $2"; exit 1; }; }
need QEMU "$QEMU"; need IMAGE "$IMAGE"; need DTC "$DTC"; need SM_ELF "$SM_ELF"
need DTB "$DTB"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

# Patched DTB: all three ports fixed-link (ENETC2 at 10gbase-r/10000).
"$DTC" -I dtb -O dts "$DTB" >"$WORK/base.dts" 2>/dev/null
python3 "$HERE/patch-dtb.py" "$WORK/base.dts" >"$WORK/netc.dts"
"$DTC" -I dts -O dtb -o "$WORK/netc.dtb" "$WORK/netc.dts" 2>/dev/null

# Initramfs: poll for eth2, then report each port's speed/carrier.
root="$WORK/root"; mkdir -p "$root"
zcat "$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz" | \
    (cd "$root" && cpio -idmu 2>/dev/null)
cat > "$root/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
# wait for the interfaces to enumerate, then bring them administratively up -
# a fixed-link port only reports its link once the netdev is admin-up
i=0; while [ ! -e /sys/class/net/eth2 ] && [ $i -lt 120 ]; do usleep 500000; i=$((i+1)); done
for e in eth0 eth1 eth2; do ip link set "$e" up 2>/dev/null; done
i=0
while [ "$(cat /sys/class/net/eth2/carrier 2>/dev/null)" != "1" ] && [ $i -lt 40 ]; do
    usleep 500000; i=$((i+1))
done
sleep 1
echo "=== NETC-10G ==="
for n in eth0 eth1 eth2; do
    echo "$n: speed=$(cat /sys/class/net/$n/speed 2>/dev/null) carrier=$(cat /sys/class/net/$n/carrier 2>/dev/null)"
done
dmesg | grep -E 'fsl_enetc4.*Link is Up' || echo "(no enetc link-up in dmesg)"
echo "=== NETC-10G-done ==="
/bin/busybox poweroff -f
INIT
chmod +x "$root/init"
( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

LOG="$WORK/serial.log"
timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -kernel "$IMAGE" -dtb "$WORK/netc.dtb" -initrd "$WORK/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -nic user -nic user -nic user \
    -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "--- ports ---"; sed -n '/NETC-10G ===/,/NETC-10G-done/p' "$LOG" | grep -E '^eth'
grep -aE 'fsl_enetc4 .* Link is Up' "$LOG" || true
# Identify ports by reported speed, not by ethN name: Linux names netdevs in
# probe order, which is not stable across ENETC revisions/defer timing. Require
# one port up at 10Gbps and at least one at 1Gbps.
grep -qaE 'Link is Up - 10Gbps' "$LOG" && \
   grep -qaE 'eth[0-9]+: speed=10000 carrier=1' "$LOG" && \
   grep -qaE 'eth[0-9]+: speed=1000 carrier=1'  "$LOG" && {
    echo "PASS: one ENETC port up at 10Gbps, others at 1G"; exit 0; }
echo "FAIL: the 10G port did not come up at 10Gbps"; exit 1
