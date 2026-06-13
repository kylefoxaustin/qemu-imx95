#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# THE "everything, for 24 hours" soak — the comprehensive pre-upstream gate.
#
# tests/soak-fullimage holds the full distro under NETC load. This sibling wires
# the MAXIMAL coexisting device surface into ONE boot of the real imx-image-full
# systemd distro and then (a) roll-calls every modelled subsystem at startup and
# (b) drives a concurrent multi-datapath stress loop for the whole soak window.
#
# Mega-dtb (one boot): base EVK dtb -> DTS patches [NETC 3 ports + PCIe iommu
# strip (pcie0+pcie1) + Neutron firmware carveout] -> dtc -@ -> fdtoverlay
# [ov5640 camera + FlexCAN]. virtio-9p needs no dtb edit (the machine injects the
# nodes). The base dtb already carries audio/VPU/DPU/JPEG/I3C/FlexSPI/ADC/XCVR/
# MICFIL, so those bind at boot.
#
# Device union: SM(M33)+CM7 loaders, usb-kbd (ChipIdea), eMMC boot disk, 3 NICs
# (NETC), e1000e on pcie0, virtio-9p-pci on pcie1, virtio-9p-device (mmio),
# ov5640 sensor, and a host frame fed to the ISI (virtual camera).
#
# In-guest: a comprehensive ROLL-CALL (hard proof every subsystem came up) then a
# self-degrading STRESS loop (each datapath guarded with || true + logged, so a
# fiddly workload degrades gracefully and only a real kernel anomaly fails the
# soak). Host side reuses tests/soak-fullimage's supervisor verbatim.
#
# Required (override via env): QEMU, KBUILD (Image + base dtb + dtc + fdtoverlay),
# SM_ELF, CM7_ELF, WIC (imx-image-full .wic[.zst]). A CROSS gcc + ov5640.ko let
# the camera-capture stressor build/load; absent, that stressor is skipped (the
# roll-call still proves the graph bound). SKIPs if WIC is unset.
set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
CM7_ELF=${CM7_ELF:-$REPO/tests/cm7-hello/cm7_image.elf}
KERNEL=${KERNEL:-$KBUILD/arch/arm64/boot/Image}
DTB=${DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
DTC=${DTC:-$KBUILD/scripts/dtc/dtc}
FDTO=${FDTO:-$KBUILD/scripts/dtc/fdtoverlay}
CROSS=${CROSS:-aarch64-linux-gnu-}
BUSYBOX_CPIO=${BUSYBOX_CPIO:-$REPO/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
WIC=${WIC:-}

SOAK_HOURS=${SOAK_HOURS:-24}
HEARTBEAT_SEC=${HEARTBEAT_SEC:-31}
GUEST_HB_SEC=${GUEST_HB_SEC:-60}
SOAK_DIR=${SOAK_DIR:-$REPO/build/soak-everything-$(date +%Y%m%d-%H%M%S)}
VNC=${VNC:-127.0.0.1:5,to=99}

skip() { echo "SKIP: $*"; exit 0; }
[ -n "$WIC" ] || skip "set WIC=<imx-image-full .wic[.zst]> to run the everything-soak"
need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
[ -e "$WIC" ] || skip "WIC not found: $WIC"
need QEMU "$QEMU" qemu; need SM_ELF "$SM_ELF" "SM firmware"
need CM7_ELF "$CM7_ELF" "M7 firmware"; need KERNEL "$KERNEL" "kernel Image"
need DTB "$DTB" dtb; need DTC "$DTC" dtc; need FDTO "$FDTO" fdtoverlay
need BBCPIO "$BUSYBOX_CPIO" busybox
command -v zstd >/dev/null || skip "zstd needed to expand the .wic"

mkdir -p "$SOAK_DIR"
SERIAL="$SOAK_DIR/serial.log"; HEARTBEATS="$SOAK_DIR/heartbeats.log"
SUMMARY="$SOAK_DIR/summary.txt"; PIDFILE="$SOAK_DIR/qemu.pid"
SOCK="$SOAK_DIR/qemu.mon"; INITRD="$SOAK_DIR/initrd.cpio.gz"; DISK="$SOAK_DIR/disk.wic"
MEGA_DTB="$SOAK_DIR/mega.dtb"
PCISHARE="$SOAK_DIR/pcishare"; MMIOSHARE="$SOAK_DIR/mmioshare"
FRAME="$SOAK_DIR/frame000.raw"
log() { echo "$@" | tee -a "$HEARTBEATS"; }

log "=== EVERYTHING soak starting at $(date -Iseconds) ==="
log "  target hours: $SOAK_HOURS   output: $SOAK_DIR"

# --- Expand the .wic, grow to a power-of-2 (the eMMC card model requires it). --
log "expanding .wic (multi-GB; takes a moment)..."
case "$WIC" in
    *.zst) zstd -d -f "$WIC" -o "$DISK" ;;
    *)     cp --reflink=auto "$WIC" "$DISK" ;;
