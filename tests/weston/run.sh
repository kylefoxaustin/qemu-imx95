#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Weston (Wayland) desktop on the i.MX 95, shown on the emulated DPU/LVDS
# output. Boots the NXP imx-image-full .wic rootfs from the (now read/write)
# eMMC, lets the image's own systemd + udev + logind bring up weston.service,
# and QMP-screendumps the desktop.
#
# Two things this exercises end to end:
#   - the SD/eMMC write+read datapath (a writable ext4 rootfs from a real
#     multi-GB .wic over ADMA - the "Card stuck being busy" fix), and
#   - the DPU FetchLayer/FrameGen scanout of a real Wayland compositor.
#
# Like the i.MX 93, there is no 3D GPU (the Mali is a probe-time stub), so
# weston must software-render: this test forces the weston.ini renderer to
# pixman (use-g2d=false). The default use-g2d=true wants the Mali GPU and
# weston exits with "No mali devices found". The .wic's ext4 uses newer
# features than the host e2fsprogs can write, so the renderer switch is done
# IN-GUEST: a tiny busybox initramfs mounts the rootfs, seds weston.ini, then
# switch_root's into systemd.
#
# Required (override via env): QEMU, KBUILD (Image + dtb + dtc), SM_ELF, and
# WIC - a raw imx-image-full .wic disk image (or .wic.zst). SKIPs if absent.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=${IMAGE:-$KBUILD/arch/arm64/boot/Image}
DTB=${DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
DTC=${DTC:-$KBUILD/scripts/dtc/dtc}
INITRD=${INITRD:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
# A core-image-weston / imx-image-full .wic (raw or .zst). No default location.
WIC=${WIC:-}
TMO=${TMO:-1200}

need() { [ -e "$2" ] || { echo "SKIP: missing $1: $2"; exit 0; }; }
[ -n "$WIC" ] || { echo "SKIP: set WIC=<imx-image-full .wic[.zst]> to run the Weston desktop test"; exit 0; }
need WIC "$WIC"
need QEMU "$QEMU"; need IMAGE "$IMAGE"; need DTB "$DTB"; need DTC "$DTC"
need SM_ELF "$SM_ELF"; need INITRD "$INITRD"
command -v zstd >/dev/null || { echo "SKIP: zstd needed to expand the .wic"; exit 0; }

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG="$WORK/serial.log"; QMP="$WORK/qmp.sock"; OUT=${OUT:-$WORK/weston.png}

# --- Prepare the disk: expand .zst, then grow to the next power-of-2 (the SD/
#     eMMC card model requires a power-of-2 capacity). --------------------------
DISK="$WORK/weston.wic"
case "$WIC" in
    *.zst) zstd -d -f "$WIC" -o "$DISK" ;;
    *)     cp --reflink=auto "$WIC" "$DISK" ;;
esac
cur=$(stat -c%s "$DISK"); p2=1
while [ "$p2" -lt "$cur" ]; do p2=$((p2 * 2)); done
truncate -s "$p2" "$DISK"

# --- Patched DTB: enable the DPU output chain + an LVDS panel so a DRM
#     connector registers (same splice the compositing test uses). --------------
"$DTC" -I dtb -O dts "$DTB" > "$WORK/base.dts" 2>/dev/null
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
i,j = span(t,'ldb@4 {'); blk = t[i:j].replace('status = "disabled";','status = "okay";',1)
ci,cj = span(blk,'channel@0 {')
cb = blk[ci:cj].replace('status = "disabled";','status = "okay";',1)
cb = cb.replace('port@1 {\n\t\t\t\t\t\treg = <0x01>;',
                'port@1 {\n\t\t\t\t\t\treg = <0x01>;\n\t\t\t\t\t\tlvds0_out: endpoint { remote-endpoint = <&panel_in>; };',1)
blk = blk[:ci]+cb+blk[cj:]; t = t[:i]+blk+t[j:]
a,b = span(t,'phy@8 {'); t = en(t,a,b)
hs = t.rindex('bridge@8 {',0,t.index('nxp,imx95-dc-pixel-link')); a,b = span(t,'bridge@8 {',hs); t = en(t,a,b)
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
open(sys.argv[2],'w').write(t)
PY
"$DTC" -I dts -O dtb -o "$WORK/panel.dtb" "$WORK/panel.dts" 2>/dev/null

