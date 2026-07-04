#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# ENETC2 10G port via the REAL PHY chain (i.MX 95, Path B).
#
# Unlike run-10g.sh (which fixed-links the 10G node), this keeps the stock
# ethernet@10,0 real chain: phy-handle -> Aquantia AQR113C on the NETC EMDIO
# (pcie@4cb00000), managed = "in-band-status", link from the port's internal DW
# xPCS. patch-dtb-real10g.py only swaps the never-resolving efuse nvmem MAC for a
# fixed local-mac (the 1G ports are fixed-linked so they bind). Exercises the
# EMDIO controller + AQR113C PHY model + the xPCS.
#
# PASS = the 10G port links at 10Gbps THROUGH the real chain:
#   - a port reports "Link is Up - 10Gbps/Full"
#   - the Aquantia AQR113C driver bound to it (real PHY, not fixed-link)
#   - no "pcs_config failed" (xPCS in-band config completed)
# (netdev names are probe-order dependent and not stable, so match by content.)
#
# Required artifacts (override via env): QEMU, KBUILD (Image + dtb + dtc),
# SM_ELF - same as run-10g.sh.

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

# Patched DTB: 1G ports fixed-link, 10G port keeps the real phy-handle/in-band.
"$DTC" -I dtb -O dts "$DTB" >"$WORK/base.dts" 2>/dev/null
python3 "$HERE/patch-dtb-real10g.py" "$WORK/base.dts" >"$WORK/netc.dts"
"$DTC" -I dts -O dtb -o "$WORK/netc.dtb" "$WORK/netc.dts" 2>/dev/null

# Initramfs: bring the ports up, wait for a 10G link, report.
root="$WORK/root"; mkdir -p "$root"
zcat "$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz" | \
    (cd "$root" && cpio -idmu 2>/dev/null)
cat > "$root/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
i=0; while [ ! -e /sys/class/net/eth2 ] && [ $i -lt 120 ]; do usleep 500000; i=$((i+1)); done
for e in eth0 eth1 eth2; do ip link set "$e" up 2>/dev/null; done
# in-band link comes up asynchronously after admin-up; wait for a 10G link
i=0
while ! dmesg | grep -q 'Link is Up - 10Gbps' && [ $i -lt 60 ]; do
    usleep 500000; i=$((i+1))
done
sleep 1
echo "=== NETC-REAL10G ==="
for n in eth0 eth1 eth2; do
    echo "$n: speed=$(cat /sys/class/net/$n/speed 2>/dev/null) carrier=$(cat /sys/class/net/$n/carrier 2>/dev/null)"
done
dmesg | grep -aE 'driver \[Aquantia AQR113C\]|Link is Up|pcs_config failed'
echo "=== NETC-REAL10G-done ==="
/bin/busybox poweroff -f
INIT
chmod +x "$root/init"
( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

LOG="$WORK/serial.log"
timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -kernel "$IMAGE" -dtb "$WORK/netc.dtb" -initrd "$WORK/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -nic user -nic user -nic user \
    -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "--- ports ---"; sed -n '/NETC-REAL10G ===/,/NETC-REAL10G-done/p' "$LOG" | grep -E '^eth'
grep -aE 'driver \[Aquantia AQR113C\]|Link is Up - 10Gbps' "$LOG" || true
grep -qa 'driver \[Aquantia AQR113C\]' "$LOG" && \
   grep -qaE 'eth[0-9]+: Link is Up - 10Gbps' "$LOG" && \
   ! grep -qa 'pcs_config failed' "$LOG" && {
    echo "PASS: 10G port up at 10Gbps via the real EMDIO -> AQR113C -> xPCS chain"
    exit 0; }
echo "FAIL: the 10G real-chain link did not come up cleanly"; exit 1
