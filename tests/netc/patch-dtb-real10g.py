#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Produce a NETC test DTS from the decompiled base i.MX 95 device tree.
#
# The machine models two ENETC station-interface PFs at the PCI devfns the BSP
# DT maps to ENETC0 (ethernet@0,0 = 00.0) and ENETC1 (ethernet@8,0 = devfn
# 0x40) - the 1G port pair the 15x15 FRDM (and the 19x19 EVK's 1G side) pin
# REAL-CHAIN VARIANT (Path B): the 1G ports (ethernet@0,0 / @8,0) are fixed-link
# so they bind, but the 10G port (ethernet@10,0) KEEPS its real phy-handle to the
# Aquantia AQR113C on the EMDIO and managed=in-band-status - only the efuse nvmem
# MAC (which never resolves under emulation) is swapped for a fixed local-mac.
# So the 10G port comes up through the real EMDIO -> AQR113C -> xPCS chain, not a
# fixed-link fake. In the base trees these nodes either carry an efuse nvmem MAC
# never resolves under emulation, or a phy-handle to an external EMDIO PHY we
# do not model. This rewrites both to enabled nodes with a fixed MAC and a
# fixed-link (no PHY/MDIO), so enetc4_pf binds each one. Each node's real reg /
# clocks / phandle are preserved from the base tree, so the same script works
# on either board's decompiled DTS.
#
# It also rewrites the NETC pcie@4ca00000 msi-map to identity (RID -> DeviceID
# 1:1, spanning all RIDs). The BSP remaps RIDs to DeviceID 0x6x, but QEMU's
# GICv3 ITS uses the PCI requester-ID directly as the DeviceID, so the BSP remap
# leaves the ITS with no matching device entry and every ENETC MSI is silently
# dropped. The single identity entry covers both ports (RID 0x0 and 0x40).
#
# The bus-1 EMDIO ECAM (pcie@4cb00000) and any 10G ethernet@10,0 are left as the
# base tree has them: those PCI functions have no backing device in the model,
# so Linux enumerates an empty bus and their DT nodes (and any PHY children /
# reset-gpios) stay inert. No need to touch them.
#
# Why a Python splice and not fdtoverlay: fdtoverlay cannot delete properties
# (the nvmem-cells) or rewrite the msi-map, and the host may not ship fdtoverlay
# at all. We edit the decompiled DTS text and recompile with the kernel's dtc.
#
# Usage:
#   dtc -I dtb -O dts base.dtb > base.dts
#   ./patch-dtb.py base.dts > netc.dts
#   dtc -I dts -O dtb -o netc.dtb netc.dts
#
# (tests/netc/run.sh does all three.)
import re
import sys

# Per-port (fixed MAC, fixed-link speed Mbps, phy-mode). Distinct MACs so a
# back-to-back test has different src/dst. ENETC2 is the 10G port (devfn 0x80,
# ethernet@10,0): a fixed-link 10gbase-r node, so enetc4_pf brings it up at 10G
# without the real Aquantia PHY / EMDIO. The MAC datapath is identical to the
# 1G ports - "10G" is just the link speed phylink reports.
PORT_CFG = {
    '0,0':  ('00 04 9f 06 11 22', 1000,  'rgmii-id'),    # ENETC0 (1G)
    '8,0':  ('00 04 9f 06 11 33', 1000,  'rgmii-id'),    # ENETC1 (1G)
}   # ENETC2 (10,0) stays REAL below (rewrite_10g_real)


def get_prop(node, name):
    """Return the raw '<...>'/'"..."' RHS of a property, or None if absent."""
    m = re.search(r'\n\s*' + re.escape(name) + r'\s*=\s*([^;]*);', node)
    return m.group(1).strip() if m else None


