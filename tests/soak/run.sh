#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Long-duration FULL-DEVICE-SET stability soak harness.
#
# Boots the whole modelled i.MX 95 at once - every subsystem that the
# full-stack coexistence gate (tests/full-stack/run.sh) brings up, but kept
# alive for hours under continuous NETC load instead of a one-shot boot:
#
#   - A55 cluster + the real System Manager (M33, m33_image.elf) serving SCMI
#   - M7 (cm7_image.elf) - soaks the SM<->M7 lifecycle + SRC.SCR bit12 path
#   - NETC: all three ENETC PFs (eth0/eth1 @1G, eth2 @10G), links held up and
#     driven with continuous ICMP load so the datapath is exercised, not idle
#   - Audio: the three ASoC cards (bt-sco, wm8962, micfil)
#   - JPEG: both HW codecs (/dev/video2 dec, /dev/video3 enc)
#   - USB: usb-kbd enumerates as a HID input on the ChipIdea host
#   - DPU: the display controller binds (headless VNC gives it a surface)
#
# WHAT IS AND ISN'T SOAKED UNDER LOAD: the soak initramfs is busybox, which has
# no GStreamer/aplay/v4l userspace (and there is no QEMU audio backend), so
# audio/JPEG/DPU are soaked for BINDING + COEXISTENCE stability - they come up
# and must stay fault-free for the whole run, but their datapaths are not driven
# here (that is what tests/gstreamer + tests/full-stack cover at boot). The one
# datapath kept under continuous load is NETC, which busybox CAN drive.
#
# Metrics (mirrors the prior v1.x / v2.x soak campaigns):
#   - Host-side: VmRSS of the qemu process (leak signal), sampled every ~31 s.
#   - Guest-side: an in-guest ZHB heartbeat (every HB) carrying uptime, MemFree,
#     per-port rx/tx byte counters and carrier state - proves the guest stayed
#     alive, the NETC datapath kept moving, and the links never dropped.
#   - SRC_GEN.SCR (0x44460010) bit 12 must stay set the whole run (sticky; a
#     flip back to 0 means the SRC model lost state - an M7-lifecycle regression).
#   - Subsystem roll-call taken once at startup (all the full-stack markers):
#     a soak that silently lost a subsystem at boot isn't soaking it.
#   - Anomaly counts grepped from the serial log: panics, RCU stalls, SCMI
#     timeouts, BUGs, oopses, segfaults, external aborts, hw csum failures.
#
# Target hours come from $SOAK_HOURS (default 36). Output dir from $SOAK_DIR
# (default $REPO/build/soak-<timestamp>). Safe under systemd-run/nohup; it
# daemonizes QEMU and the heartbeat loop runs in foreground.
#
# Stop conditions (whichever fires first):
#   1. Wall-clock reached SOAK_HOURS - clean stop, SUMMARY printed,
#      exit 0 if no anomalies, exit 1 if any.
#   2. QEMU process died - exit 2 with a panic dump of the serial-log tail.
#   3. SIGTERM/SIGINT from the supervisor - clean stop, partial SUMMARY,
#      exit 130.
#
# Required artifacts (override via env): QEMU, KBUILD (Image + dtb + dtc),
# SM_ELF, CM7_ELF, MODROOT (kernel build tree with the =m audio/jpeg .ko's,
# whose vermagic MUST match the booted Image). Same layout as
# tests/full-stack/run.sh and tests/netc/run-10g.sh.

