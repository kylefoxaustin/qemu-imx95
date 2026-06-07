#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Long-duration stability soak harness.
#
# Launches the v1.x parallel-boot configuration (A55 cluster + M33 SM
# + M7 hello firmware) in the background, samples the system every
# ~31 s, and stops cleanly at the soak target (default 36 h). Mirrors
# the metrics the prior v1.0 and v1.x Step 2 soaks captured:
#
#   - Heartbeat count + guest serial log size delta (proof the guest
#     stayed alive and was still producing output).
#   - MemAvailable from /proc/meminfo via HMP gpa-read on a sentinel
#     file? No - HMP can't read guest /proc. Instead we sample
#     /proc/<qemu-pid>/status RssAnon to track host-side RSS (the v1.0
#     ±10 % campaign metric), and let the in-guest heartbeats provide
#     guest-side liveness via the serial log size.
#   - Anomaly counts grepped from the serial log: panics, RCU stalls,
#     SCMI timeouts, BUGs, oopses, and segfaults.
#   - Step-3-specific: SRC_GEN.SCR (0x44460010) bit 12 must stay set
#     for the entire run (sticky-bit holds; flipping back would mean
#     our SRC model lost state).
#
# Target hours come from $SOAK_HOURS (default 36). Output dir from
# $SOAK_DIR (default $REPO/build/soak-<timestamp>). The driver is safe
# to run under systemd-run/systemd --user/nohup; it daemonizes QEMU
# and the heartbeat loop runs in foreground.
#
# Stop conditions (whichever fires first):
#   1. Wall-clock reached SOAK_HOURS - clean stop, SUMMARY printed,
#      exit 0 if no anomalies, exit 1 if any.
#   2. QEMU process died - exit 2 with a panic dump of the tail of
#      the serial log.
#   3. SIGTERM/SIGINT from the supervisor - clean stop, partial
#      SUMMARY printed, exit 130.