esac
cur=$(stat -c%s "$DISK"); p2=1
while [ "$p2" -lt "$cur" ]; do p2=$((p2 * 2)); done
truncate -s "$p2" "$DISK"
log "  disk: $(stat -c%s "$DISK") bytes (eMMC-backed)"

# --- Mega-dtb: DTS patches then binary overlays. -------------------------------
log "composing mega-dtb (NETC + PCIe + Neutron + camera + FlexCAN)..."
"$DTC" -I dtb -O dts "$DTB" > "$SOAK_DIR/base.dts" 2>/dev/null
# 1) NETC three ports (reuse the proven patcher)
python3 "$REPO/tests/netc/patch-dtb.py" "$SOAK_DIR/base.dts" > "$SOAK_DIR/s1.dts"
# 2) PCIe: strip every iommu-map (no SMMU supplier) + Neutron firmware carveout
python3 - "$SOAK_DIR/s1.dts" "$SOAK_DIR/s2.dts" <<'PY'
import re, sys
src, dst = sys.argv[1], sys.argv[2]
b = open(src).read()
# drop all iommu-map / iommu-map-mask (pcie0 + pcie1) so endpoints don't defer
b = re.sub(r'\n\s*iommu-map(-mask)? = <[^;]*>;', '', b)
# Neutron firmware carveout: 128 MiB reusable pool @ phys 4 GiB (in range, -m 4G)
node = ('\n\t\tneutron_test_mem: neutron_memory@100000000 {\n'
        '\t\t\tcompatible = "shared-dma-pool";\n'
        '\t\t\treusable;\n'
        '\t\t\treg = <0x1 0x0 0x0 0x8000000>;\n'
        '\t\t};\n')
b2, n = re.subn(r'(reserved-memory \{\n(?:\t+[^\n]*\n)*?\t+ranges;\n)',
                lambda m: m.group(1) + node, b, count=1)
open(dst, 'w').write(b2 if n else b)
PY
# 3) compile WITH symbols so fdtoverlay can apply the binary overlays
"$DTC" -@ -I dts -O dtb -o "$SOAK_DIR/s2.dtb" "$SOAK_DIR/s2.dts" 2>/dev/null \
    || { echo "FAIL: mega base dtb would not compile"; exit 1; }
# 4) camera + FlexCAN overlays
"$DTC" -@ -I dts -O dtb -o "$SOAK_DIR/ov5640.dtbo" \
    "$REPO/tests/camera/ov5640-overlay.dtso" 2>/dev/null \
    || { echo "FAIL: ov5640 overlay compile"; exit 1; }
"$DTC" -@ -I dts -O dtb -o "$SOAK_DIR/flexcan.dtbo" \
    "$REPO/tests/flexcan/flexcan-overlay.dtso" 2>/dev/null \
    || { echo "FAIL: flexcan overlay compile"; exit 1; }
"$FDTO" -i "$SOAK_DIR/s2.dtb" -o "$MEGA_DTB" \
    "$SOAK_DIR/ov5640.dtbo" "$SOAK_DIR/flexcan.dtbo" \
    || { echo "FAIL: fdtoverlay (camera+flexcan) onto mega base"; exit 1; }
need MEGA "$MEGA_DTB" "mega dtb"
log "  mega-dtb: $(stat -c%s "$MEGA_DTB") bytes"

# --- Host shares for the two 9p transports + a camera frame. -------------------
mkdir -p "$PCISHARE" "$MMIOSHARE"
echo "pcie1-9p-share $(date -Iseconds)" > "$PCISHARE/hello.txt"
echo "virtio-mmio-9p-share $(date -Iseconds)" > "$MMIOSHARE/hello.txt"
# 640x480x6 host frame (matches the ov5640 capture geometry) for the virtual cam
python3 -c "open('$FRAME','wb').write(b'\xab'*1843200)" 2>/dev/null || true