set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
CM7_ELF=${CM7_ELF:-$REPO/tests/cm7-hello/cm7_image.elf}
KERNEL=${KERNEL:-$KBUILD/arch/arm64/boot/Image}
DTB=${DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
DTC=${DTC:-$KBUILD/scripts/dtc/dtc}
# The =m audio + jpeg modules are staged from the booted kernel's OWN build tree
# (vermagic must match the Image) - NOT a BSP rootfs whose modules mismatch.
MODROOT=${MODROOT:-$KBUILD}
BUSYBOX_CPIO=${BUSYBOX_CPIO:-$REPO/tests/busybox-initramfs/busybox-initramfs.cpio.gz}

SOAK_HOURS=${SOAK_HOURS:-36}
HEARTBEAT_SEC=${HEARTBEAT_SEC:-31}
GUEST_HB_SEC=${GUEST_HB_SEC:-60}
SOAK_DIR=${SOAK_DIR:-$REPO/build/soak-$(date +%Y%m%d-%H%M%S)}
VNC=${VNC:-127.0.0.1:0,to=99}

need() {
    [ -e "$2" ] && return 0
    echo "error: $3 not found:" >&2
    echo "    $2" >&2
    exit 1
}
need QEMU    "$QEMU"    "qemu-system-aarch64"
need SM_ELF  "$SM_ELF"  "SM firmware m33_image.elf"
need CM7_ELF "$CM7_ELF" "M7 firmware cm7_image.elf"
need KERNEL  "$KERNEL"  "kernel Image"
need DTB     "$DTB"     "device tree"
need DTC     "$DTC"     "dtc (kernel scripts/dtc/dtc)"
need BBCPIO  "$BUSYBOX_CPIO" "busybox initramfs"

mkdir -p "$SOAK_DIR"
SERIAL="$SOAK_DIR/serial.log"
HEARTBEATS="$SOAK_DIR/heartbeats.log"
SUMMARY="$SOAK_DIR/summary.txt"
PIDFILE="$SOAK_DIR/qemu.pid"
SOCK="$SOAK_DIR/qemu.mon"
DTB_PATCHED="$SOAK_DIR/netc.dtb"
INITRD="$SOAK_DIR/initrd.cpio.gz"

log() { echo "$@" | tee -a "$HEARTBEATS"; }

log "=== full-device-set soak starting at $(date -Iseconds) ==="
log "  target hours:     $SOAK_HOURS"
log "  host heartbeat:   ${HEARTBEAT_SEC}s"
log "  guest heartbeat:  ${GUEST_HB_SEC}s"
log "  output dir:       $SOAK_DIR"

# --- Build the patched DTB: all three NETC ports fixed-link, ENETC2 @10G. -----
"$DTC" -I dtb -O dts "$DTB" >"$SOAK_DIR/base.dts" 2>/dev/null
python3 "$REPO/tests/netc/patch-dtb.py" "$SOAK_DIR/base.dts" >"$SOAK_DIR/netc.dts"
"$DTC" -I dts -O dtb -o "$DTB_PATCHED" "$SOAK_DIR/netc.dts" 2>/dev/null
need DTBP "$DTB_PATCHED" "patched device tree"

# --- Build the soak initramfs: busybox + staged audio/jpeg modules + /init. ----
ROOTFS="$SOAK_DIR/root"; rm -rf "$ROOTFS"; mkdir -p "$ROOTFS"
zcat "$BUSYBOX_CPIO" | (cd "$ROOTFS" && cpio -idmu 2>/dev/null)

# Loadable audio + jpeg modules (load order preserved). NETC/USB/DPU are
# built-in and need none. The init insmods this exact list in sequence.
MODS="snd-soc-fsl-utils imx-pcm-dma snd-soc-fsl-sai snd-soc-fsl-micfil \
      snd-soc-wm8962 snd-soc-dmic snd-soc-imx-audmux snd-soc-fsl-asoc-card \
      snd-soc-imx-card v4l2-jpeg mxc-jpeg-encdec"
mkdir -p "$ROOTFS/mods"
for m in $MODS; do
    f=$(find "$MODROOT" -name "$m.ko" | head -1)
    need "$m.ko under MODROOT" "$f"
    cp "$f" "$ROOTFS/mods/"
done

{
    echo '#!/bin/busybox sh'
    echo "MODS=\"$MODS\""
    echo "GUEST_HB_SEC=$GUEST_HB_SEC"
} > "$ROOTFS/init"
cat >> "$ROOTFS/init" <<'INIT'
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1

# Load the staged audio + jpeg modules (NETC/USB/DPU are built-in).
for m in $MODS; do insmod /mods/$m.ko 2>&1; done

# Announce userspace so the host supervisor starts sampling (printed before the
# ready-wait so a slow subsystem doesn't look like a failed boot).
echo "=== imx95 fullstack userspace ==="

# NETC: wait for the last port to enumerate, then admin-up all three.
i=0; while [ ! -e /sys/class/net/eth2 ] && [ $i -lt 120 ]; do usleep 500000; i=$((i+1)); done
for e in eth0 eth1 eth2; do ip link set "$e" up 2>/dev/null; done

# Wait until the asserted subsystems settle (full device set boots slower than
# the single-subsystem tests) before the one-time roll-call snapshot.
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

# rx/tx byte counters for a port (0 if absent).
ctr() { cat /sys/class/net/$1/statistics/$2 2>/dev/null || echo 0; }
carr() { cat /sys/class/net/$1/carrier 2>/dev/null || echo 0; }

# Soak loop: drive continuous ICMP load on every port and emit a heartbeat.
n=0
while :; do
    n=$((n+1))
    # A short burst per port keeps the datapath moving without saturating TCG.
    for e in eth0 eth1 eth2; do
        ping -I "$e" -c 3 -W 1 10.0.2.2 >/dev/null 2>&1 &
    done
    wait
    mf=$(awk '/^MemFree:/{print $2}' /proc/meminfo)
    up=$(awk '{print int($1)}' /proc/uptime)
    echo "ZHB $n up=$up memfree_kB=$mf" \
         "eth0=$(ctr eth0 rx_bytes)/$(ctr eth0 tx_bytes)" \
         "eth1=$(ctr eth1 rx_bytes)/$(ctr eth1 tx_bytes)" \
         "eth2=$(ctr eth2 rx_bytes)/$(ctr eth2 tx_bytes)" \
         "carrier=$(carr eth0)$(carr eth1)$(carr eth2)"
    sleep "$GUEST_HB_SEC"
done
INIT
chmod +x "$ROOTFS/init"
( cd "$ROOTFS" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$INITRD"
need INITRD "$INITRD" "built initramfs"

# --- Launch the daemonized full-device-set guest. -----------------------------
"$QEMU" -M imx95-19x19-evk -m 2G -vnc "$VNC" \
    -kernel "$KERNEL" -dtb "$DTB_PATCHED" -initrd "$INITRD" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -device loader,file="$CM7_ELF",cpu-num=7 \
    -device usb-kbd \
    -nic user -nic user -nic user \
    -serial file:"$SERIAL" -serial null \
    -monitor unix:"$SOCK",server,nowait \
    -daemonize -pidfile "$PIDFILE"

QPID=$(cat "$PIDFILE")
log "  qemu pid:         $QPID"

# Pre-init every global the summary reads, BEFORE arming the trap, so a signal
# arriving during the userspace wait still writes a coherent summary under -u.
HB=0; SCR_LAST=X; SCR_EVER_LOW=0; RSS_MIN=; RSS_MAX=; RSS_LAST=

cleanup() {
    local rc=${1:-130}
    if [ -e "$PIDFILE" ] && kill -0 "$QPID" 2>/dev/null; then
        kill "$QPID" 2>/dev/null || true
        sleep 1
        kill -0 "$QPID" 2>/dev/null && kill -9 "$QPID" 2>/dev/null || true
    fi
    rm -f "$SOCK" "$PIDFILE"
    write_summary
    exit "$rc"
}
trap 'cleanup 130' INT TERM

count_anomalies() {
    local pattern=$1 n
    n=$(grep -caE "$pattern" "$SERIAL" 2>/dev/null || true)
    echo "${n:-0}"
}

read_scr_bit12() {
    # Returns 0 / 1 / X. DO NOT send HMP "quit" - it kills the VM. nc -q 2
    # closes the client after 2 s idle; the nowait monitor keeps listening.
    local out scr
    out=$( echo "xp /1wx 0x44460010" \
           | timeout 5 nc -U -q 2 "$SOCK" 2>&1 || true)
    scr=$(echo "$out" | grep -oE '44460010: 0x[0-9a-fA-F]+' | tail -1 | awk '{print $NF}')
    case "$scr" in
        0x[0-9a-fA-F]*)
            if [ $(( scr & 0x1000 )) -ne 0 ]; then echo 1; else echo 0; fi
            ;;
        *) echo X ;;
    esac
}

