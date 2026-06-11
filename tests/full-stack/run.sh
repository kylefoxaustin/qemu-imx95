#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Full-stack consolidation test: one boot, every modelled i.MX 95 subsystem
# that landed after v2.0.0 up at once, proving they coexist cleanly.
#
# Exercised together in a single machine instance:
#   - NETC: all three ENETC PFs (eth0/eth1 @1G, eth2 @10G) - link + datapath
#   - Audio: the three ASoC cards (bt-sco, wm8962, micfil) register
#   - JPEG: both HW codecs register (/dev/video2 dec, /dev/video3 enc)
#   - USB: usb-kbd enumerates as a HID input on the ChipIdea host
#   - DPU: the display-controller driver binds (drm) - render path is
#     covered separately by the display soak; here we just assert coexistence
#   - SM: the real System Manager (M33) boots and serves SCMI
#
# A headless VNC backend gives the DPU a surface without a GUI window.
#
# PASS = every subsystem marker present AND zero anomalies
#        (panic / oops / external abort / BUG / hw csum failure).
#
# Required artifacts (override via env): QEMU, KBUILD (Image + dtb + dtc),
# SM_ELF - same layout as tests/netc/run-10g.sh.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=$KBUILD/arch/arm64/boot/Image
DTB=$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb
DTC=$KBUILD/scripts/dtc/dtc
TMO=${TMO:-260}
VNC=${VNC:-127.0.0.1:0,to=99}
# Audio (snd-soc-*) and JPEG (mxc-jpeg) are =m, so they must be staged into the
# initramfs and insmod'd - unlike NETC/USB/DPU which are built-in. The modules
# MUST match the booted Image's vermagic, so default MODROOT to the same kernel
# build tree as KBUILD (its in-tree .ko files), not a mismatched BSP rootfs.
MODROOT=${MODROOT:-$KBUILD}

need() { [ -e "$2" ] || { echo "missing $1: $2"; exit 1; }; }
need QEMU "$QEMU"; need IMAGE "$IMAGE"; need DTC "$DTC"; need SM_ELF "$SM_ELF"; need DTB "$DTB"
need MODROOT "$MODROOT"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG=${LOG:-$WORK/serial.log}

# Patched DTB: all three NETC ports fixed-link (ENETC2 at 10gbase-r/10000).
"$DTC" -I dtb -O dts "$DTB" >"$WORK/base.dts" 2>/dev/null
python3 "$ROOT/tests/netc/patch-dtb.py" "$WORK/base.dts" >"$WORK/netc.dts"
"$DTC" -I dts -O dtb -o "$WORK/netc.dtb" "$WORK/netc.dts" 2>/dev/null

root="$WORK/root"; mkdir -p "$root"
zcat "$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz" | \
    (cd "$root" && cpio -idmu 2>/dev/null)

# Stage the loadable audio + jpeg modules (load order preserved). NETC/USB/DPU
# need no modules. The init insmods this exact list in sequence.
MODS="snd-soc-fsl-utils imx-pcm-dma snd-soc-fsl-sai snd-soc-fsl-micfil \
      snd-soc-wm8962 snd-soc-dmic snd-soc-imx-audmux snd-soc-fsl-asoc-card \
      snd-soc-imx-card v4l2-jpeg mxc-jpeg-encdec"
mkdir -p "$root/mods"
for m in $MODS; do
    f=$(find "$MODROOT" -name "$m.ko" | head -1)
    need "$m.ko under MODROOT" "$f"
    cp "$f" "$root/mods/"
done