# --- switch_root initramfs: patch weston.ini to pixman, then hand off to the
#     image's systemd. The guest kernel mounts the .wic ext4 (its features are
#     newer than the host's e2fsprogs). ----------------------------------------
STAGE="$WORK/root"; mkdir -p "$STAGE"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
i=0; while [ ! -e /dev/mmcblk0p2 ] && [ $i -lt 60 ]; do usleep 500000; i=$((i+1)); done
mkdir -p /mnt
mount -t ext4 /dev/mmcblk0p2 /mnt || { echo "WESTON-PREINIT: mount failed"; exec sh; }
ini=/mnt/etc/xdg/weston/weston.ini
sed -i 's/^use-g2d=true/use-g2d=false/' "$ini"
grep -q '^renderer=' "$ini" || sed -i 's/^use-g2d=false/use-g2d=false\nrenderer=pixman/' "$ini"
echo "WESTON-PREINIT: $(grep -E 'use-g2d|renderer' "$ini" | tr '\n' ' ')"
exec switch_root /mnt /sbin/init
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

# --- Boot + capture. ---------------------------------------------------------
"$QEMU" -M imx95-19x19-evk -m 4G -display none -vga none \
    -qmp "unix:$QMP,server=on,wait=off" \
    -kernel "$IMAGE" -dtb "$WORK/panel.dtb" -initrd "$WORK/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init systemd.log_target=console systemd.journald.forward_to_console=1" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -drive if=none,format=raw,file="$DISK",id=mmc0 -device emmc,drive=mmc0 \
    -serial file:"$LOG" -serial null >/dev/null 2>&1 &
QPID=$!

shot() { python3 - "$QMP" "$OUT" <<'PY' 2>/dev/null
import socket, json, sys
try:
    s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); f=s.makefile("rw"); f.readline()
    def c(o): f.write(json.dumps(o)+"\r\n"); f.flush(); return f.readline()
    c({"execute":"qmp_capabilities"})
    c({"execute":"screendump","arguments":{"filename":sys.argv[2],"format":"png"}})
except Exception:
    pass
PY
}
painted() { python3 - "$OUT" <<'PY' 2>/dev/null
import sys
from PIL import Image, ImageStat
im=Image.open(sys.argv[1]).convert("L"); st=ImageStat.Stat(im)
print(f"{im.width}x{im.height} mean={st.mean[0]:.1f} std={st.stddev[0]:.1f}")
sys.exit(0 if st.stddev[0] > 8 else 1)
PY
}

ok=0; start=$(date +%s)
while [ $(( $(date +%s) - start )) -lt "$TMO" ]; do
    kill -0 "$QPID" 2>/dev/null || break
    sleep 20; el=$(( $(date +%s) - start ))
    # Capture on the systemd-notify "Started Weston" marker, or - since that
    # line's timing/escapes can be flaky - once the boot is well past the logo
    # phase and the DPU is scanning out a painted 1280x800 frame (the desktop).
    mk=0; grep -qaE 'Started.*Wayland compositor' "$LOG" 2>/dev/null && mk=1
    { [ "$mk" = 1 ] || [ "$el" -gt 600 ]; } || continue
    sleep 8; shot
    info=$(painted) && { echo "  ok   weston desktop painted [$info] (marker=$mk)"; ok=1; break; }
done
shot
kill "$QPID" 2>/dev/null || true

echo "=== weston report ==="
grep -aE 'WESTON-PREINIT|Started.*Wayland compositor|No mali|weston.service.*[Ff]ail' "$LOG" 2>/dev/null | tail -5
if [ "$ok" = 1 ]; then
    cp "$OUT" "${WESTON_PNG:-$ROOT/build/weston-desktop.png}" 2>/dev/null || true
    echo "PASS: Weston desktop up on the DPU ($OUT)"; exit 0
fi
echo "FAIL: Weston desktop not confirmed within ${TMO}s ($(painted))"; exit 1
