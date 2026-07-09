#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 NETC Timer / PTP clock (hw/net/imx95_netc_timer.c, ptp_netc driver).
#
# The NETC block exposes its IEEE 1588 clock as PCI function 1131:ee02 at
# devfn 0x18.0 on the NETC bus - ethernet@18,0 in the EVK dtb. Real silicon
# binds ptp_netc there and registers a PHC; we did not model it at all, so the
# guest had no NETC PTP clock.
#
# The assertion that matters is NOT that the PHC registers. ptp_netc's
# gettimex64() reads TMR_CUR_TIME (BAR0 + 0xf0/0xf4), not the counter it wrote,
# so a plain backing-store register file would register a PHC that returns a
# frozen timestamp - clock_gettime() succeeds and reports a constant, wrong
# time. That is a silent wrong answer. This test therefore reads TMR_CUR_TIME
# twice, seconds apart, and requires it to have ADVANCED.
#
# PASS = ptp_netc binds, a PHC named "NETC Timer PTP clock" appears, and
#        TMR_CUR_TIME advances between two reads.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=$KBUILD/arch/arm64/boot/Image
DTB=$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb
TMO=${TMO:-150}
need() { [ -e "$2" ] || { echo "missing $1: $2"; exit 1; }; }
need QEMU "$QEMU"; need IMAGE "$IMAGE"; need DTB "$DTB"; need SM_ELF "$SM_ELF"
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG=${LOG:-$WORK/serial.log}; root="$WORK/root"; mkdir -p "$root"
zcat "$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz" | (cd "$root" && cpio -idmu 2>/dev/null)
cat > "$root/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc; /bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
sleep 12
echo "=== PTP-TEST ==="
for p in /sys/class/ptp/ptp*; do
    [ -e "$p/clock_name" ] && echo "phc=$(basename $p) name=$(cat $p/clock_name)"
done
D=/sys/bus/pci/devices/0002:00:18.0
echo "driver=$(basename $(readlink $D/driver 2>/dev/null) 2>/dev/null)"
# BAR0 base from the PCI resource file. TMR_CUR_TIME is 64-bit at +0xf0/+0xf4.
# Read the full 64 bits: the low word alone wraps every ~4.3 s, so a 32-bit
# comparison across a multi-second sleep would fail most of the time. The low
# word must be read first - that read latches the high word.
BAR0=$(awk 'NR==1 {print $1}' $D/resource 2>/dev/null)
echo "bar0=$BAR0"
CUR_L=$(printf '0x%x\n' $(( BAR0 + 0xf0 )))
CUR_H=$(printf '0x%x\n' $(( BAR0 + 0xf4 )))
read_cur() {
    l=$(devmem $CUR_L 32 2>/dev/null); h=$(devmem $CUR_H 32 2>/dev/null)
    [ -n "$l" ] && [ -n "$h" ] || return 1
    echo $(( $(( h )) * 4294967296 + $(( l )) ))
}
T1=$(read_cur) || T1=""
sleep 3
T2=$(read_cur) || T2=""
echo "cur_time_1=$T1"
echo "cur_time_2=$T2"
if [ -n "$T1" ] && [ -n "$T2" ] && [ "$T2" -gt "$T1" ]; then
    echo "clock_advanced=yes delta_ns=$(( T2 - T1 ))"
else
    echo "clock_advanced=no"
fi
echo "=== PTP-TEST-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$root/init"
( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"
timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "=== ptp report ==="; sed -n '/PTP-TEST ===/,/PTP-TEST-DONE/p' "$LOG" | grep -vE 'PTP-TEST ==='
pass=1
grep -qa 'driver=ptp_netc' "$LOG" && echo "  ok   ptp_netc bound to 0002:00:18.0" \
    || { echo "  MISS ptp_netc did not bind"; pass=0; }
grep -qa 'name=NETC Timer PTP clock' "$LOG" && echo "  ok   PHC registered" \
    || { echo "  MISS PHC not registered"; pass=0; }
grep -qa 'clock_advanced=yes' "$LOG" && echo "  ok   TMR_CUR_TIME advances (not a frozen clock)" \
    || { echo "  MISS clock frozen or unreadable - a PHC that lies is worse than none"; pass=0; }
for a in 'Kernel panic' 'Oops' 'external abort' 'Unhandled fault'; do
    n=$(grep -ac "$a" "$LOG" 2>/dev/null || true)
    [ "${n:-0}" = 0 ] || { echo "  MISS anomaly: $a x$n"; pass=0; }
done
[ "$pass" = 1 ] && { echo "PASS: NETC Timer binds and its PHC keeps time"; exit 0; }
echo "FAIL"; exit 1