# One-time subsystem roll-call: every full-stack marker must be present, else
# the soak isn't actually soaking that subsystem. Records into SUBSYS_REPORT.
SUBSYS_REPORT=""
SUBSYS_OK=1
assert_subsystems() {
    local n1g n10g
    n1g=$(grep -acE 'fsl_enetc4 .* Link is Up - 1Gbps' "$SERIAL" 2>/dev/null || true)
    n10g=$(grep -acE 'fsl_enetc4 .* Link is Up - 10Gbps' "$SERIAL" 2>/dev/null || true)
    local mark
    if [ "${n10g:-0}" -ge 1 ] && [ "${n1g:-0}" -ge 2 ]; then
        mark="  ok   NETC 3 ports (1Gbps=$n1g 10Gbps=$n10g)"
    else
        mark="  MISS NETC 3 ports (1Gbps=$n1g 10Gbps=$n10g)"; SUBSYS_OK=0
    fi
    SUBSYS_REPORT="$mark"
    chk() {
        if grep -qa "$2" "$SERIAL"; then
            SUBSYS_REPORT="$SUBSYS_REPORT
  ok   $1"
        else
            SUBSYS_REPORT="$SUBSYS_REPORT
  MISS $1"; SUBSYS_OK=0
        fi
    }
    chk "audio bt-sco"  'btscoaudio'
    chk "audio wm8962"  'wm8962audio'
    chk "audio micfil"  'micfilaudio'
    chk "jpeg decoder"  '4c500000.jpegdec: decoder device registered'
    chk "jpeg encoder"  '4c550000.jpegenc: encoder device registered'
    chk "usb-kbd HID"   'QEMU USB Keyboard'
    chk "DPU drm bind"  'Initialized imx95-dpu'
    log "--- subsystem roll-call ---"
    echo "$SUBSYS_REPORT" | tee -a "$HEARTBEATS"
}

