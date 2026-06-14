#!/usr/bin/env bash
#
# Attach a reference LVDS LCD panel to an i.MX95 19x19 EVK device tree.
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The QEMU imx95-19x19-evk machine is deliberately FAITHFUL to the real board:
# the stock NXP dtb enables the DPU display controller but attaches no panel, so
# (exactly like a bare EVK) DRM reports "Cannot find any crtc or sizes" and there
# is no scanout. To light up the LCD you do what you'd do on real silicon -
# attach a panel - which is a device-tree change, not a machine change.
#
# This script splices the EVK's reference LVDS chain into a base dtb: it enables
# the DPU -> pixel-interleaver -> LDB(channel0) -> LVDS0-phy output path and adds
# a 1280x800 panel-lvds node, so DRM registers a connector with a fixed mode and
# /dev/fb0 (and weston's drm-backend) come up. Boot the result with `-dtb`.
#
# Usage:   attach-lcd.sh <base.dtb> <out.dtb>
#   DTC=/path/to/dtc   (default: dtc on PATH, or the kernel build tree's dtc)
#
# This is the same splice tests/weston/run.sh uses (validated: Weston desktop +
# fb0 render on the DPU). Re-run it whenever the base dtb changes.
set -euo pipefail

BASE=${1:?usage: attach-lcd.sh <base.dtb> <out.dtb>}
OUT=${2:?usage: attach-lcd.sh <base.dtb> <out.dtb>}
DTC=${DTC:-$(command -v dtc || echo "")}
[ -z "$DTC" ] && for c in \
    "$HOME/Documents/linux-imx95-build/scripts/dtc/dtc" \
    /usr/bin/dtc; do [ -x "$c" ] && DTC=$c && break; done
[ -x "${DTC:-}" ] || { echo "error: dtc not found; set DTC=/path/to/dtc" >&2; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
"$DTC" -I dtb -O dts "$BASE" > "$TMP/base.dts" 2>/dev/null

python3 - "$TMP/base.dts" "$TMP/panel.dts" <<'PY'
import sys
t = open(sys.argv[1]).read()
def span(t, hdr, start=0):
    i=t.index(hdr,start); j=i+t[i:].index('{'); d=0; k=j
    while k < len(t):
        if t[k]=='{': d+=1
        elif t[k]=='}':
            d-=1
            if d==0: return i, k+1
        k+=1
def en(t,a,b): return t[:a]+t[a:b].replace('status = "disabled";','status = "okay";',1)+t[b:]
# LDB channel 0 + its output endpoint to the panel
i,j = span(t,'ldb@4 {'); blk = t[i:j].replace('status = "disabled";','status = "okay";',1)
ci,cj = span(blk,'channel@0 {')
cb = blk[ci:cj].replace('status = "disabled";','status = "okay";',1)
cb = cb.replace('port@1 {\n\t\t\t\t\t\treg = <0x01>;',
                'port@1 {\n\t\t\t\t\t\treg = <0x01>;\n\t\t\t\t\t\tlvds0_out: endpoint { remote-endpoint = <&panel_in>; };',1)
blk = blk[:ci]+cb+blk[cj:]; t = t[:i]+blk+t[j:]
# LVDS0 phy, the DC pixel-link bridge, and the pixel-interleaver + its channel 0
a,b = span(t,'phy@8 {'); t = en(t,a,b)
hs = t.rindex('bridge@8 {',0,t.index('nxp,imx95-dc-pixel-link')); a,b = span(t,'bridge@8 {',hs); t = en(t,a,b)
ci = t.index('nxp,imx95-pixel-interleaver'); hl = t.rindex('\n',0,t.rindex('{\n',0,ci))+1
hdr = t[hl:t.index('{',hl)+1].strip()
a,b = span(t, hdr); t = en(t,a,b)
a2,b2 = span(t,'channel@0 {', t.index('nxp,imx95-pixel-interleaver')); t = en(t,a2,b2)
# Reference 1280x800 LVDS panel
panel = '''
\tpanel_lvds: panel-lvds {
\t\tcompatible = "panel-lvds"; width-mm = <217>; height-mm = <136>;
\t\tdata-mapping = "vesa-24";
\t\tpanel-timing { clock-frequency = <71000000>;
\t\t\thactive = <1280>; vactive = <800>;
\t\t\thsync-len = <70>; hfront-porch = <70>; hback-porch = <80>;
\t\t\tvsync-len = <10>; vfront-porch = <10>; vback-porch = <10>; };
\t\tport { panel_in: endpoint { remote-endpoint = <&lvds0_out>; }; };
\t};
'''
t = t.rstrip(); assert t.endswith('};'); t = t[:-2]+panel+'};\n'
open(sys.argv[2],'w').write(t)
PY

"$DTC" -I dts -O dtb -o "$OUT" "$TMP/panel.dts" 2>/dev/null
echo "wrote $OUT (DPU + reference 1280x800 LVDS panel enabled)"
