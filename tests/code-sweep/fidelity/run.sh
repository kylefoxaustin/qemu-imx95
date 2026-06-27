#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fidelity detectors: boot the model and CLASSIFY peripheral blocks as COMPUTES /
# FAULTS / SILENT-WRONG (signals done but returns garbage). This is the "hunt"
# side of the board-farm fidelity standard - it surfaces blocks that silently
# lie so the farm knows what not to trust. See docs/validation/fidelity-audit.md.
#
# A detector "passes" by classifying its block correctly; a SILENT-WRONG verdict
# is the detector working, not the model passing. Exit is informational (0 if the
# detectors ran), with SILENT-WRONG blocks listed.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=${IMAGE:-$KBUILD/arch/arm64/boot/Image}
DTB=${DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
INITRD=${INITRD:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
CC=${CC:-aarch64-linux-gnu-gcc}; TMO=${TMO:-180}
skip() { echo "SKIP: $*"; exit 0; }
for f in "$QEMU" "$IMAGE" "$DTB" "$SM_ELF" "$INITRD"; do [ -e "$f" ] || skip "missing $f"; done
command -v "$CC" >/dev/null || skip "no cross compiler"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/root"; mkdir -p "$STAGE"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
# build every detector C in this dir into the initramfs
built=""
for c in "$HERE"/*_detect.c; do
    [ -e "$c" ] || continue
    n=$(basename "$c" .c)
    "$CC" -O2 -static -o "$STAGE/$n" "$c" 2>/dev/null && built="$built $n"
done
[ -n "$built" ] || skip "no detectors built"
{ echo "#!/bin/busybox sh"; cat <<INIT
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
sleep 3
echo "=== FIDELITY ==="
for d in$built; do echo "---- \$d ----"; "/\$d"; done
echo "=== FIDELITY-DONE ==="
/bin/busybox poweroff -f
INIT
} > "$STAGE/init"
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.gz"

LOG="$WORK/serial.log"
timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "================== FIDELITY DETECTORS =================="
sed -n '/=== FIDELITY ===/,/=== FIDELITY-DONE ===/p' "$LOG" | grep -vE '^\[[0-9 ]+\.[0-9]+\]'
echo "======================================================="
grep -qa '=== FIDELITY-DONE ===' "$LOG" || { echo "FAIL: detectors did not finish"; exit 1; }
sw=$(sed -n '/=== FIDELITY ===/,/=== FIDELITY-DONE ===/p' "$LOG" | grep -c '^VERDICT:.*SILENT-WRONG')
echo "blocks classified SILENT-WRONG: $sw  (see docs/validation/fidelity-audit.md)"