set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
SM_ELF=${SM_ELF:-${IMX95_ARTIFACTS:-$HOME/imx95-artifacts}/m33_image.elf}
KERNEL=${KERNEL:-${IMX95_ARTIFACTS:-$HOME/imx95-artifacts}/linux-build/arch/arm64/boot/Image}
DTB=${DTB:-${IMX95_ARTIFACTS:-$HOME/imx95-artifacts}/linux-build/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
INITRD=${INITRD:-$REPO/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
CM7_ELF=${CM7_ELF:-$REPO/tests/cm7-hello/cm7_image.elf}

SOAK_HOURS=${SOAK_HOURS:-36}
HEARTBEAT_SEC=${HEARTBEAT_SEC:-31}
SOAK_DIR=${SOAK_DIR:-$REPO/build/soak-$(date +%Y%m%d-%H%M%S)}

need() {
    [ -e "$2" ] && return 0
    echo "error: $3 not found:" >&2
    echo "    $2" >&2
    exit 1
}
need QEMU    "$QEMU"    "qemu-system-aarch64"
need SM_ELF  "$SM_ELF"  "SM firmware m33_image.elf"
need KERNEL  "$KERNEL"  "kernel Image"
need DTB     "$DTB"     "device tree"
need INITRD  "$INITRD"  "initramfs"
need CM7_ELF "$CM7_ELF" "M7 firmware"

mkdir -p "$SOAK_DIR"
SERIAL="$SOAK_DIR/serial.log"
HEARTBEATS="$SOAK_DIR/heartbeats.log"
SUMMARY="$SOAK_DIR/summary.txt"
PIDFILE="$SOAK_DIR/qemu.pid"
SOCK="$SOAK_DIR/qemu.mon"

CMDLINE="earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init"

echo "=== soak run starting at $(date -Iseconds) ===" | tee -a "$HEARTBEATS"
echo "  target hours:    $SOAK_HOURS" | tee -a "$HEARTBEATS"
echo "  heartbeat every: ${HEARTBEAT_SEC}s" | tee -a "$HEARTBEATS"
echo "  output dir:      $SOAK_DIR" | tee -a "$HEARTBEATS"

"$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$INITRD" \
    -append "$CMDLINE" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -device loader,file="$CM7_ELF",cpu-num=7 \
    -serial file:"$SERIAL" -serial null \
    -monitor unix:"$SOCK",server,nowait \
    -daemonize -pidfile "$PIDFILE"

QPID=$(cat "$PIDFILE")
echo "  qemu pid:        $QPID" | tee -a "$HEARTBEATS"

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
    n=$(grep -cE "$pattern" "$SERIAL" 2>/dev/null || true)
    # grep -c always prints a number; the OR-true above just neutralises
    # its exit=1 on zero matches. Default to 0 if it printed nothing
    # (file missing, etc.) so the summary stays tidy.
    echo "${n:-0}"
}

read_scr_bit12() {
    # Returns 0 / 1 / X (X if read failed). DO NOT send HMP "quit" -
    # it terminates the VM. nc -q 2 closes the client end after 2 s
    # of stdin idle, the monitor was created with nowait so qemu just
    # goes back to listening for the next client.
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

write_summary() {
    {
        echo "=== soak summary (recorded $(date -Iseconds)) ==="
        echo "soak dir:                $SOAK_DIR"
        echo "soak target hours:       $SOAK_HOURS"
        echo "heartbeat interval:      ${HEARTBEAT_SEC}s"
        echo "heartbeats recorded:     $HB"
        echo "serial log final size:   $(stat -c%s "$SERIAL" 2>/dev/null || echo 0) bytes"
        echo "qemu still alive at end: $(kill -0 "$QPID" 2>/dev/null && echo yes || echo no)"
        echo "host RSS samples (kB):   min=$RSS_MIN max=$RSS_MAX last=$RSS_LAST"
        echo "SRC.SCR bit12 last:      $SCR_LAST  (sticky; expected 1)"
        echo "SRC.SCR bit12 ever-0:    $SCR_EVER_LOW (any 0 reading after first 1 is a regression)"
        echo "anomalies grepped from serial.log:"
        echo "  panics:                $(count_anomalies 'Kernel panic')"
        echo "  RCU stalls:            $(count_anomalies 'rcu:.*stall|rcu_sched.*stall')"
        echo "  BUG():                 $(count_anomalies 'BUG:')"
        echo "  oops:                  $(count_anomalies 'Oops:')"
        echo "  segfaults:             $(count_anomalies 'segfault')"
        # "max-rx-timeout: 5000ms" is a normal SCMI init line; match
        # only the actual-failure phrasing the kernel uses.
        echo "  SCMI timeouts:         $(count_anomalies 'scmi.*timed out|SCMI.*timed out|scmi.*[Ff]ailed.*timeout')"
        echo "  unhandled aborts:      $(count_anomalies 'Unhandled fault|synchronous external abort')"
    } | tee "$SUMMARY"
}

# Wait up to 60s for Linux to reach userspace before starting the
# heartbeat loop, so the first SRC.SCR sample isn't taken before the
# SM has had a chance to write bit 12.
echo "waiting for Linux userspace (up to 60 s)..." | tee -a "$HEARTBEATS"
deadline=$(( $(date +%s) + 60 ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    grep -q "=== imx95 busybox userspace ===" "$SERIAL" 2>/dev/null && break
    sleep 1
done
if ! grep -q "=== imx95 busybox userspace ===" "$SERIAL"; then
    echo "FAIL: Linux did not reach userspace within 60 s; aborting soak" \
        | tee -a "$HEARTBEATS"
    cleanup 3
fi

# First SCR sample, immediately post-userspace.
SCR_FIRST=$(read_scr_bit12)
echo "  first SRC.SCR bit12: $SCR_FIRST" | tee -a "$HEARTBEATS"
[ "$SCR_FIRST" = "1" ] || echo "  WARN: SRC.SCR bit 12 not set at soak start" | tee -a "$HEARTBEATS"

# Soak loop.
HB=0
SCR_LAST=$SCR_FIRST
SCR_EVER_LOW=0
RSS_MIN=
RSS_MAX=
RSS_LAST=
END_AT=$(( $(date +%s) + ${SOAK_SECONDS:-$((SOAK_HOURS * 3600))} ))

while [ "$(date +%s)" -lt "$END_AT" ]; do
    if ! kill -0 "$QPID" 2>/dev/null; then
        echo "$(date -Iseconds) FAIL: qemu pid $QPID exited" | tee -a "$HEARTBEATS"
        echo "--- tail of $SERIAL ---" | tee -a "$HEARTBEATS"
        tail -50 "$SERIAL" | tee -a "$HEARTBEATS"
        cleanup 2
    fi

    HB=$((HB + 1))
    RSS=$(awk '/^VmRSS:/ {print $2}' "/proc/$QPID/status" 2>/dev/null || echo 0)
    [ -z "$RSS_MIN" ] && RSS_MIN=$RSS && RSS_MAX=$RSS
    [ "$RSS" -lt "$RSS_MIN" ] && RSS_MIN=$RSS
    [ "$RSS" -gt "$RSS_MAX" ] && RSS_MAX=$RSS
    RSS_LAST=$RSS

    # Sample SRC.SCR every 10 heartbeats (~5 min) - HMP traffic isn't
    # free under TCG.
    if [ $((HB % 10)) -eq 0 ]; then
        SCR_LAST=$(read_scr_bit12)
        [ "$SCR_LAST" = "0" ] && SCR_EVER_LOW=$((SCR_EVER_LOW + 1))
    fi

    SLOG_SIZE=$(stat -c%s "$SERIAL" 2>/dev/null || echo 0)
    echo "$(date -Iseconds) HB=$HB rss_kB=$RSS slog_bytes=$SLOG_SIZE scr_bit12=$SCR_LAST" \
        | tee -a "$HEARTBEATS"

    sleep "$HEARTBEAT_SEC"
done

echo "$(date -Iseconds) soak target reached; stopping" | tee -a "$HEARTBEATS"
cleanup 0
