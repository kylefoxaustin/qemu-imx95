#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the TWO-PORT enet-lab3 initramfs as a PINNABLE ARTIFACT for holobench's
# macvtap coordinator (the emulated-95 <-> real-silicon lab).
#
# The seam (holobench, CLAUDE.md 7): holobench NEVER invokes a script in this
# tree - it consumes ARTIFACTS by md5+commit and refuses to launch on drift. So
# this script's OUTPUT (a cpio.gz + a dtb + a manifest of md5/commit) is the
# integration surface, not the script itself. run.sh stays the human entry point.
#
# What the artifact does: one QEMU-95 guest, TWO ENETC PFs, two enet-lab3 beacon
# instances - one per PHYSICAL wire - so a failure LOCALISES to a transport:
#   PF 0002:00:00.0 (ENETC0) -> leg0 -> `enet-lab3 <if> 0x88B7 0x88B9`  (FRDM/LAN)
#   PF 0002:00:08.0 (ENETC1) -> leg1 -> `enet-lab3 <if> 0x88B7 0x88BA`  (Orin/USB)
# my_et is 0x88B7 on BOTH wires (this node is imx95 on both); the peers differ
# because the two real endpoints live on two different cables. Ethertypes are
# IN-BLOCK (0x88B5..0x88BF) so the sniffer does not drop them (enet-lab3.c:511).
#
# The coordinator emits the two `-nic tap,fd=` backends IN ORDER (1st->00.0,
# 2nd->08.0) and hands the guest two macvtap fds; this artifact never learns the
# fd numbers. The guest resolves each PF's netdev BY PCI ADDRESS, never ethN -
# Linux names ENETC netdevs in probe-completion order (~1/3 of boots misname
# them; see tests/netc/run-2port.sh).
#
# Required (env): KBUILD (Image + dtb + scripts/dtc/dtc), aarch64 cross-gcc.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
DTB=$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb
DTC=$KBUILD/scripts/dtc/dtc
DEFAULT_CPIO=${DEFAULT_CPIO:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
CC=${CC:-aarch64-linux-gnu-gcc}
OUT=${OUT:-$HOME/Documents/qemu-imx95-artifacts/enet-lab3-2port}

for f in "$DTB" "$DTC" "$DEFAULT_CPIO" "$HERE/enet-lab3.c" "$ROOT/tests/netc/patch-dtb.py"; do
    [ -e "$f" ] || { echo "MISSING: $f" >&2; exit 1; }
done
command -v "$CC" >/dev/null || { echo "MISSING cross-gcc: $CC" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

# --- static aarch64 lab tool (byte-identical build to run.sh) ---
"$CC" -static -O2 -Wall "$HERE/enet-lab3.c" -o "$WORK/enet-lab3"

# --- 2-port dtb: patch-dtb.py enables BOTH ethernet@0,0 and ethernet@8,0 ---
"$DTC" -I dtb -O dts "$DTB" >"$WORK/base.dts" 2>/dev/null
python3 "$ROOT/tests/netc/patch-dtb.py" "$WORK/base.dts" >"$WORK/enetc.dts"
"$DTC" -I dts -O dtb -o "$OUT/imx95-19x19-evk-enetc.dtb" "$WORK/enetc.dts" 2>/dev/null

# --- initramfs: busybox + enet-lab3 + the TWO-PORT init ---
root="$WORK/root"; mkdir -p "$root"/{bin,proc,sys,dev}
( cd "$WORK" && zcat "$DEFAULT_CPIO" | cpio -idmu --quiet 'bin/busybox' )
cp "$WORK/bin/busybox" "$root/bin/busybox"
cp "$WORK/enet-lab3"   "$root/enet-lab3"
cat > "$root/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox --install -s /bin 2>/dev/null

# Resolve an ENETC PF's netdev BY PCI ADDRESS. ethN is unreliable: Linux names
# ENETC netdevs in probe-COMPLETION order, so ~1/3 of boots name one after
# 0002:00:10.0 (tests/netc/run-2port.sh). Never trust the name; trust the BDF.
resolve() { # $1 = PCI BDF -> prints netdev, or empty
    n=0
    while [ $n -lt 60 ]; do
        IF=$(ls "/sys/bus/pci/devices/$1/net" 2>/dev/null | head -1)
        [ -n "$IF" ] && { echo "$IF"; return 0; }
        sleep 1; n=$((n + 1))
    done
    return 1
}

IF0=$(resolve 0002:00:00.0) || { echo "ENET-LAB3 FAIL (no netdev on 0002:00:00.0 / leg0)"; poweroff -f; }
IF1=$(resolve 0002:00:08.0) || { echo "ENET-LAB3 FAIL (no netdev on 0002:00:08.0 / leg1)"; poweroff -f; }
ip link set "$IF0" up
ip link set "$IF1" up
sleep 1
echo "ENET-LAB3 boot: leg0(FRDM/LAN)=$IF0@0002:00:00.0 carrier=$(cat /sys/class/net/$IF0/carrier 2>/dev/null) | leg1(Orin/USB)=$IF1@0002:00:08.0 carrier=$(cat /sys/class/net/$IF1/carrier 2>/dev/null)"

# Two beacons, one per PHYSICAL wire. Both are imx95 (my_et 0x88B7); the peer
# each DEPENDS on differs because the two real endpoints are on two cables:
#   leg0 (PF 00.0, first -nic) watches the real FRDM  at 0x88B9
#   leg1 (PF 08.0, second -nic) watches the real Orin at 0x88BA
# Both HOLD (no deadline): to a peer, LEFT-EARLY and CRASHED are the same
# observation - departure is the coordinator's decision; it kills us.
/enet-lab3 "$IF0" 0x88B7 0x88B9 &
/enet-lab3 "$IF1" 0x88B7 0x88BA &
wait
echo "ENET-LAB3 both legs exited"
sleep 1; poweroff -f
INIT
chmod +x "$root/init"
( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$OUT/lab3-2port.cpio.gz"

# --- manifest: md5 + commit, the strings holobench pins ---
COMMIT=$(cd "$ROOT" && git rev-parse HEAD)
DIRTY=$(cd "$ROOT" && git status --porcelain -- tests/enet-lab3 tests/netc 2>/dev/null | head -1)
{
    echo "# enet-lab3 2-port macvtap artifact manifest"
    echo "built_from_commit: $COMMIT"
    [ -n "$DIRTY" ] && echo "WARNING_tree_dirty: yes (tests/enet-lab3 or tests/netc has uncommitted changes)"
    echo "machine:           imx95-19x19-evk"
    echo "leg0_pf:           0002:00:00.0   et=0x88B7 peer=0x88B9   (FRDM/LAN, 1st -nic)"
    echo "leg1_pf:           0002:00:08.0   et=0x88B7 peer=0x88BA   (Orin/USB, 2nd -nic)"
    echo "nic_model:         fsl-enetc"
    echo "initrd:            lab3-2port.cpio.gz  md5=$(md5sum "$OUT/lab3-2port.cpio.gz" | cut -d' ' -f1)"
    echo "dtb:               imx95-19x19-evk-enetc.dtb  md5=$(md5sum "$OUT/imx95-19x19-evk-enetc.dtb" | cut -d' ' -f1)"
    echo "tool_src_md5:      enet-lab3.c $(md5sum "$HERE/enet-lab3.c" | cut -d' ' -f1)"
} > "$OUT/MANIFEST.txt"

echo "=== 2-port artifact built -> $OUT ==="
cat "$OUT/MANIFEST.txt"
