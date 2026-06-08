#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# uSDHC read/write datapath test for BOTH card types: an eMMC on uSDHC1 (the
# EVK's soldered eMMC slot) and an SD card on uSDHC2 (the removable SD slot).
#
# For each card the in-guest init writes a 2 MiB random pattern to the raw
# block device, syncs, drops caches, reads it back and compares md5 - exercising
# the multi-block WRITE (CMD25) + READ (CMD18) ADMA datapath end to end against
# the stock Linux sdhci-esdhc-imx driver. The host then confirms the pattern
# actually persisted into each backing image (proof the writes reached storage,
# not just the page cache).
#
# This is the regression test for the open-ended multi-block write fix
# ("Card stuck being busy", hw/sd/sd.c + hw/sd/sdhci.c): before it, the write
# wedged and no writable card was usable; reads always worked.
#
# NOTE: the EVK's SD slot (uSDHC2) has a card-detect GPIO on a pca953x I2C
# expander that the model reports as "empty", so the test patches the DT with
# `non-removable` to force the attached card to be probed. The CD-GPIO/
# card-presence wiring is a separate fidelity item; the write/read datapath
# under test here is independent of it.
#
# Required (override via env): QEMU, KBUILD (Image + dtb + dtc), SM_ELF.
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
TMO=${TMO:-240}

need() { [ -e "$2" ] || { echo "SKIP: missing $1: $2"; exit 0; }; }
need QEMU "$QEMU"; need IMAGE "$IMAGE"; need DTB "$DTB"; need DTC "$DTC"
need SM_ELF "$SM_ELF"; need INITRD "$INITRD"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG="$WORK/serial.log"
EMMC="$WORK/emmc.img"; SD="$WORK/sd.img"; PAT="$WORK/pattern"

# Power-of-2 card images (the SD/eMMC models require it) + a known pattern.
truncate -s 64M "$EMMC" "$SD"
head -c 2097152 /dev/urandom > "$PAT"

# DT: force-probe the attached SD card on uSDHC2 (bypass the unmodelled CD GPIO).
"$DTC" -I dtb -O dts "$DTB" > "$WORK/base.dts" 2>/dev/null
python3 - "$WORK/base.dts" "$WORK/sd.dts" <<'PY'
import sys
t = open(sys.argv[1]).read()
i = t.index('mmc@42860000 {'); b = t.index('{', i)
t = t[:b+1] + '\n\t\t\t\tnon-removable;\n\t\t\t\tbroken-cd;' + t[b+1:]
open(sys.argv[2], 'w').write(t)
PY
"$DTC" -I dts -O dtb -o "$WORK/sd.dtb" "$WORK/sd.dts" 2>/dev/null

# initramfs: busybox + the pattern + the write/readback init.
STAGE="$WORK/root"; mkdir -p "$STAGE"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
cp "$PAT" "$STAGE/pattern"
cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
i=0; while [ $i -lt 60 ]; do ls /sys/block/mmcblk* >/dev/null 2>&1 && break; sleep 1; i=$((i+1)); done
sleep 3
echo "=== SD-EMMC-TEST ==="
echo "block devs: $(ls -d /sys/block/mmcblk* 2>/dev/null | xargs -n1 basename 2>/dev/null | tr '\n' ' ')"
for d in /sys/block/mmcblk*; do
  [ -e "$d" ] || continue
  n=$(basename "$d"); case "$n" in *boot*|*rpmb*) continue;; esac
  typ=$(cat "$d/device/type" 2>/dev/null)
  dd if=/pattern of=/dev/$n bs=1M count=2 conv=fsync 2>/dev/null; sync
  echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
  dd if=/dev/$n of=/readback bs=1M count=2 2>/dev/null
  pm=$(md5sum /pattern | cut -d' ' -f1); rm=$(md5sum /readback 2>/dev/null | cut -d' ' -f1)
  if [ "$pm" = "$rm" ] && [ -n "$rm" ]; then r=PASS; else r=FAIL; fi
  echo "RESULT $n type=$typ WRITE_READBACK=$r"
done
echo "=== SD-EMMC-TEST-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -kernel "$IMAGE" -dtb "$WORK/sd.dtb" -initrd "$WORK/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -drive if=none,format=raw,file="$EMMC",id=mmc0 -device emmc,drive=mmc0 \
    -drive if=sd,format=raw,file="$SD" \
    -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "=== sd-emmc report ==="
sed -n '/SD-EMMC-TEST ===/,/SD-EMMC-TEST-DONE/p' "$LOG" | grep -vE '^\[' | grep -vE 'SD-EMMC-TEST'

pass=1
emmc_ok=$(grep -c 'type=MMC WRITE_READBACK=PASS' "$LOG" 2>/dev/null || true)
sd_ok=$(grep -c 'type=SD WRITE_READBACK=PASS' "$LOG" 2>/dev/null || true)
[ "${emmc_ok:-0}" -ge 1 ] && echo "  ok   eMMC write+readback" || { echo "  MISS eMMC write+readback"; pass=0; }
[ "${sd_ok:-0}" -ge 1 ]   && echo "  ok   SD write+readback"   || { echo "  MISS SD write+readback"; pass=0; }

# Host-side persistence: the written pattern must be in each backing image.
for nm in EMMC:emmc SD:sd; do
    lbl=${nm%%:*}; var=${nm##*:}; img=$([ "$var" = emmc ] && echo "$EMMC" || echo "$SD")
    if cmp -s -n 2097152 "$img" "$PAT"; then echo "  ok   $lbl persisted to backing image";
    else echo "  MISS $lbl not persisted"; pass=0; fi
done

[ "$pass" = 1 ] && { echo "PASS: SD + eMMC read/write datapaths verified"; exit 0; }
echo "FAIL: see misses above"; exit 1