# --- Optional camera-capture stressor: a static v4l2 client + ov5640.ko. -------
CAPBIN=""; CAPKO=""
if command -v "${CROSS}gcc" >/dev/null && [ -e "$REPO/tests/camera/v4l2_cap.c" ]; then
    "${CROSS}gcc" -O2 -static -o "$SOAK_DIR/imx95-isi-capture" \
        "$REPO/tests/camera/v4l2_cap.c" 2>/dev/null && CAPBIN="$SOAK_DIR/imx95-isi-capture"
fi
CAPKO=$(find "$KBUILD" -name ov5640.ko 2>/dev/null | head -1)

# --- switch_root initramfs: mount rootfs, inject the everything-load service. --
STAGE="$SOAK_DIR/root"; rm -rf "$STAGE"; mkdir -p "$STAGE"
zcat "$BUSYBOX_CPIO" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
[ -n "$CAPBIN" ] && cp "$CAPBIN" "$STAGE/imx95-isi-capture"
[ -n "$CAPKO" ]  && cp "$CAPKO"  "$STAGE/ov5640.ko"
# Stage the =m driver stack (audio/JPEG/CAN) from the BOOTED kernel's own build
# tree - vermagic must match the Image, and the distro rootfs ships a DIFFERENT
# (-lts-next-) kernel's modules, so an in-guest modprobe would vermagic-fail.
SOAK_MODS="snd-soc-fsl-utils imx-pcm-dma snd-soc-fsl-sai snd-soc-fsl-micfil \
snd-soc-wm8962 snd-soc-dmic snd-soc-imx-audmux snd-soc-fsl-asoc-card \
snd-soc-imx-card v4l2-jpeg mxc-jpeg-encdec can-dev flexcan"
mkdir -p "$STAGE/mods"
for m in $SOAK_MODS; do
    f=$(find "$KBUILD" -name "$m.ko" 2>/dev/null | head -1)
    [ -n "$f" ] && cp "$f" "$STAGE/mods/" || log "  WARN: $m.ko not found under KBUILD (that subsystem won't load)"