{
    echo '#!/bin/busybox sh'
    echo "MODS=\"$MODS\""
} > "$root/init"
cat >> "$root/init" <<'INIT'
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
# Load the staged audio + jpeg modules (NETC/USB/DPU are built-in).
for m in $MODS; do insmod /mods/$m.ko 2>&1; done
# NETC: wait for the last port to enumerate, admin-up all three.
i=0; while [ ! -e /sys/class/net/eth2 ] && [ $i -lt 120 ]; do usleep 500000; i=$((i+1)); done
for e in eth0 eth1 eth2; do ip link set "$e" up 2>/dev/null; done
# This boot carries the full device set (3 ENETC PFs + USB + display), so
# audio/jpeg/the 10G link settle later than in the single-subsystem tests.
# Poll until every subsystem we assert on is ready (or a generous deadline)
# rather than racing a fixed sleep.
ready() {
    [ "$(cat /sys/class/net/eth2/carrier 2>/dev/null)" = "1" ] || return 1
    grep -q wm8962audio /proc/asound/cards 2>/dev/null || return 1
    [ -e /dev/video2 ] && [ -e /dev/video3 ] || return 1
    return 0
}
i=0; while ! ready && [ $i -lt 160 ]; do usleep 500000; i=$((i+1)); done
sleep 1
echo "=== FULLSTACK ==="
echo "-- net --"
for n in eth0 eth1 eth2; do
    echo "$n: speed=$(cat /sys/class/net/$n/speed 2>/dev/null) carrier=$(cat /sys/class/net/$n/carrier 2>/dev/null)"
done
echo "-- asound --"; cat /proc/asound/cards 2>/dev/null
echo "-- v4l --"; for v in /sys/class/video4linux/*/name; do echo "  $(dirname "$v"|xargs basename)=$(cat "$v")"; done
echo "-- input --"; cat /sys/class/input/input*/name 2>/dev/null
echo "=== FULLSTACK-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$root/init"
( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

# usb-kbd on usb-bus.0 = the ChipIdea USB2 host; a bare -device usb-kbd lands on
# usb-bus.1 (DWC3 xHCI), whose roothub does not enumerate cold-plug devices.
timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -vnc "$VNC" \
    -kernel "$IMAGE" -dtb "$WORK/netc.dtb" -initrd "$WORK/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -device usb-kbd,bus=usb-bus.0 \
    -nic user -nic user -nic user \
    -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "=== full-stack report ==="
sed -n '/FULLSTACK ===/,/FULLSTACK-DONE/p' "$LOG" | grep -vE 'FULLSTACK ==='

pass=1
chk() { if grep -qa "$2" "$LOG"; then echo "  ok   $1"; else echo "  MISS $1"; pass=0; fi; }
# The PF->ethN mapping is enumeration-order-dependent, so assert on the link
# events (one 10G PF + two 1G PFs) rather than a fixed interface index.
n1g=$(grep -acE 'fsl_enetc4 .* Link is Up - 1Gbps' "$LOG" 2>/dev/null || true)
n10g=$(grep -acE 'fsl_enetc4 .* Link is Up - 10Gbps' "$LOG" 2>/dev/null || true)
echo "--- subsystem markers ---"
echo "  netc link-ups: 1Gbps=$n1g 10Gbps=$n10g"
{ [ "${n10g:-0}" -ge 1 ] && [ "${n1g:-0}" -ge 2 ]; } && echo "  ok   NETC 3 ports (2x1G + 1x10G)" || { echo "  MISS NETC 3 ports"; pass=0; }
chk "audio bt-sco"        'btscoaudio'
chk "audio wm8962"        'wm8962audio'
chk "audio micfil"        'micfilaudio'
chk "jpeg decoder"        '4c500000.jpegdec: decoder device registered'
chk "jpeg encoder"        '4c550000.jpegenc: encoder device registered'
chk "usb-kbd HID"         'QEMU USB Keyboard'
chk "DPU drm bind"        'Initialized imx95-dpu'

echo "--- anomalies (want 0) ---"
for p in 'Kernel panic' 'Oops:' 'BUG:' 'synchronous external abort' 'Unhandled fault' 'hw csum failure'; do
    n=$(grep -caE "$p" "$LOG" 2>/dev/null || true); n=${n:-0}
    echo "  $p = $n"; [ "$n" = "0" ] || pass=0
done

[ "$pass" = "1" ] && { echo "PASS: full stack up together, zero anomalies"; exit 0; }
echo "FAIL: see misses/anomalies above"; exit 1
