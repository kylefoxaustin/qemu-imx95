#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# NETC throughput / stability load-soak launcher.
#
# Boots the modelled i.MX 95 ENETC PF (eth0) with the iperf load initramfs
# (tests/netc/netc-load-initramfs.cpio.gz) and a host-side iperf sink, then
# detaches and lets it run for ~24h. The guest drives a continuous
# bidirectional iperf load against the slirp host (10.0.2.2:5001) and prints a
# ZSNAP heartbeat every 60s carrying eth0's rx/tx counters + MemFree, so you
# can confirm the v2.0.0 NETC datapath sustained traffic (prior runs held
# ~660 Mbps) with zero rx/tx errors or drops and flat memory.
#
# This is the in-tree, reproducible form of the soak that previously lived only
# in build/ (frozen binary + restart script). The frozen binary is optional -
# by default it uses the normal build/qemu-system-aarch64; point QEMU= at a
# pinned binary (e.g. build/qemu-netc-soak-frozen) to soak a frozen build.
#
# Required artifacts (override via env):
#   QEMU   - qemu-system-aarch64               (default: ../../build/...)
#   KBUILD - kernel build dir (Image + dtb + scripts/dtc/dtc)
#   SM_ELF - System Manager m33_image.elf  (legacy SMELF also accepted)
#   INITRD - load initramfs (auto-built via build-load-initramfs.sh if absent)
# Also requires host `iperf` (the script starts the sink).
#
# Duration: TIMEOUT_S outer backstop (default ~24.2h). The guest /init also
# self-stops after its ZSNAP budget; the host timeout is the real stop.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
# SM_ELF is the repo-wide name; accept legacy SMELF too.
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
INITRD=${INITRD:-$HERE/netc-load-initramfs.cpio.gz}
TIMEOUT_S=${TIMEOUT_S:-87000}     # ~24.2h backstop

# The iperf target (gateway 10.0.2.2, TCP 5001) is baked into the load
# initramfs by build-load-initramfs.sh, so it is NOT a runtime knob here: the
# host sink must match that. To change it, rebuild the initramfs (GW=/IPERF_*).

IMAGE=$KBUILD/arch/arm64/boot/Image
DTB=$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb
DTC=$KBUILD/scripts/dtc/dtc

need() { [ -e "$2" ] || { echo "missing $1: $2" >&2; exit 1; }; }
need QEMU "$QEMU"; need IMAGE "$IMAGE"; need DTB "$DTB"; need DTC "$DTC"; need SM_ELF "$SM_ELF"
command -v iperf >/dev/null || { echo "missing host dependency: iperf" >&2; exit 1; }

if [ ! -e "$INITRD" ]; then
    echo "load initramfs missing; building it..."
    "$HERE/build-load-initramfs.sh" || { echo "initramfs build failed" >&2; exit 1; }
fi
need INITRD "$INITRD"

RUN="$ROOT/build/netc-load-soak-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN"

# Patched DTB (decompile -> patch-dtb.py -> recompile), regenerated per run so
# it always tracks the current kernel build.
"$DTC" -I dtb -O dts "$DTB" >"$RUN/base.dts" 2>/dev/null
python3 "$HERE/patch-dtb.py" "$RUN/base.dts" >"$RUN/netc.dts"
"$DTC" -I dts -O dtb -o "$RUN/netc.dtb" "$RUN/netc.dts" 2>/dev/null
need "patched DTB" "$RUN/netc.dtb"
cp "$INITRD" "$RUN/initramfs.cpio.gz"

# 1) Host iperf sink (the load target slirp maps to 10.0.2.2:5001 — the
# default port the baked-in guest iperf client connects to).
echo "starting host iperf -s (TCP 5001) ..."
pkill -x iperf 2>/dev/null || true
nohup iperf -s >"$RUN/iperf-server.log" 2>&1 &
echo $! >"$RUN/iperf-server.pid"
sleep 1

# 2) QEMU under the outer timeout backstop, detached.
echo "launching NETC load soak -> $RUN"
nohup timeout --signal=KILL "$TIMEOUT_S" \
    "$QEMU" -M imx95-19x19-evk -m 2G -display none \
        -kernel "$IMAGE" -dtb "$RUN/netc.dtb" -initrd "$RUN/initramfs.cpio.gz" \
        -append "earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
        -device loader,file="$SM_ELF",cpu-num=6 \
        -nic user,model=fsl-enetc \
        -serial file:"$RUN/serial.log" -serial null -monitor none \
        >"$RUN/qemu.out" 2>&1 &
echo $! >"$RUN/timeout.pid"

echo
echo "=== NETC load soak launched ==="
echo "run dir:   $RUN"
echo "throughput watch:  grep ZSNAP $RUN/serial.log | tail"
echo "liveness:          pgrep -af '$(basename "$QEMU").*$(basename "$RUN")'"
echo "errors (want 0):   grep -aoE '(rx|tx)(err|drop)=[0-9]+' $RUN/serial.log | sort -u"
echo "stop:              kill \$(cat $RUN/timeout.pid); pkill -x iperf"