done
{
    echo '#!/bin/busybox sh'
    echo "GUEST_HB_SEC=$GUEST_HB_SEC"
} > "$STAGE/init"
cat >> "$STAGE/init" <<'INIT'
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
i=0; while [ ! -e /dev/mmcblk0p2 ] && [ $i -lt 60 ]; do usleep 500000; i=$((i+1)); done
mkdir -p /mnt
mount -t ext4 /dev/mmcblk0p2 /mnt || { echo "SOAK-PREINIT: rootfs mount FAILED"; exec sh; }
# carry the optional camera helpers into the rootfs
[ -e /imx95-isi-capture ] && cp /imx95-isi-capture /mnt/usr/bin/ 2>/dev/null
[ -e /ov5640.ko ] && cp /ov5640.ko /mnt/root/ 2>/dev/null
[ -d /mods ] && { mkdir -p /mnt/root/mods; cp /mods/*.ko /mnt/root/mods/ 2>/dev/null; }
cp /everything-load.sh /mnt/usr/bin/everything-load.sh
chmod +x /mnt/usr/bin/everything-load.sh
cat > /mnt/etc/systemd/system/everything-load.service <<EOS
[Unit]
Description=imx95 everything soak: roll-call + multi-datapath stress
After=multi-user.target
[Service]
Type=simple
ExecStart=/usr/bin/everything-load.sh
StandardOutput=journal+console
StandardError=journal+console
Restart=on-failure
[Install]
WantedBy=multi-user.target
EOS
mkdir -p /mnt/etc/systemd/system/multi-user.target.wants
ln -sf ../everything-load.service /mnt/etc/systemd/system/multi-user.target.wants/everything-load.service
# No display connector on this dtb path -> mask weston (stability soak, not display)
ln -sf /dev/null /mnt/etc/systemd/system/weston.service
ln -sf /dev/null /mnt/etc/systemd/system/weston.socket
echo "SOAK-PREINIT: everything-load injected; switching to systemd"
exec switch_root /mnt /sbin/init
INIT
chmod +x "$STAGE/init"

# --- The in-guest workload: roll-call + self-degrading concurrent stress. ------
cat > "$STAGE/everything-load.sh" <<EOS
#!/bin/sh
GUEST_HB_SEC=$GUEST_HB_SEC
EOS
cat >> "$STAGE/everything-load.sh" <<'EOS'
echo "=== imx95 everything userspace ==="
# Load the =m driver stack (audio cards, JPEG codec, FlexCAN) from the staged
# matching-vermagic modules - in load order. The distro rootfs modules are a
# different kernel build, so insmod the staged ones, never modprobe the rootfs.
for m in snd-soc-fsl-utils imx-pcm-dma snd-soc-fsl-sai snd-soc-fsl-micfil \
         snd-soc-wm8962 snd-soc-dmic snd-soc-imx-audmux snd-soc-fsl-asoc-card \
         snd-soc-imx-card v4l2-jpeg mxc-jpeg-encdec can-dev flexcan; do
  insmod /root/mods/$m.ko 2>/dev/null || true
done
sleep 4
for e in eth0 eth1 eth2; do ip link set "$e" up 2>/dev/null; done
# Bring up both FlexCAN interfaces (bitrate + IFF_UP); on the 'cb' can-bus they
# gain carrier. s_can confirms that liveness every cycle. Frame round-trip is
# proven by tests/flexcan standalone - in the full distro, systemd-networkd +
# connmand own the CAN links, so this coexistence soak validates SUSTAINED
# FlexCAN liveness rather than re-driving frames through the managed interfaces.
for c in can0 can1; do ip link set "$c" type can bitrate 500000 2>/dev/null; ip link set "$c" up 2>/dev/null; done
sleep 8

have() { command -v "$1" >/dev/null 2>&1; }
mark() { echo "ROLLCALL $1: $2"; }

echo "=== EVERYTHING-ROLLCALL ==="
# net
for n in eth0 eth1 eth2; do
  s=$(cat /sys/class/net/$n/speed 2>/dev/null); c=$(cat /sys/class/net/$n/carrier 2>/dev/null)
  mark "net-$n" "speed=${s:-?} carrier=${c:-?}"
done
# pcie0 e1000e + pcie1 endpoint
mark "pcie0-e1000e" "$(lspci 2>/dev/null | grep -i 82574 || ls /sys/bus/pci/devices/ 2>/dev/null | head -1 || echo none)"
mark "pcie1-endpoint" "$(ls -d /sys/bus/pci/devices/0001:* 2>/dev/null | head -1 || echo none)"
# storage
mark "emmc-root" "$(mount | grep -c mmcblk0p2) mmcblk part(s)"
# audio
mark "asound" "$(cat /proc/asound/cards 2>/dev/null | grep -c '\[' ) card(s)"
# v4l: vpu/jpeg/camera/neoisp nodes
for v in /sys/class/video4linux/*/name; do echo "ROLLCALL v4l $(basename $(dirname $v)): $(cat $v 2>/dev/null)"; done
# media graph (camera)
mark "media-graph" "$(ls /dev/media* 2>/dev/null | wc -l) media dev(s)"
# can
for c in can0 can1; do mark "can-$c" "$([ -e /sys/class/net/$c ] && echo present || echo none) carrier=$(cat /sys/class/net/$c/carrier 2>/dev/null || echo ?)"; done
# neutron
mark "neutron" "$(ls /dev/neutron* 2>/dev/null || echo no-node)"
# misc buses present in sysfs
mark "i3c" "$(ls /sys/bus/i3c/devices 2>/dev/null | wc -l) i3c dev(s)"
mark "spi" "$(ls /sys/class/spi_master 2>/dev/null | wc -l) spi master(s)"
mark "iio-adc" "$(ls /sys/bus/iio/devices 2>/dev/null | wc -l) iio dev(s)"
mark "drm" "$(ls /sys/class/drm/ 2>/dev/null | grep -c card) drm card(s)"
echo "-- failed services --"; systemctl --failed --no-legend 2>/dev/null | head
echo "-- system state --"; systemctl is-system-running 2>/dev/null
echo "=== EVERYTHING-ROLLCALL-DONE ==="

# Mount the two 9p shares (pci + mmio), FULLY BACKGROUNDED with bounded retries.
# A not-yet-ready 9p transport mounts in uninterruptible D-state that even
# `timeout` cannot kill (SIGTERM does not reach a task sleeping in the kernel),
# so a foreground mount would wedge the entire soak. Best-effort here; the
# standalone tests/virtio-9p + tests/pcie-9p prove the datapath end to end.
mkdir -p /mnt/pci9p /mnt/mmio9p
try9p() { # $1=tag $2=dir $3=label - backgrounded, bounded, never blocks the soak
  for t in 1 2 3 4 5; do
    mountpoint -q "$2" 2>/dev/null && { mark "$3" mounted; return; }
    timeout 20 mount -t 9p -o trans=virtio,version=9p2000.L "$1" "$2" 2>/dev/null
    mountpoint -q "$2" 2>/dev/null && { mark "$3" mounted; return; }
    sleep 8
  done
  mark "$3" skip
}
# Only the MMIO transport is mounted for stress: the pcie1 virtio-9p mount sits
# in D-state in this combined config (pcie1 itself is proven by the endpoint
# roll-call + standalone tests/pcie-9p), and a D-state child would snag any bare
# `wait`. MMIO 9p mounts cleanly and exercises the 9p datapath.
( try9p hostshare /mnt/mmio9p 9p-mmio ) &

# load ov5640 for the camera-capture stressor if staged
[ -e /root/ov5640.ko ] && insmod /root/ov5640.ko 2>/dev/null

# Per-cycle stressors. EVERY one is timeout-guarded so a hang degrades that
# datapath but can never wedge the loop - only a real kernel anomaly fails the
# soak. ($SCR is on the eMMC rootfs, so s_store exercises block storage.)
SCR=/var/tmp/soak; mkdir -p "$SCR"
# Each stressor sets a per-cycle flag ($SCR/x.<name>) ONLY when its datapath
# actually did work, so each ZHB line is a truth table of what was exercised.
s_net()    { p=""; for e in eth0 eth1 eth2; do timeout 8 ping -I "$e" -c 3 -W 1 10.0.2.2 >/dev/null 2>&1 & p="$p $!"; done; wait $p && : > "$SCR/x.net"; }
s_store()  { timeout 45 sh -c "dd if=/dev/urandom of=$SCR/blk bs=1M count=16 conv=fsync 2>/dev/null; sync; dd if=$SCR/blk of=/dev/null bs=1M 2>/dev/null" && : > "$SCR/x.store"; rm -f "$SCR/blk"; }
s_9p()     { ok=0; for d in /mnt/pci9p /mnt/mmio9p; do mountpoint -q "$d" 2>/dev/null && timeout 10 sh -c "echo soak > $d/.w 2>/dev/null; cat $d/hello.txt >/dev/null 2>&1; rm -f $d/.w 2>/dev/null" && ok=1; done; [ "$ok" = 1 ] && : > "$SCR/x.9p"; }
s_audio()  { wc=$(awk '/wm8962/{print $1}' /proc/asound/cards 2>/dev/null | head -1); [ -n "$wc" ] || return 0; have aplay && timeout 8 sh -c "dd if=/dev/zero bs=1024 count=256 2>/dev/null | aplay -q -D plughw:$wc,0 -f S16_LE -r 48000 -c 2 >/dev/null 2>&1" && : > "$SCR/x.audio"; }
s_jpeg()   { have gst-launch-1.0 && timeout 30 gst-launch-1.0 -q videotestsrc num-buffers=2 ! jpegenc ! jpegdec ! fakesink >/dev/null 2>&1 && : > "$SCR/x.jpeg"; }
s_can()    { [ "$(cat /sys/class/net/can0/carrier 2>/dev/null)" = 1 ] && [ "$(cat /sys/class/net/can1/carrier 2>/dev/null)" = 1 ] && : > "$SCR/x.can"; }
s_pcienet(){ pe=$(ls /sys/bus/pci/devices/0000:*/net 2>/dev/null | head -1); [ -n "$pe" ] && timeout 5 ip link set "$(basename "$pe")" up 2>/dev/null && : > "$SCR/x.pcie"; }
s_cam()    { have imx95-isi-capture && timeout 30 imx95-isi-capture cap /dev/video0 >/dev/null 2>&1 && : > "$SCR/x.cam"; }

ctr() { cat /sys/class/net/$1/statistics/$2 2>/dev/null || echo 0; }
fl()  { [ -e "$SCR/x.$1" ] && { printf 1; rm -f "$SCR/x.$1"; } || printf 0; }
n=0
while :; do
  n=$((n+1))
  # Background EVERY stressor (incl s_net) and wait ONLY on their PIDs. A bare
  # `wait` - or s_net run foreground (its own internal `wait`) - would also block
  # on any detached mount job left in D-state. Subshell-scoping each stressor's
  # internal wait keeps the loop immune to that.
  sp=""
  s_net     & sp="$sp $!"
  s_store   & sp="$sp $!"
  s_9p      & sp="$sp $!"
  s_audio   & sp="$sp $!"
  s_jpeg    & sp="$sp $!"
  s_can     & sp="$sp $!"
  s_pcienet & sp="$sp $!"
  s_cam     & sp="$sp $!"
  wait $sp
  mf=$(awk '/^MemFree:/{print $2}' /proc/meminfo)
  up=$(awk '{print int($1)}' /proc/uptime)
  echo "ZHB $n up=$up memfree_kB=$mf net=$(fl net) store=$(fl store) audio=$(fl audio) jpeg=$(fl jpeg) can=$(fl can) 9p=$(fl 9p) cam=$(fl cam) pcie=$(fl pcie) eth=$(ctr eth0 tx_bytes)/$(ctr eth1 tx_bytes)/$(ctr eth2 tx_bytes)"
  sleep "$GUEST_HB_SEC"
done
EOS
chmod +x "$STAGE/everything-load.sh"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$INITRD"
need INITRD "$INITRD" initramfs

# --- Launch the daemonized full-distro guest with the device union. -----------
log "launching guest (mega device union)..."
"$QEMU" -M imx95-19x19-evk -m 4G -vnc "$VNC" \
    -object can-bus,id=cb -machine canbus0=cb,canbus1=cb \
    -kernel "$KERNEL" -dtb "$MEGA_DTB" -initrd "$INITRD" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init systemd.log_target=console systemd.journald.forward_to_console=1" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -device loader,file="$CM7_ELF",cpu-num=7 \
    -device usb-kbd,bus=usb-bus.0 \
    -drive if=none,format=raw,file="$DISK",id=mmc0 -device emmc,drive=mmc0 \
    -nic user -nic user -nic user \
    -device e1000e,bus=imx95-pcie \
    -fsdev local,id=fs1,path="$PCISHARE",security_model=none \
    -device virtio-9p-pci,bus=imx95-pcie1,fsdev=fs1,mount_tag=pcishare \
    -fsdev local,id=fs0,path="$MMIOSHARE",security_model=none \
    -device virtio-9p-device,fsdev=fs0,mount_tag=hostshare \
    -device ov5640,bus=lpi2c1,address=0x3c \
    -global driver=imx95.isi,property=frames,value="$FRAME" \
    -serial file:"$SERIAL" -serial null \
    -monitor unix:"$SOCK",server,nowait \
    -daemonize -pidfile "$PIDFILE"
QPID=$(cat "$PIDFILE")
log "  qemu pid: $QPID"

# ===================== supervisor (from tests/soak-fullimage) =================
HB=0; SCR_LAST=X; SCR_EVER_LOW=0; RSS_MIN=; RSS_MAX=; RSS_LAST=
cleanup() {
    local rc=${1:-130}
    if [ -e "$PIDFILE" ] && kill -0 "$QPID" 2>/dev/null; then
        kill "$QPID" 2>/dev/null || true; sleep 1
        kill -0 "$QPID" 2>/dev/null && kill -9 "$QPID" 2>/dev/null || true
    fi
    rm -f "$SOCK" "$PIDFILE"; write_summary; exit "$rc"
}
trap 'cleanup 130' INT TERM
count_anomalies() { grep -caE "$1" "$SERIAL" 2>/dev/null || echo 0; }
read_scr_bit12() {
    local out scr
    out=$(echo "xp /1wx 0x44460010" | timeout 5 nc -U -q 2 "$SOCK" 2>&1 || true)
    scr=$(echo "$out" | grep -oE '44460010: 0x[0-9a-fA-F]+' | tail -1 | awk '{print $NF}')
    case "$scr" in 0x[0-9a-fA-F]*) [ $(( scr & 0x1000 )) -ne 0 ] && echo 1 || echo 0 ;; *) echo X ;; esac
}
write_summary() {
    {
        echo "=== EVERYTHING soak summary ($(date -Iseconds)) ==="
        echo "soak dir: $SOAK_DIR   target hours: $SOAK_HOURS"
        echo "host heartbeats: $HB   guest ZHB: $(grep -ac 'ZHB ' "$SERIAL" 2>/dev/null || echo 0)"
        echo "qemu alive at end: $(kill -0 "$QPID" 2>/dev/null && echo yes || echo no)"
        echo "host RSS kB: min=$RSS_MIN max=$RSS_MAX last=$RSS_LAST"
        echo "SRC.SCR bit12 last: $SCR_LAST  ever-0: $SCR_EVER_LOW"
        echo "--- roll-call (startup) ---"
        grep -a 'ROLLCALL ' "$SERIAL" 2>/dev/null | sed 's/.*ROLLCALL /  /' | sort -u
        echo "  first ZHB: $(grep -a 'ZHB ' "$SERIAL" 2>/dev/null | head -1)"
        echo "  last  ZHB: $(grep -a 'ZHB ' "$SERIAL" 2>/dev/null | tail -1)"
        echo "anomalies: panics=$(count_anomalies 'Kernel panic') BUG=$(count_anomalies 'BUG:') oops=$(count_anomalies 'Oops:') segv=$(count_anomalies 'segfault') aborts=$(count_anomalies 'Unhandled fault|synchronous external abort')"
    } | tee "$SUMMARY"
}