# Pull the first/last in-guest ZHB heartbeat field to prove the datapath kept
# moving and memory stayed flat. $1=awk field expr on a "ZHB" line.
zhb_first() { grep '^ZHB ' "$SERIAL" 2>/dev/null | head -1; }
zhb_last()  { grep '^ZHB ' "$SERIAL" 2>/dev/null | tail -1; }
zhb_field() { echo "$1" | grep -oE "$2=[0-9]+" | head -1 | cut -d= -f2; }
zhb_tx()    { echo "$1" | grep -oE "$2=[0-9]+/[0-9]+" | head -1 | cut -d/ -f2; }

write_summary() {
    local zf zl
    zf=$(zhb_first); zl=$(zhb_last)
    {
        echo "=== full-device-set soak summary (recorded $(date -Iseconds)) ==="
        echo "soak dir:                $SOAK_DIR"
        echo "soak target hours:       $SOAK_HOURS"
        echo "host heartbeat interval: ${HEARTBEAT_SEC}s"
        echo "host heartbeats:         $HB"
        echo "guest heartbeats (ZHB):  $(grep -c '^ZHB ' "$SERIAL" 2>/dev/null || echo 0)"
        echo "serial log final size:   $(stat -c%s "$SERIAL" 2>/dev/null || echo 0) bytes"
        echo "qemu still alive at end: $(kill -0 "$QPID" 2>/dev/null && echo yes || echo no)"
        echo "host RSS samples (kB):   min=$RSS_MIN max=$RSS_MAX last=$RSS_LAST"
        echo "SRC.SCR bit12 last:      $SCR_LAST  (sticky; expected 1)"
        echo "SRC.SCR bit12 ever-0:    $SCR_EVER_LOW (any 0 after first 1 is a regression)"
        echo "subsystem roll-call:     $([ "$SUBSYS_OK" = 1 ] && echo "all up" || echo "MISSING (see below)")"
        echo "$SUBSYS_REPORT"
        echo "guest liveness / NETC datapath:"
        echo "  first ZHB: ${zf:-<none captured>}"
        echo "  last  ZHB: ${zl:-<none captured>}"
        if [ -n "$zf" ] && [ -n "$zl" ]; then
            echo "  MemFree kB:  first=$(zhb_field "$zf" memfree_kB) last=$(zhb_field "$zl" memfree_kB)"
            echo "  eth0 tx grew: $(zhb_tx "$zf" eth0) -> $(zhb_tx "$zl" eth0)"
            echo "  eth1 tx grew: $(zhb_tx "$zf" eth1) -> $(zhb_tx "$zl" eth1)"
            echo "  eth2 tx grew: $(zhb_tx "$zf" eth2) -> $(zhb_tx "$zl" eth2)"
        fi
        echo "  carrier-not-111 ZHBs:  $(grep '^ZHB ' "$SERIAL" 2>/dev/null | grep -cvE 'carrier=111' || true)"
        echo "anomalies grepped from serial.log:"
        echo "  panics:                $(count_anomalies 'Kernel panic')"
        echo "  RCU stalls:            $(count_anomalies 'rcu:.*stall|rcu_sched.*stall')"
        echo "  BUG():                 $(count_anomalies 'BUG:')"
        echo "  oops:                  $(count_anomalies 'Oops:')"
        echo "  segfaults:             $(count_anomalies 'segfault')"
        echo "  SCMI timeouts:         $(count_anomalies 'scmi.*timed out|SCMI.*timed out|scmi.*[Ff]ailed.*timeout')"
        echo "  unhandled aborts:      $(count_anomalies 'Unhandled fault|synchronous external abort')"
        echo "  hw csum failures:      $(count_anomalies 'hw csum failure')"
    } | tee "$SUMMARY"
}