def rewrite_port(s, unit, mac, speed, phy_mode):
    """Replace the ethernet@<unit> node with an enabled fixed-link node,
    preserving its base reg / clocks / clock-names / phandle."""
    i = s.index('ethernet@%s {' % unit)
    j = s.index('};', i) + 2          # property-only node: first }; closes it
    node = s[i:j]
    if node.count('{') != 1:
        sys.exit('error: unexpected nesting in ethernet@%s block' % unit)

    reg = get_prop(node, 'reg')
    if reg is None:
        sys.exit('error: ethernet@%s has no reg' % unit)
    clocks = get_prop(node, 'clocks')
    clock_names = get_prop(node, 'clock-names')
    phandle = get_prop(node, 'phandle')

    L = ['ethernet@%s {' % unit,
         '\t\t\t\tcompatible = "fsl,imx95-enetc";',
         '\t\t\t\treg = %s;' % reg]
    if clocks is not None:
        L.append('\t\t\t\tclocks = %s;' % clocks)
    if clock_names is not None:
        L.append('\t\t\t\tclock-names = %s;' % clock_names)
    L += ['\t\t\t\tlocal-mac-address = [%s];' % mac,
          '\t\t\t\tphy-mode = "%s";' % phy_mode,
          '\t\t\t\tstatus = "okay";']
    if phandle is not None:
        L.append('\t\t\t\tphandle = %s;' % phandle)
    L += ['',
          '\t\t\t\tfixed-link {',
          '\t\t\t\t\tspeed = <%d>;' % speed,
          '\t\t\t\t\tfull-duplex;',
          '\t\t\t\t};',
          '\t\t\t};']
    return s[:i] + '\n'.join(L) + s[j:]


# The NETC pcie@4ca00000 msi-map (8 entries, RID -> <its>/DeviceID 0x6x).
# Matched by structure rather than a verbatim string for portability:
#  - dtc versions differ in hex formatting ("0x0"/"0x1" vs "0x00"/"0x01"), and
#  - the ITS phandle is assigned per-DTB (e.g. 0x85 on the 19x19 EVK build,
#    0x79 on the 15x15 FRDM build), so it must not be hardcoded.
# We anchor on the unique NETC signature - the first entry mapping to DeviceID
# 0x60 and the last to 0x67 via the *same* phandle - capturing that phandle
# (group 1, required to repeat by the \1 backref so other PCIe msi-maps on the
# board, which use DeviceID bases 0x10/0x98, can't match) and spanning the
# middle entries with [^>]*. Replaced with a single identity entry over all RIDs
# (covers all three ports: RID 0x0, 0x40, 0x80), reusing the captured ITS phandle.
OLD_MSI_MAP = re.compile(
    r'msi-map = <0x0+ (0x[0-9a-f]+) 0x60 0x0*1 [^>]*\1 0x67 0x0*1>;')
NEW_MSI_MAP = r'msi-map = <0x0 \g<1> 0x0 0x10000>;'


def rewrite_10g_real(s):
    """Keep the stock ethernet@10,0 real chain (phy-handle -> AQR c45 on the
    EMDIO, managed=in-band-status) - only swap the never-resolving efuse nvmem
    MAC for a fixed local-mac-address so fw_devlink stops blocking the PF."""
    i = s.index('ethernet@10,0 {')
    j = s.index('};', i) + 2
    node = s[i:j]
    out = []
    for line in node.splitlines():
        if 'nvmem-cells' in line:
            out.append('\t\t\t\tlocal-mac-address = [00 04 9f 06 11 44];')
        elif 'nvmem-cell-names' in line:
            continue
        else:
            out.append(line)
    return s[:i] + '\n'.join(out) + s[j:]


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else '/dev/stdin'
    s = open(src).read()

    for unit, (mac, speed, phy_mode) in PORT_CFG.items():
        s = rewrite_port(s, unit, mac, speed, phy_mode)

    s = rewrite_10g_real(s)
    s, n = OLD_MSI_MAP.subn(NEW_MSI_MAP, s)
    if n != 1:
        sys.exit('error: NETC msi-map not matched once (got %d; phandle '
                 'renumbered or layout changed?)' % n)

    sys.stdout.write(s)


if __name__ == '__main__':
    main()
