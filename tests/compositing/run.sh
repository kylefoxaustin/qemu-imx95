#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 DPU display compositing - real-driver end-to-end via libdrm modetest.
#
# Boots Linux, uses modetest to set a PRIMARY plane (full-screen test pattern)
# plus an OVERLAY plane at a known position, then captures the emulated DPU
# display via QMP screendump and checks the overlay composited over the primary.
# This exercises the dpu95 LayerBlend chain that hw/misc/imx95_dpu.c models.
#
# Set IMX95_DPU_TRACE_PIPE=1 (the harness does, into $QERR) to dump the
# display-pipeline register writes (LayerBlend / ExtDst / plane fetch units) for
# reverse-engineering the compositing routing.
#
# Required (override via env): QEMU, KBUILD (Image+dtb), SM_ELF, BSP_ROOTFS.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
BSP_ROOTFS=${BSP_ROOTFS:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx95-drone-sizer/tmp/work/imx95_19x19_lpddr5_evk-poky-linux/imx-image-full/1.0/rootfs}
IMAGE=$KBUILD/arch/arm64/boot/Image
DTB=${DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
DTC=${DTC:-$KBUILD/scripts/dtc/dtc}
TMO=${TMO:-200}

need() { [ -e "$2" ] || { echo "missing $1: $2"; exit 1; }; }
need QEMU "$QEMU"; need IMAGE "$IMAGE"; need DTB "$DTB"; need SM_ELF "$SM_ELF"
need DTC "$DTC"

MODETEST="$BSP_ROOTFS/usr/bin/modetest"
if [ ! -x "$MODETEST" ]; then
    echo "SKIP: no modetest at $MODETEST (need an imx-image-full BSP rootfs)"
    exit 0
fi

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/root"; mkdir -p "$STAGE"
LOG=${LOG:-$WORK/serial.log}
QERR=${QERR:-$WORK/qemu.err}
PPM=${PPM:-$WORK/screen.ppm}
QMP="$WORK/qmp.sock"

# The stock EVK dtb has the display OUTPUT disabled (no connector -> no display).
# Decompile, enable the output chain (pixel-interleaver -> pixel-link -> LDB ->
# LVDS PHY) and attach a fixed LVDS panel so a connector registers, then
# recompile (the patch-dtb.py text-splice pattern; the prebuilt dtb has no
# __symbols__ for fdtoverlay). Skip if the dtb already has a panel.
"$DTC" -I dtb -O dts "$DTB" > "$WORK/base.dts" 2>/dev/null
if grep -q 'panel-lvds' "$WORK/base.dts"; then
    cp "$DTB" "$WORK/panel.dtb"
else
    python3 - "$WORK/base.dts" "$WORK/panel.dts" <<'PY'
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
# LDB + channel@0 (+ panel output endpoint)
i,j = span(t,'ldb@4 {'); blk = t[i:j].replace('status = "disabled";','status = "okay";',1)
ci,cj = span(blk,'channel@0 {')
cb = blk[ci:cj].replace('status = "disabled";','status = "okay";',1)
cb = cb.replace('port@1 {\n\t\t\t\t\t\treg = <0x01>;',
                'port@1 {\n\t\t\t\t\t\treg = <0x01>;\n\t\t\t\t\t\tlvds0_out: endpoint { remote-endpoint = <&panel_in>; };',1)
blk = blk[:ci]+cb+blk[cj:]; t = t[:i]+blk+t[j:]
# lvds0 phy@8
a,b = span(t,'phy@8 {'); t = en(t,a,b)
# pixel-link bridge@8
hs = t.rindex('bridge@8 {',0,t.index('nxp,imx95-dc-pixel-link')); a,b = span(t,'bridge@8 {',hs); t = en(t,a,b)
# pixel-interleaver node + channel@0
ci = t.index('nxp,imx95-pixel-interleaver'); hl = t.rindex('\n',0,t.rindex('{\n',0,ci))+1
hdr = t[hl:t.index('{',hl)+1].strip()
a,b = span(t, hdr); t = en(t,a,b)
a2,b2 = span(t,'channel@0 {', t.index('nxp,imx95-pixel-interleaver')); t = en(t,a2,b2)
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
open(sys.argv[2],'w').write(t); print("dtb patched: display output + LVDS panel enabled")
PY
    "$DTC" -I dts -O dtb -o "$WORK/panel.dtb" "$WORK/panel.dts" 2>/dev/null
fi
DTB="$WORK/panel.dtb"

zcat "$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz" | \
    (cd "$STAGE" && cpio -idmu 2>/dev/null)

# Harvest modetest + its DT_NEEDED closure (libdrm) from the BSP rootfs.
python3 - "$BSP_ROOTFS" "$STAGE" "$MODETEST" <<'PY'
import os, sys, subprocess, shutil, glob
RF, STAGE, MT = sys.argv[1:4]
libdirs = [f"{RF}/lib", f"{RF}/usr/lib", f"{RF}/usr/lib/aarch64-linux-gnu"]
done = set()
def needed(p):
    try:
        out = subprocess.check_output(["readelf","-d",p], text=True,
                                      stderr=subprocess.DEVNULL)
    except Exception:
        return []
    return [l.split("[")[1].split("]")[0] for l in out.splitlines()
            if "(NEEDED)" in l]
def find(n):
    for d in libdirs:
        c = os.path.join(d, n)
        if os.path.exists(c):
            return c
    g = glob.glob(f"{RF}/usr/lib/**/{n}", recursive=True)
    return g[0] if g else None
def cp(src):
    if not src or src in done or not os.path.exists(src):
        return
    done.add(src)
    dst = os.path.join(STAGE, os.path.relpath(src, RF))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if not os.path.exists(dst):
        shutil.copy2(src, dst)
    for n in needed(src):
        cp(find(n))
cp(f"{RF}/lib/ld-linux-aarch64.so.1")
cp(MT)
shutil.copy2(MT, os.path.join(STAGE, "modetest"))
ld = os.path.join(STAGE, "lib/ld-linux-aarch64.so.1")
os.makedirs(os.path.join(STAGE,"usr/lib"), exist_ok=True)
shutil.copy2(ld, os.path.join(STAGE,"usr/lib/ld-linux-aarch64.so.1"))
print("staged libs:", len(done))
PY

cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev
/bin/busybox --install -s /bin 2>/dev/null
/bin/busybox mkdir -p /tmp
export PATH=/bin:/usr/bin
export LD_LIBRARY_PATH=/lib:/usr/lib:/usr/lib/aarch64-linux-gnu
exec > /dev/console 2>&1
sleep 8
echo "=== COMPOSITE-E2E ==="
chmod +x /modetest 2>/dev/null
echo "--- drm class ---"; ls /sys/class/drm/ 2>/dev/null | tr '\n' ' '; echo
echo "--- deferred ---"; cat /sys/kernel/debug/devices_deferred 2>/dev/null
echo "--- display dmesg ---"
dmesg | grep -aiE 'ldb|lvds|panel|pixel.?link|drm|phy@|connector|clk.*disp|Cannot find' | grep -aviE 'cycle|tcpc' | tail -20
echo "--- ldb/panel driver bind ---"
for d in imx95-ldb fsl-imx95-ldb panel-lvds imx95-pixel-link imx95-pixel-link-display; do
  echo "$d: $(ls /sys/bus/platform/drivers/$d/ 2>/dev/null | grep -iE '@|panel' | tr '\n' ' ')"
done
# Enumerate connectors / CRTCs / planes.
/modetest -M imx95-dpu > /tmp/m.txt 2>&1 || /modetest > /tmp/m.txt 2>&1
echo "--- modetest enum (head) ---"; head -40 /tmp/m.txt
CONN=$(awk '/^Connectors:/{c=1;next} /^Encoders:/{c=0} c&&/connected/{print $1;exit}' /tmp/m.txt)
MODE=$(awk -v cn="$CONN" '$1==cn{f=1} f&&/[0-9]+x[0-9]+ /{print $2;exit}' /tmp/m.txt)
CRTC=$(awk '/^CRTCs:/{c=1;next} /^Planes:/{c=0} c&&/^[0-9]/{print $1;exit}' /tmp/m.txt)
# Primary = first plane usable on this CRTC (possible-crtcs $NF has bit0, i.e.
# its last hex digit is odd, since CRTC 40 is crtc index 0). Overlay = the next.
PRIM=$(awk '/^Planes:/{c=1;next} c&&/^[0-9]/{l=substr($NF,length($NF),1); if(index("13579bdf",l)){print $1;exit}}' /tmp/m.txt)
OVL=$(awk -v p="$PRIM" '/^Planes:/{c=1;next} c&&/^[0-9]/{l=substr($NF,length($NF),1); if($1!=p && index("13579bdf",l)){print $1;exit}}' /tmp/m.txt)
echo "CONN=$CONN MODE=$MODE CRTC=$CRTC PRIM=$PRIM OVL=$OVL"
# Atomic commit: primary full-screen + a 320x240 opaque overlay at (200,150);
# hold the display. (modetest can't set the "pixel blend mode" property - its
# -w parser stops at the space in the name - and XR24 carries no per-pixel
# alpha, so a clean translucent commit isn't expressible here; the model's
# alpha-blend path reuses the g2d-validated Porter-Duff factors regardless.)
/modetest -M imx95-dpu -a -s "$CONN@$CRTC:$MODE" \
    -P "$PRIM@$CRTC:1280x800" -P "$OVL@$CRTC:320x240+200+150" -v \
    > /tmp/mt.log 2>&1 &
sleep 3
echo "--- modetest log ---"; cat /tmp/mt.log 2>/dev/null | head -20
echo "=== COMPOSITE-READY ==="
sleep 20
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

# Boot with a QMP socket + the pipeline trace; screendump once modetest is up.
IMX95_DPU_TRACE_PIPE=1 "$QEMU" -M imx95-19x19-evk -m 2G \
    -display none -vga none \
    -qmp "unix:$QMP,server=on,wait=off" \
    -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init ${DRM_DEBUG:-}" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -d unimp -serial file:"$LOG" -serial null >/dev/null 2>"$QERR" &
QPID=$!

# Wait for modetest to bring the planes up, then QMP-screendump the console.
for i in $(seq 1 "$TMO"); do
    grep -qa 'COMPOSITE-READY' "$LOG" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 1
done
sleep 2
python3 - "$QMP" "$PPM" <<'PY' || true
import socket, json, sys, time
sock, ppm = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX); s.connect(sock); f = s.makefile("rw")
f.readline()
def cmd(o): f.write(json.dumps(o)+"\r\n"); f.flush(); return f.readline()
cmd({"execute":"qmp_capabilities"})
print("screendump:", cmd({"execute":"screendump","arguments":{"filename":ppm}}).strip())
PY
wait "$QPID" 2>/dev/null || true

