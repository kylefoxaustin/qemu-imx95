#!/bin/busybox sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Make ethN mean a fixed ENETC port, in every guest.
#
# The machine has three ENETC PFs and Linux names netdevs in probe-completion
# order, so "eth0" is NOT reliably the PF at devfn 00.0 - the three PFs probe
# concurrently and the winner changes run to run. Any test that pairs a name
# with a specific backend (a -nic hubport, a -nic user, a socket link) is a
# coin flip. That cost us a ~30% flake in tests/netc/run-2port.sh, a silent
# "net tests will skip" in the code-sweep peripheral tier, and one confident,
# wrong bisect onto an innocent commit.
#
# Rename by PCI address instead. Two phases, because a direct rename can
# collide with a name Linux already handed to a different port.
#
# Result: eth0 = 0002:00:00.0, eth1 = 0002:00:08.0, eth2 = 0002:00:10.0.
# Ports with no netdev (not enabled in the dtb) are skipped, leaving the
# remaining names to fall where they were.

ENETC_BDFS="0002:00:00.0 0002:00:08.0 0002:00:10.0"

# Phase 1: park every ENETC port under a temporary, collision-free name.
i=0
for bdf in $ENETC_BDFS; do
    cur=$(ls "/sys/bus/pci/devices/$bdf/net" 2>/dev/null | head -1)
    if [ -n "$cur" ]; then
        ip link set dev "$cur" down 2>/dev/null
        ip link set dev "$cur" name "enetcp$i" 2>/dev/null
    fi
    i=$((i + 1))
done

# Phase 2: hand out the canonical names now that none are taken.
i=0
for bdf in $ENETC_BDFS; do
    if [ -e "/sys/class/net/enetcp$i" ]; then
        ip link set dev "enetcp$i" name "eth$i" 2>/dev/null
    fi
    i=$((i + 1))
done

for bdf in $ENETC_BDFS; do
    cur=$(ls "/sys/bus/pci/devices/$bdf/net" 2>/dev/null | head -1)
    echo "ZENETC $bdf = ${cur:-<none>}"
done
