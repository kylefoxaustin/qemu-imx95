#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Produce a NETC test DTS from the decompiled base i.MX 95 EVK device tree.
#
# The modelled ENETC PF sits at PCI devfn 08.0, which the BSP DT maps to
# ENETC1 (ethernet@8,0) - a slot the netc-blk-ctrl NETCMIX init accepts (its
# link-MII switch only handles ENETC0=0x0 and ENETC1=0x40), unlike devfn 0x10.
# In the base tree that node is "disabled" and carries an efuse nvmem MAC cell
# that never resolves under emulation. This rewrites it to an enabled node with
# a fixed MAC and a fixed-link (no real PHY/MDIO), so enetc4_pf can bind.
#
# It also rewrites the NETC pcie@4ca00000 msi-map to identity (RID -> DeviceID
# 1:1). The BSP remaps RIDs to DeviceID 0x6x, but QEMU's GICv3 ITS uses the PCI
# requester-ID directly as the DeviceID, so the BSP remap leaves the ITS with no
# matching device entry and every ENETC MSI is silently dropped.
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

NEW_NODE = (
    'ethernet@8,0 {\n'
    '\t\t\t\tcompatible = "fsl,imx95-enetc";\n'
    '\t\t\t\treg = <0x4000 0x0 0x0 0x0 0x0>;\n'
    '\t\t\t\tclocks = <0x11 0x66>;\n'
    '\t\t\t\tclock-names = "enet_ref_clk";\n'
    '\t\t\t\tlocal-mac-address = [00 04 9f 06 11 22];\n'
    '\t\t\t\tphy-mode = "rgmii-id";\n'
    '\t\t\t\tstatus = "okay";\n'
    '\t\t\t\tphandle = <0x14e>;\n'
    '\n'
    '\t\t\t\tfixed-link {\n'
    '\t\t\t\t\tspeed = <1000>;\n'
    '\t\t\t\t\tfull-duplex;\n'
    '\t\t\t\t};\n'
    '\t\t\t};'
)

# The NETC pcie@4ca00000 msi-map (8 entries, RID -> 0x85/DeviceID 0x6x).
# Matched by structure rather than a verbatim string: dtc versions differ in
# hex formatting (e.g. "0x0"/"0x1" vs zero-padded "0x00"/"0x01"), so we anchor
# on the unique 8-entry NETC signature - phandle 0x85 mapping to DeviceIDs
# 0x60..0x67 - and span the middle entries with [^>]*. The msi phandle (0x85)
# is fixed by the base tree; if dtc renumbers it this regex must be updated.
# Replaced with a single identity entry over all RIDs.
OLD_MSI_MAP = re.compile(
    r'msi-map = <0x0+ 0x85 0x60 0x0*1 [^>]*0x85 0x67 0x0*1>;')
NEW_MSI_MAP = 'msi-map = <0x0 0x85 0x0 0x10000>;'


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else '/dev/stdin'
    s = open(src).read()
    i = s.index('ethernet@8,0 {')
    j = s.index('};', i) + 2          # ethernet@8,0 has no children: first }; closes it
    if s[i:j].count('{') != 1:
        sys.exit('error: unexpected nesting in ethernet@8,0 block')
    s = s[:i] + NEW_NODE + s[j:]

    s, n = OLD_MSI_MAP.subn(NEW_MSI_MAP, s)
    if n != 1:
        sys.exit('error: NETC msi-map not matched once (got %d; phandle '
                 'renumbered or layout changed?)' % n)

    sys.stdout.write(s)


if __name__ == '__main__':
    main()
