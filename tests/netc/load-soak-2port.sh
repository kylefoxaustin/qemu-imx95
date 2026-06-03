#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Two-port NETC throughput / stability load-soak launcher.
#
# The companion to load-soak.sh, but it drives BOTH modelled ENETC PFs under
# sustained bidirectional traffic instead of one port against the slirp host.
# The two PFs are joined on one QEMU L2 hub (two `-nic hubport,hubid=0`); the
# guest moves eth1 into its own net+mount namespace (so frames cross the
# modelled wire, not the host loopback) and runs an in-guest iperf server on
# eth1 with a looping bidirectional client on eth0. Every frame therefore
# traverses BOTH device models' TX and RX BD-ring DMA paths, both directions.
#
# Self-contained: unlike load-soak.sh there is NO host-side iperf sink - the
# server runs inside the guest netns. The iperf binary + glibc runtime are
# reused from the committed single-port load initramfs
# (netc-load-initramfs.cpio.gz); only the /init differs, assembled on the fly.
#
# A ZSNAP heartbeat every SNAP_SEC carries BOTH ports' rx/tx + err/drop counters
# (eth1's read from inside its namespace via a private sysfs mount) plus MemFree,
# so the same health checks as the single-port soak apply: want every
# rxerr/rxdrop/txerr/txdrop = 0 and MemFree flat.
#
# Required artifacts (override via env):
#   QEMU   - qemu-system-aarch64               (default: ../../build/...)
#   KBUILD - kernel build dir (Image + dtb + scripts/dtc/dtc)
#   SM_ELF - System Manager m33_image.elf      (legacy SMELF also accepted)
#   LOADCPIO - single-port load initramfs to borrow iperf+libs from
#              (default: ./netc-load-initramfs.cpio.gz; build-load-initramfs.sh)
# Tunables: BOARD (evk|frdm), TIMEOUT_S, SNAP_SEC, SNAP_MAX, IPERF_T.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

BOARD=${BOARD:-evk}
case "$BOARD" in
    evk|19x19)  DTB_NAME=imx95-19x19-evk ;;
    frdm|15x15) DTB_NAME=imx95-15x15-frdm ;;
    *) echo "BOARD must be evk|frdm" >&2; exit 2 ;;
esac

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
LOADCPIO=${LOADCPIO:-$HERE/netc-load-initramfs.cpio.gz}
TIMEOUT_S=${TIMEOUT_S:-87000}     # ~24.2h backstop
SNAP_SEC=${SNAP_SEC:-60}
SNAP_MAX=${SNAP_MAX:-1450}        # ~24h of samples; the host timeout is the real stop
IPERF_T=${IPERF_T:-300}           # per iperf-client run length (s), looped

IMAGE=$KBUILD/arch/arm64/boot/Image
DTB=$KBUILD/arch/arm64/boot/dts/freescale/$DTB_NAME.dtb
DTC=$KBUILD/scripts/dtc/dtc

need() { [ -e "$2" ] || { echo "missing $1: $2"; exit 1; }; }
need QEMU "$QEMU"; need IMAGE "$IMAGE"; need DTC "$DTC"
need SM_ELF "$SM_ELF"; need LOADCPIO "$LOADCPIO"; need DTB "$DTB"

OUTDIR=$ROOT/build/netc-2port-soak-$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUTDIR"
WORK=$(mktemp -d)

# --- patched DTB: both ports fixed-link + identity msi-map ---
"$DTC" -I dtb -O dts "$DTB" >"$WORK/base.dts" 2>/dev/null
python3 "$HERE/patch-dtb.py" "$WORK/base.dts" >"$WORK/netc.dts"
"$DTC" -I dts -O dtb -o "$OUTDIR/netc.dtb" "$WORK/netc.dts" 2>/dev/null

# --- initramfs: reuse iperf+libs from the single-port load cpio, swap /init ---
root="$WORK/root"; mkdir -p "$root"
( cd "$root" && zcat "$LOADCPIO" | cpio -idmu --quiet )
[ -x "$root/usr/bin/iperf" ] || { echo "error: $LOADCPIO has no /usr/bin/iperf" >&2; exit 1; }