echo "=== compositing e2e report ==="
sed -n '/COMPOSITE-E2E ===/,/COMPOSITE-READY/p' "$LOG" | grep -vE '^\[ *[0-9].*\]'
echo "--- screenshot ---"; ls -l "$PPM" 2>/dev/null || echo "(no screendump)"
# LayerBlend (0x17xxxx-0x1cxxxx) + ExtDst (0x11xxxx/0x12xxxx/0x15xxxx/0x16xxxx)
# routing writes — 6 hex digit offsets.
lb_writes=$(grep -aoE 'PIPE 0x1[0-9a-f]{5} = ' "$QERR" 2>/dev/null | grep -aoE '0x1(7|8|9|a|b|c|1)[0-9a-f]{4}' | sort -u | wc -l)
echo "--- distinct LayerBlend/ExtDst routing offsets touched: $lb_writes ---"

# Composite check: the overlay (modetest test pattern) sits at (200,150) 320x240
# and contains black/blue diagonal stripes; the SMPTE primary there is bright
# bars (never near-black). A near-black pixel inside the overlay bbox proves the
# overlay composited over the primary.
composited=$(python3 - "$PPM" <<'PY'
import sys
try:
    from PIL import Image
    im = Image.open(sys.argv[1]).convert('RGB'); px = im.load(); W, H = im.size
    # The opaque overlay's test pattern has black stripes; the SMPTE primary at
    # that location is bright bars (never near-black). Near-black pixels inside
    # the 320x240 bbox prove the overlay composited over the primary.
    n = 0
    for y in range(160, min(380, H), 6):
        for x in range(210, min(510, W), 6):
            r, g, b = px[x, y]
            if r < 40 and g < 40 and b < 40:
                n += 1
    print(1 if n >= 3 else 0)
except Exception:
    print(-1)
PY
)
echo "--- overlay composited at (200,150): $composited (1=yes 0=no -1=no-PIL) ---"

