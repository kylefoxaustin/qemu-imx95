#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Long-duration soak on the FULL NXP imx-image-full Linux distro (not busybox).
#
# This is tests/soak/run.sh's bigger sibling: instead of a ~1 MB busybox
# initramfs, it boots the real multi-GB imx-image-full .wic rootfs from eMMC and
# hands off to the image's own systemd - so the soak exercises the full distro
# (udev, the network stack, all the services and module autoloads) the way a
# real board does, then holds it under continuous NETC load for hours.
#
# Boot shape (reuses the proven tests/weston path): a tiny busybox initramfs
# mounts the .wic ext4 at /dev/mmcblk0p2, drops a one-shot soak-load systemd
# service into it, then switch_root's into /sbin/init. The service runs after
# multi-user.target: it brings the three ENETC ports up, drives continuous ICMP
# load, and emits an in-guest "ZHB" heartbeat to the console. The host side
# daemonizes QEMU and runs the same supervisor as tests/soak (RSS leak sampling,
# SRC.SCR bit12 stickiness, anomaly grep, SUMMARY).
#
# The booted kernel (Image from KBUILD) is the same 6.12.49 the .wic was built
# against, so the distro's =m audio/jpeg/etc. modules load against it.
#
# Required (override via env): QEMU, KBUILD (Image + dtb + dtc), SM_ELF,
# CM7_ELF, and WIC (the imx-image-full .wic or .wic.zst). SKIPs if WIC is unset.
set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
CM7_ELF=${CM7_ELF:-$REPO/tests/cm7-hello/cm7_image.elf}
KERNEL=${KERNEL:-$KBUILD/arch/arm64/boot/Image}
DTB=${DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
DTC=${DTC:-$KBUILD/scripts/dtc/dtc}
BUSYBOX_CPIO=${BUSYBOX_CPIO:-$REPO/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
WIC=${WIC:-}

SOAK_HOURS=${SOAK_HOURS:-24}
HEARTBEAT_SEC=${HEARTBEAT_SEC:-31}
GUEST_HB_SEC=${GUEST_HB_SEC:-60}
SOAK_DIR=${SOAK_DIR:-$REPO/build/soak-fullimage-$(date +%Y%m%d-%H%M%S)}
VNC=${VNC:-127.0.0.1:2,to=99}

skip() { echo "SKIP: $*"; exit 0; }
[ -n "$WIC" ] || skip "set WIC=<imx-image-full .wic[.zst]> to run the full-image soak"
need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
[ -e "$WIC" ] || skip "WIC not found: $WIC"
need QEMU "$QEMU" qemu; need SM_ELF "$SM_ELF" "SM firmware"
need CM7_ELF "$CM7_ELF" "M7 firmware"; need KERNEL "$KERNEL" "kernel Image"
need DTB "$DTB" dtb; need DTC "$DTC" dtc; need BBCPIO "$BUSYBOX_CPIO" busybox
command -v zstd >/dev/null || skip "zstd needed to expand the .wic"

mkdir -p "$SOAK_DIR"
SERIAL="$SOAK_DIR/serial.log"; HEARTBEATS="$SOAK_DIR/heartbeats.log"
SUMMARY="$SOAK_DIR/summary.txt"; PIDFILE="$SOAK_DIR/qemu.pid"
SOCK="$SOAK_DIR/qemu.mon"; DTB_PATCHED="$SOAK_DIR/netc.dtb"
INITRD="$SOAK_DIR/initrd.cpio.gz"; DISK="$SOAK_DIR/disk.wic"
log() { echo "$@" | tee -a "$HEARTBEATS"; }

log "=== full-IMAGE soak starting at $(date -Iseconds) ==="
log "  target hours: $SOAK_HOURS   output: $SOAK_DIR"

# --- Expand the .wic, grow to a power-of-2 (the eMMC card model requires it). --
log "expanding .wic (multi-GB; this takes a moment)..."
case "$WIC" in
    *.zst) zstd -d -f "$WIC" -o "$DISK" ;;
    *)     cp --reflink=auto "$WIC" "$DISK" ;;
esac
cur=$(stat -c%s "$DISK"); p2=1
while [ "$p2" -lt "$cur" ]; do p2=$((p2 * 2)); done
truncate -s "$p2" "$DISK"
log "  disk: $(stat -c%s "$DISK") bytes (eMMC-backed)"

# --- Patched DTB: all three NETC ports fixed-link, ENETC2 @10G (same as soak). -
"$DTC" -I dtb -O dts "$DTB" >"$SOAK_DIR/base.dts" 2>/dev/null
python3 "$REPO/tests/netc/patch-dtb.py" "$SOAK_DIR/base.dts" >"$SOAK_DIR/netc.dts"
"$DTC" -I dts -O dtb -o "$DTB_PATCHED" "$SOAK_DIR/netc.dts" 2>/dev/null
need DTBP "$DTB_PATCHED" "patched dtb"