cat > "$root/init" <<INIT
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc; /bin/busybox mount -t sysfs sysfs /sys
/bin/busybox --install -s /bin 2>/dev/null
export LD_LIBRARY_PATH=/usr/lib
n=0; while { [ ! -d /sys/class/net/eth0 ] || [ ! -d /sys/class/net/eth1 ]; } && [ \$n -lt 60 ]; do sleep 1; n=\$((n+1)); done
echo "Z2SOAK-IF \$(ls /sys/class/net | tr '\n' ' ')"
# Hold a net+mount namespace (own sysfs) so eth1's counters are readable.
unshare -n -m /bin/busybox sh -c 'mount --make-rprivate / 2>/dev/null; mount -t sysfs sysfs /sys 2>/dev/null; exec /bin/busybox sleep 1000000000' &
NSPID=\$!; sleep 1
ip link set eth1 netns \$NSPID
ip addr add 10.0.0.1/24 dev eth0; ip link set eth0 up
nsenter -t \$NSPID -n -m ip link set lo up
nsenter -t \$NSPID -n -m ip addr add 10.0.0.2/24 dev eth1
nsenter -t \$NSPID -n -m ip link set eth1 up
sleep 3
echo "Z2SOAK-LINK eth0=\$(cat /sys/class/net/eth0/operstate) eth1=\$(nsenter -t \$NSPID -n -m cat /sys/class/net/eth1/operstate 2>/dev/null)"
S0=/sys/class/net/eth0/statistics
e1() { nsenter -t \$NSPID -n -m cat /sys/class/net/eth1/statistics/\$1 2>/dev/null; }
snap() { echo "ZSNAP \$1 t=\$(cut -d. -f1 /proc/uptime) e0tx=\$(cat \$S0/tx_packets) e0rx=\$(cat \$S0/rx_packets) e0txb=\$(cat \$S0/tx_bytes) e0rxb=\$(cat \$S0/rx_bytes) e0rxerr=\$(cat \$S0/rx_errors) e0rxdrop=\$(cat \$S0/rx_dropped) e0txerr=\$(cat \$S0/tx_errors) e0txdrop=\$(cat \$S0/tx_dropped) e1tx=\$(e1 tx_packets) e1rx=\$(e1 rx_packets) e1rxerr=\$(e1 rx_errors) e1rxdrop=\$(e1 rx_dropped) e1txerr=\$(e1 tx_errors) e1txdrop=\$(e1 tx_dropped) memfree=\$(grep MemFree /proc/meminfo | tr -dc 0-9)"; }
snap START
# in-guest iperf server on eth1 (in the ns); looping bidirectional client on eth0
nsenter -t \$NSPID -n -m /usr/bin/iperf -s >/dev/null 2>&1 &
sleep 2
( while true; do /usr/bin/iperf -c 10.0.0.2 -t $IPERF_T -d >/dev/null 2>&1; sleep 1; done ) &
LOADER=\$!
i=0
while [ \$i -lt $SNAP_MAX ]; do sleep $SNAP_SEC; i=\$((i+1)); snap "T\${i}"; done
kill \$LOADER 2>/dev/null
snap END
echo "Z2SOAK-DONE"; poweroff -f
INIT
chmod +x "$root/init"
( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$OUTDIR/2port.cpio.gz"

cat > "$OUTDIR/cmdline" <<EOF
board=$DTB_NAME timeout=${TIMEOUT_S}s snap=${SNAP_SEC}s x${SNAP_MAX} iperf_t=${IPERF_T}s
EOF

echo "Two-port NETC load-soak ($DTB_NAME) -> $OUTDIR"
echo "  watch:  grep ZSNAP $OUTDIR/serial.log | tail"
echo "  health: grep -aoE '(e0|e1)(rx|tx)(err|drop)=[0-9]+' $OUTDIR/serial.log | sort -u   (want all =0)"
echo "  done:   grep Z2SOAK-DONE $OUTDIR/serial.log   (clean finish; backstop did NOT fire)"
# qemu.out is NOT expected empty here: the hub deliberately connects the two
# ports to each other, not to the host, so QEMU always prints one benign
# "hub 0 is not connected to host network" line. A healthy run's qemu.out is
# exactly that one line; anything else (or a non-empty after Z2SOAK-DONE) is
# worth a look.

setsid timeout --signal=KILL "$TIMEOUT_S" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -kernel "$IMAGE" -dtb "$OUTDIR/netc.dtb" -initrd "$OUTDIR/2port.cpio.gz" \
    -append "earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -nic hubport,hubid=0,model=fsl-enetc \
    -nic hubport,hubid=0,model=fsl-enetc \
    -serial file:"$OUTDIR/serial.log" -serial null -monitor none \
    >"$OUTDIR/qemu.out" 2>&1 &
echo "launched pid $! (detached). rm -rf $WORK on exit is automatic."
rm -rf "$WORK"