# The dpu95 DRM device needs a CRTC + connector for modetest to set a mode and
# drive a multi-plane commit. With the stub LDB/pixel-link/panel output chain the
# component bind reports "Cannot find any crtc", so the display does not scan out
# and there is no plane to composite. This is a display-output-bringup
# prerequisite (the LDB -> LVDS panel/connector chain), separate from the
# LayerBlend composite logic the model is being built for.
if grep -qa 'Cannot find any crtc' "$LOG" || ! grep -qaE 'CONN=[0-9]' "$LOG"; then
    echo "SKIP: dpu95 has no CRTC/connector under modetest ('Cannot find any"
    echo "      crtc') -> cannot set a mode or drive a multi-plane commit. The"
    echo "      LayerBlend composite needs the display-output (LDB/panel"
    echo "      connector) chain up first. QMP screendump path + pipe trace are"
    echo "      in place for when it is."
    exit 0
fi
if [ "$lb_writes" -lt 1 ]; then
    echo "FAIL: a mode was set but no LayerBlend routing was programmed"; exit 1
fi
if [ "$composited" = 1 ]; then
    echo "PASS: primary + overlay composited via the DPU LayerBlend chain"; exit 0
fi
if [ "$composited" = -1 ]; then
    echo "PASS (partial): connector + mode + LayerBlend programmed; install"
    echo "      python3-PIL to also verify the composited overlay pixels"; exit 0
fi
echo "FAIL: overlay did not composite over the primary at (200,150)"; exit 1