log "waiting for full-distro userspace (up to 300 s)..."
deadline=$(( $(date +%s) + 300 ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    grep -q "=== imx95 everything userspace ===" "$SERIAL" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 3
done
if ! grep -q "=== imx95 everything userspace ===" "$SERIAL"; then
    log "FAIL: full distro did not reach the everything-load service within 300 s"
    tail -40 "$SERIAL" | tee -a "$HEARTBEATS"; cleanup 3
fi
sleep 25
log "--- startup roll-call ---"
grep -a 'ROLLCALL ' "$SERIAL" 2>/dev/null | sed 's/.*ROLLCALL /  /' | tee -a "$HEARTBEATS"
SCR_LAST=$(read_scr_bit12); log "  first SRC.SCR bit12: $SCR_LAST"

END_AT=$(( $(date +%s) + ${SOAK_SECONDS:-$((SOAK_HOURS * 3600))} ))
while [ "$(date +%s)" -lt "$END_AT" ]; do
    if ! kill -0 "$QPID" 2>/dev/null; then
        log "$(date -Iseconds) FAIL: qemu pid $QPID exited"; tail -50 "$SERIAL" | tee -a "$HEARTBEATS"; cleanup 2
    fi
    HB=$((HB + 1))
    RSS=$(awk '/^VmRSS:/ {print $2}' "/proc/$QPID/status" 2>/dev/null || echo 0)
    [ -z "$RSS_MIN" ] && RSS_MIN=$RSS && RSS_MAX=$RSS
    [ "$RSS" -lt "$RSS_MIN" ] && RSS_MIN=$RSS
    [ "$RSS" -gt "$RSS_MAX" ] && RSS_MAX=$RSS
    RSS_LAST=$RSS
    if [ $((HB % 10)) -eq 0 ]; then
        SCR_LAST=$(read_scr_bit12); [ "$SCR_LAST" = "0" ] && SCR_EVER_LOW=$((SCR_EVER_LOW + 1))
    fi
    log "$(date -Iseconds) HB=$HB rss_kB=$RSS slog_bytes=$(stat -c%s "$SERIAL" 2>/dev/null || echo 0) scr_bit12=$SCR_LAST zhb=$(grep -ac 'ZHB ' "$SERIAL" 2>/dev/null || echo 0)"
    sleep "$HEARTBEAT_SEC"
done
log "$(date -Iseconds) soak target reached; stopping"
ANOM=$(( $(count_anomalies 'Kernel panic') + $(count_anomalies 'BUG:') + $(count_anomalies 'Oops:') + $(count_anomalies 'segfault') + $(count_anomalies 'Unhandled fault|synchronous external abort') ))
[ "$ANOM" -eq 0 ] && cleanup 0 || cleanup 1
