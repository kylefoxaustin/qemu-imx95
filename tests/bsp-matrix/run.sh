#!/usr/bin/env bash
# i.MX95 BSP-matrix boot-smoke: boot one validated-BSP entry to userspace and
# assert the two kernel-side markers (SCMI handshake served by the real SM +
# busybox userspace roll-call). Build each BSP image ONCE; this is the cheap
# re-runnable regression. Entry artifacts come from env (defaults = the on-box
# pinned-walnascar-6.12.49 set), so CI/holobench can point it at any entry dir.
#
#   tests/bsp-matrix/run.sh [label]
#   KERNEL=… DTB=… INITRD=… SM_FW=… [QEMU=…] tests/bsp-matrix/run.sh latest-nxp-whinlatter
set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
LABEL=${1:-pinned-walnascar-6.12.49}
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
KERNEL=${KERNEL:-$HOME/Documents/linux-imx95-build/arch/arm64/boot/Image}
DTB=${DTB:-$HOME/Documents/linux-imx95-build/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
INITRD=${INITRD:-$REPO/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
SM_FW=${SM_FW:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}
TIMEOUT=${TIMEOUT:-200}
SCMI="SCMI Protocol v2.1 'NXP:IMX' Firmware version 0x333"
USERSPACE="=== imx95 busybox userspace ==="

for v in QEMU KERNEL DTB INITRD SM_FW; do
  f=${!v}; [ -e "$f" ] || { echo "MISSING $v: $f"; exit 2; }
done
LOG=$(mktemp /tmp/bsp-smoke.XXXX.log)
echo "== BSP-matrix boot-smoke: $LABEL =="
echo "   kernel=$(basename "$KERNEL")  sm=$(basename "$SM_FW")"
timeout "$TIMEOUT" "$QEMU" -M imx95-19x19-evk -m 4G -display none -serial "file:$LOG" -monitor none \
  -kernel "$KERNEL" -dtb "$DTB" -initrd "$INITRD" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_FW",cpu-num=6 &
QPID=$!
got_scmi=0; got_us=0
for _ in $(seq 1 "$TIMEOUT"); do
  kill -0 "$QPID" 2>/dev/null || break
  grep -qF "$SCMI" "$LOG" 2>/dev/null && got_scmi=1
  grep -qF "$USERSPACE" "$LOG" 2>/dev/null && got_us=1
  [ $got_scmi = 1 ] && [ $got_us = 1 ] && break
  sleep 1
done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
echo "   SCMI handshake (real SM): $([ $got_scmi = 1 ] && echo PASS || echo ----)"
echo "   userspace roll-call:      $([ $got_us = 1 ] && echo PASS || echo ----)"
if [ $got_scmi = 1 ] && [ $got_us = 1 ]; then
  echo ">>> $LABEL: PASS (booted to userspace, SCMI served by the real SM)"; echo "   log: $LOG"; exit 0
else
  echo ">>> $LABEL: FAIL"; echo "   --- last serial lines ---"; tail -15 "$LOG" 2>/dev/null; exit 1
fi