# --- switch_root initramfs: mount the rootfs, inject the soak-load service, then
#     hand off to the image's systemd. --------------------------------------------
STAGE="$SOAK_DIR/root"; rm -rf "$STAGE"; mkdir -p "$STAGE"
zcat "$BUSYBOX_CPIO" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
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

# Drop the in-guest soak-load script + a systemd service that runs it after
# multi-user.target, writing its heartbeat to the console (journald forwards).
cat > /mnt/usr/bin/soak-load.sh <<EOS
#!/bin/sh
echo "=== imx95 fullimage userspace ==="
# Bring the three ENETC ports up (the distro net manager may also touch them).
i=0; while [ ! -e /sys/class/net/eth2 ] && [ \$i -lt 120 ]; do sleep 1; i=\$((i+1)); done
for e in eth0 eth1 eth2; do ip link set "\$e" up 2>/dev/null; done
sleep 5
echo "=== FULLIMAGE ==="
echo "-- os --"; (. /etc/os-release 2>/dev/null; echo "\$PRETTY_NAME")
echo "-- net --"; for n in eth0 eth1 eth2; do echo "\$n: speed=\$(cat /sys/class/net/\$n/speed 2>/dev/null) carrier=\$(cat /sys/class/net/\$n/carrier 2>/dev/null)"; done
echo "-- asound --"; cat /proc/asound/cards 2>/dev/null
echo "-- v4l --"; for v in /sys/class/video4linux/*/name; do echo "  \$(basename \$(dirname \$v))=\$(cat \$v)"; done
echo "-- services --"; systemctl is-system-running 2>/dev/null; systemctl --failed --no-legend 2>/dev/null | head
echo "=== FULLIMAGE-DONE ==="
ctr() { cat /sys/class/net/\$1/statistics/\$2 2>/dev/null || echo 0; }
carr() { cat /sys/class/net/\$1/carrier 2>/dev/null || echo 0; }
n=0
while :; do
  n=\$((n+1))
  for e in eth0 eth1 eth2; do ping -I "\$e" -c 3 -W 1 10.0.2.2 >/dev/null 2>&1 & done
  wait
  mf=\$(awk '/^MemFree:/{print \$2}' /proc/meminfo)
  up=\$(awk '{print int(\$1)}' /proc/uptime)
  echo "ZHB \$n up=\$up memfree_kB=\$mf eth0=\$(ctr eth0 rx_bytes)/\$(ctr eth0 tx_bytes) eth1=\$(ctr eth1 rx_bytes)/\$(ctr eth1 tx_bytes) eth2=\$(ctr eth2 rx_bytes)/\$(ctr eth2 tx_bytes) carrier=\$(carr eth0)\$(carr eth1)\$(carr eth2)"
  sleep GUEST_HB_PLACEHOLDER
done
EOS
sed -i "s/GUEST_HB_PLACEHOLDER/$GUEST_HB_SEC/" /mnt/usr/bin/soak-load.sh
chmod +x /mnt/usr/bin/soak-load.sh
cat > /mnt/etc/systemd/system/soak-load.service <<EOS
[Unit]
Description=imx95 soak load + heartbeat
After=multi-user.target
[Service]
Type=simple
ExecStart=/usr/bin/soak-load.sh
StandardOutput=journal+console
StandardError=journal+console
Restart=on-failure
[Install]
WantedBy=multi-user.target
EOS
mkdir -p /mnt/etc/systemd/system/multi-user.target.wants
ln -sf ../soak-load.service /mnt/etc/systemd/system/multi-user.target.wants/soak-load.service

# The soak boots the NETC dtb (no display panel/connector), so weston's DRM
# backend has no output and would restart-loop for the whole run. Mask it - this
# is a stability soak, not a display test (tests/weston covers the desktop on a
# panel-patched dtb). Keeps the distro from a failed-service churn.
ln -sf /dev/null /mnt/etc/systemd/system/weston.service
ln -sf /dev/null /mnt/etc/systemd/system/weston.socket
echo "SOAK-PREINIT: soak-load service injected; switching to systemd"
exec switch_root /mnt /sbin/init
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$INITRD"
need INITRD "$INITRD" initramfs

# --- Launch the daemonized full-distro guest. ---------------------------------
"$QEMU" -M imx95-19x19-evk -m 4G -vnc "$VNC" \
    -kernel "$KERNEL" -dtb "$DTB_PATCHED" -initrd "$INITRD" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init systemd.log_target=console systemd.journald.forward_to_console=1" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -device loader,file="$CM7_ELF",cpu-num=7 \
    -device usb-kbd,bus=usb-bus.0 \
    -drive if=none,format=raw,file="$DISK",id=mmc0 -device emmc,drive=mmc0 \
    -nic user -nic user -nic user \
    -serial file:"$SERIAL" -serial null \
    -monitor unix:"$SOCK",server,nowait \
    -daemonize -pidfile "$PIDFILE"
QPID=$(cat "$PIDFILE")
log "  qemu pid: $QPID"

HB=0; SCR_LAST=X; SCR_EVER_LOW=0; RSS_MIN=; RSS_MAX=; RSS_LAST=; SUBSYS_OK=1; SUBSYS_REPORT=""
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
    local zf zl
    # ZHB lines are systemd-console-prefixed (soak-load.sh[pid]:), so no anchor.
    zf=$(grep -a 'ZHB ' "$SERIAL" 2>/dev/null | head -1); zl=$(grep -a 'ZHB ' "$SERIAL" 2>/dev/null | tail -1)
    {
        echo "=== full-IMAGE soak summary ($(date -Iseconds)) ==="
        echo "soak dir: $SOAK_DIR   target hours: $SOAK_HOURS"
        echo "host heartbeats: $HB   guest ZHB: $(grep -ac 'ZHB ' "$SERIAL" 2>/dev/null || echo 0)"
        echo "qemu alive at end: $(kill -0 "$QPID" 2>/dev/null && echo yes || echo no)"
        echo "host RSS kB: min=$RSS_MIN max=$RSS_MAX last=$RSS_LAST"
        echo "SRC.SCR bit12 last: $SCR_LAST  ever-0: $SCR_EVER_LOW"
        echo "subsystem roll-call: $([ "$SUBSYS_OK" = 1 ] && echo 'all up' || echo 'see below')"
        echo "$SUBSYS_REPORT"
        echo "  first ZHB: ${zf:-<none>}"
        echo "  last  ZHB: ${zl:-<none>}"
        echo "anomalies: panics=$(count_anomalies 'Kernel panic') BUG=$(count_anomalies 'BUG:') oops=$(count_anomalies 'Oops:') segv=$(count_anomalies 'segfault') aborts=$(count_anomalies 'Unhandled fault|synchronous external abort')"
    } | tee "$SUMMARY"
}

log "waiting for full-distro userspace (up to 300 s)..."
deadline=$(( $(date +%s) + 300 ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    grep -q "=== imx95 fullimage userspace ===" "$SERIAL" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 3
done
if ! grep -q "=== imx95 fullimage userspace ===" "$SERIAL"; then
    log "FAIL: full distro did not reach the soak-load service within 300 s"
    log "--- serial tail ---"; tail -40 "$SERIAL" | tee -a "$HEARTBEATS"; cleanup 3
fi

sleep 20
# Roll-call: NETC ports up (read off the soak-load FULLIMAGE carrier lines, which
# the fixed-link distro reports as speed/carrier, not a "Link is Up" string),
# plus the full-distro subsystems that come up.
n1g=$(grep -acE 'eth[01]: speed=1000 carrier=1' "$SERIAL" 2>/dev/null || echo 0)
n10g=$(grep -acE 'eth2: speed=10000 carrier=1' "$SERIAL" 2>/dev/null || echo 0)
SUBSYS_REPORT="  $([ "${n10g:-0}" -ge 1 ] && [ "${n1g:-0}" -ge 2 ] && echo ok || { echo MISS; SUBSYS_OK=0; }) NETC 3 ports (1G=$n1g 10G=$n10g)"
chk() { if grep -qa "$2" "$SERIAL"; then SUBSYS_REPORT="$SUBSYS_REPORT
  ok   $1"; else SUBSYS_REPORT="$SUBSYS_REPORT
  MISS $1"; fi; }
chk "systemd userspace" 'imx95 fullimage userspace'
chk "Wave6 VPU nodes"   'Wave6 VPU'
chk "DPU drm bind"      'Initialized imx95-dpu'
log "--- subsystem roll-call ---"; echo "$SUBSYS_REPORT" | tee -a "$HEARTBEATS"

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
    log "$(date -Iseconds) HB=$HB rss_kB=$RSS slog_bytes=$(stat -c%s "$SERIAL" 2>/dev/null || echo 0) scr_bit12=$SCR_LAST"
    sleep "$HEARTBEAT_SEC"
done
log "$(date -Iseconds) soak target reached; stopping"
ANOM=$(( $(count_anomalies 'Kernel panic') + $(count_anomalies 'BUG:') + $(count_anomalies 'Oops:') + $(count_anomalies 'segfault') + $(count_anomalies 'Unhandled fault|synchronous external abort') ))
[ "$ANOM" -eq 0 ] && cleanup 0 || cleanup 1