# Wait for Linux userspace before sampling (full device set boots slower than
# the old minimal soak, so allow up to 180 s).
log "waiting for Linux userspace (up to 180 s)..."
deadline=$(( $(date +%s) + 180 ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    grep -q "=== imx95 fullstack userspace ===" "$SERIAL" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 2
done

if ! grep -q "=== imx95 fullstack userspace ===" "$SERIAL"; then
    log "FAIL: Linux did not reach userspace within 180 s; aborting soak"
    log "--- tail of serial.log ---"; tail -40 "$SERIAL" | tee -a "$HEARTBEATS"
    cleanup 3
fi

# Let the subsystem markers settle (10G link + audio cards + jpeg nodes), then
# take the one-time roll-call.
sleep 30
assert_subsystems
[ "$SUBSYS_OK" = 1 ] || log "  WARN: not every subsystem came up; soak continues but coverage is reduced"

SCR_FIRST=$(read_scr_bit12)
SCR_LAST=$SCR_FIRST
log "  first SRC.SCR bit12: $SCR_FIRST"
[ "$SCR_FIRST" = "1" ] || log "  WARN: SRC.SCR bit 12 not set at soak start"

END_AT=$(( $(date +%s) + ${SOAK_SECONDS:-$((SOAK_HOURS * 3600))} ))
while [ "$(date +%s)" -lt "$END_AT" ]; do
    if ! kill -0 "$QPID" 2>/dev/null; then
        log "$(date -Iseconds) FAIL: qemu pid $QPID exited"
        log "--- tail of serial.log ---"; tail -50 "$SERIAL" | tee -a "$HEARTBEATS"
        cleanup 2
    fi

    HB=$((HB + 1))
    RSS=$(awk '/^VmRSS:/ {print $2}' "/proc/$QPID/status" 2>/dev/null || echo 0)
    [ -z "$RSS_MIN" ] && RSS_MIN=$RSS && RSS_MAX=$RSS
    [ "$RSS" -lt "$RSS_MIN" ] && RSS_MIN=$RSS
    [ "$RSS" -gt "$RSS_MAX" ] && RSS_MAX=$RSS
    RSS_LAST=$RSS

    # Sample SRC.SCR every 10 heartbeats (~5 min) - HMP traffic isn't free
    # under TCG.
    if [ $((HB % 10)) -eq 0 ]; then
        SCR_LAST=$(read_scr_bit12)
        [ "$SCR_LAST" = "0" ] && SCR_EVER_LOW=$((SCR_EVER_LOW + 1))
    fi

    SLOG_SIZE=$(stat -c%s "$SERIAL" 2>/dev/null || echo 0)
    log "$(date -Iseconds) HB=$HB rss_kB=$RSS slog_bytes=$SLOG_SIZE scr_bit12=$SCR_LAST"

    sleep "$HEARTBEAT_SEC"
done

log "$(date -Iseconds) soak target reached; stopping"
# Exit 1 if any anomaly was logged, else 0 (subsystem misses are a WARN, not a
# hard fail - a started soak with reduced coverage still produced useful data).
ANOM=$(( $(count_anomalies 'Kernel panic') + $(count_anomalies 'BUG:') \
       + $(count_anomalies 'Oops:') + $(count_anomalies 'segfault') \
       + $(count_anomalies 'Unhandled fault|synchronous external abort') \
       + $(count_anomalies 'hw csum failure') ))
[ "$ANOM" -eq 0 ] && cleanup 0 || cleanup 1
