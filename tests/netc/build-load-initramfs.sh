#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the NETC *load*-soak initramfs: busybox + a dynamically-linked aarch64
# iperf, with a /init that brings eth0 up and runs a continuous bidirectional
# iperf load against the slirp host (10.0.2.2:5001), printing a ZSNAP line with
# eth0's rx/tx counters + MemFree every 60 s. This is what drives the
# throughput soak in tests/netc/load-soak.sh (the prior runs sustained
# ~660 Mbps with zero datapath errors).
#
# busybox is reused from the default initramfs (offline). iperf + its glibc /
# libstdc++ runtime are fetched from Ubuntu 22.04 (jammy) arm64 .debs - no
# cross-compiler needed. iperf is C++ so it pulls libstdc++ too; the loader and
# libs are bundled under /usr/lib and surfaced via LD_LIBRARY_PATH.
#
# Output: tests/netc/netc-load-initramfs.cpio.gz  (override with OUT=)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
DEFAULT_CPIO="$HERE/../busybox-initramfs/busybox-initramfs.cpio.gz"
OUT=${OUT:-$HERE/netc-load-initramfs.cpio.gz}

# jammy arm64 packages (override if a pool URL ages out). iperf 2.1.5 is the
# version the soak was validated with.
BASE=${UBUNTU_PORTS:-http://ports.ubuntu.com/ubuntu-ports/pool}
IPERF_DEB=${IPERF_DEB:-$BASE/universe/i/iperf/iperf_2.1.5+dfsg1-1_arm64.deb}
LIBC_DEB=${LIBC_DEB:-$BASE/main/g/glibc/libc6_2.35-0ubuntu3.13_arm64.deb}
LIBSTDCPP_DEB=${LIBSTDCPP_DEB:-$BASE/main/g/gcc-12/libstdc++6_12.3.0-1ubuntu1~22.04.3_arm64.deb}
LIBGCC_DEB=${LIBGCC_DEB:-$BASE/main/g/gcc-12/libgcc-s1_12.3.0-1ubuntu1~22.04.3_arm64.deb}

# Guest-side knobs baked into /init:
GW=${GW:-10.0.2.2}                 # slirp gateway / iperf server
GUEST_IP=${GUEST_IP:-10.0.2.15}
MAC=${MAC:-00:04:9f:06:11:22}      # matches local-mac-address in patch-dtb.py
IPERF_T=${IPERF_T:-300}            # per-iperf-client run length (s), looped
SNAP_SEC=${SNAP_SEC:-60}           # ZSNAP cadence
SNAP_MAX=${SNAP_MAX:-1450}         # ~24h of samples; host timeout is the real stop

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

root="$WORK/root"
mkdir -p "$root"/{bin,proc,sys,dev,lib,usr/bin,usr/lib}

# --- busybox (offline, from the default initramfs) ---
[ -e "$DEFAULT_CPIO" ] || { echo "error: default initramfs not found: $DEFAULT_CPIO" >&2; exit 1; }
( cd "$WORK" && zcat "$DEFAULT_CPIO" | cpio -idmu --quiet 'bin/busybox' )
cp "$WORK/bin/busybox" "$root/bin/busybox"
install -m 0755 "$ROOT/tests/lib/guest-enetc-names.sh" "$root/bin/enetc-names"

# --- iperf + runtime libs (fetched debs) ---
fetch_x() { # url destdir
    local deb="$WORK/$(basename "$1")"
    echo "fetching $(basename "$1") ..."
    wget -qO "$deb" "$1" || { echo "error: fetch failed: $1" >&2; exit 1; }
    dpkg-deb -x "$deb" "$2"
}
fetch_x "$IPERF_DEB"     "$WORK/iperf"
fetch_x "$LIBC_DEB"      "$WORK/libc"
fetch_x "$LIBSTDCPP_DEB" "$WORK/libstdcpp"
fetch_x "$LIBGCC_DEB"    "$WORK/libgcc"

cp -L "$(find "$WORK/iperf" -type f -path '*/usr/bin/iperf' | head -1)" "$root/usr/bin/iperf"

# Copy the dynamic loader (to the path the ELF interpreter expects, /lib, and
# also /usr/lib) plus the NEEDED libs into /usr/lib. cp -L dereferences the
# soname symlinks so the regular files land.
LD=$(find "$WORK/libc" -name 'ld-linux-aarch64.so.1' | head -1)
cp -L "$LD" "$root/lib/ld-linux-aarch64.so.1"
cp -L "$LD" "$root/usr/lib/ld-linux-aarch64.so.1"
for so in libc.so.6 libm.so.6 libpthread.so.0; do
    f=$(find "$WORK/libc" -name "$so" | head -1)
    [ -n "$f" ] && cp -L "$f" "$root/usr/lib/$so"
done
cp -L "$(find "$WORK/libstdcpp" -name 'libstdc++.so.6.*' -type f | head -1)" "$root/usr/lib/libstdc++.so.6"
cp -L "$(find "$WORK/libgcc"    -name 'libgcc_s.so.1'     -type f | head -1)" "$root/usr/lib/libgcc_s.so.1"

[ -x "$root/usr/bin/iperf" ] || { echo "error: iperf not assembled" >&2; exit 1; }

# --- /init (parameters expanded; note unquoted heredoc tag) ---
cat > "$root/init" <<INIT
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc; /bin/busybox mount -t sysfs sysfs /sys
/bin/busybox --install -s /bin 2>/dev/null
export LD_LIBRARY_PATH=/usr/lib
n=0; while [ ! -d /sys/bus/pci/devices/0002:00:00.0/net ] && [ \$n -lt 40 ]; do sleep 1; n=\$((n+1)); done
# The -nic backend is on ENETC PF0, but Linux names netdevs in probe order, so
# eth0 is not reliably that PF. Canonicalise by PCI address first.
/bin/enetc-names
ip link set eth0 address $MAC; ip link set eth0 up
ip addr add $GUEST_IP/24 dev eth0; ip route add default via $GW 2>/dev/null
sleep 2
echo "ZSOAK-LINK=\$(cat /sys/class/net/eth0/operstate)"
S=/sys/class/net/eth0/statistics
snap() { echo "ZSNAP \$1 t=\$(cut -d. -f1 /proc/uptime) tx=\$(cat \$S/tx_packets) rx=\$(cat \$S/rx_packets) txb=\$(cat \$S/tx_bytes) rxb=\$(cat \$S/rx_bytes) rxerr=\$(cat \$S/rx_errors) rxdrop=\$(cat \$S/rx_dropped) txerr=\$(cat \$S/tx_errors) txdrop=\$(cat \$S/tx_dropped) memfree=\$(grep MemFree /proc/meminfo | tr -dc 0-9)"; }
snap START
( while true; do /usr/bin/iperf -c $GW -t $IPERF_T -d >/dev/null 2>&1; sleep 1; done ) &
LOADER=\$!
i=0
while [ \$i -lt $SNAP_MAX ]; do
  sleep $SNAP_SEC; i=\$((i+1)); snap "T\${i}"
done
kill \$LOADER 2>/dev/null
snap END
echo "ZSOAK-DONE"; poweroff -f
INIT
chmod +x "$root/init"

( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$OUT"
echo "wrote $OUT ($(stat -c%s "$OUT") bytes)"
echo "  iperf load: -c $GW -t ${IPERF_T}s -d (looped); ZSNAP every ${SNAP_SEC}s"
