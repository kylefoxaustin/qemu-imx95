#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 FlexSPI + serial NOR test (hw/ssi/imx_fspi.c, spi-nxp-fspi driver).
#
# flexspi1 @0x425e0000 ("nxp,imx8mm-fspi") was a UNIMP stub returning zeros, so
# spi-nor's JEDEC-ID/SFDP probe matched no flash and the driver failed. The real
# model decodes the driver's LUT and replays it onto an SSI bus carrying a
# QEMU m25p80 (a Micron mt25ql512ab; the EVK's real mt35xu01gbba forces an
# 8D-8D-8D mode the generic m25p80 doesn't model, so a fully-SDR part stands in
# and exercises the same controller datapath), so:
#
#   - spi-nxp-fspi binds and registers an spi-mem controller,
#   - spi-nor reads the JEDEC ID + SFDP and identifies the flash,
#   - an mtd device appears, a raw read returns the flash content (erased 0xff
#     from the freshly-created chip), and a page-program write reads straight
#     back (the TX FIFO path).
#
# PASS = mtd device enumerated + no probe failure + clean read + write round-
#        trip, with zero faults.
#
# Required artifacts (override via env): QEMU, KBUILD (Image + dtb), SM_ELF.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=$KBUILD/arch/arm64/boot/Image
DTB=$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb
TMO=${TMO:-120}

need() { [ -e "$2" ] || { echo "missing $1: $2"; exit 1; }; }
need QEMU "$QEMU"; need IMAGE "$IMAGE"; need DTB "$DTB"; need SM_ELF "$SM_ELF"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG=${LOG:-$WORK/serial.log}
root="$WORK/root"; mkdir -p "$root"
zcat "$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz" | \
    (cd "$root" && cpio -idmu 2>/dev/null)
cat > "$root/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
sleep 8
echo "=== FLEXSPI-TEST ==="
echo "-- spi-nor / mtd identification --"
dmesg | grep -iE 'fspi|spi-nor|spi_nor|jedec|mt35|mtd[0-9]' | grep -ivE 'mtdblock'
echo "-- mtd devices --"
cat /proc/mtd 2>/dev/null
echo "-- raw mtd read (first 16 bytes) --"
# A freshly-created m25p80 with no backing image is erased (all 0xff); a read
# off the mtd char device must return those bytes, proving the read datapath
# (not the stub's zeros).
dd if=/dev/mtd0 bs=16 count=1 2>/dev/null | od -An -tx1 | head -1
echo "-- write/read-back (page-program path) --"
# Writing into erased flash only clears bits (1->0), so a fresh region takes an
# arbitrary pattern with no explicit erase. This exercises WREN + page program
# (the TX FIFO path) and reads it straight back.
printf 'FlexSPI-OK\xa5\x5a\x01\x02\x03\x04' > /pat
dd if=/pat of=/dev/mtd0 bs=16 count=1 conv=notrunc 2>/dev/null
dd if=/dev/mtd0 bs=16 count=1 2>/dev/null | od -An -c | head -1
echo "=== FLEXSPI-TEST-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$root/init"
( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "=== flexspi test report ==="
sed -n '/FLEXSPI-TEST ===/,/FLEXSPI-TEST-DONE/p' "$LOG" | grep -vE 'FLEXSPI-TEST ==='

pass=1
# spi-nxp-fspi + spi-nor enumerate the flash as a 64 MiB (0x04000000) mtd
# device; the stub never got this far (probe failed -22 / no mtd).
grep -qaE 'mtd0: 04000000 .* "425e0000.spi"' "$LOG" \
    && echo "  ok   spi-nor enumerated the NOR (64 MiB mtd0)" \
    || { echo "  MISS flash not enumerated"; pass=0; }
grep -qaiE 'spi-nor.*(failed|probe with driver spi-nor failed)' "$LOG" \
    && { echo "  MISS spi-nor probe failed"; pass=0; } \
    || echo "  ok   no spi-nor probe failure"
grep -qaE 'ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff' "$LOG" \
    && echo "  ok   mtd read returned flash data (erased 0xff)" \
    || { echo "  MISS mtd read (datapath?)"; pass=0; }
grep -qaE 'F   l   e   x   S   P   I   -   O   K' "$LOG" \
    && echo "  ok   page-program write/read-back round-trips (TX path)" \
    || { echo "  MISS write/read-back (TX path?)"; pass=0; }
for p in 'external abort' 'Unhandled fault' 'Oops:' 'Kernel panic'; do
    n=$(grep -caE "$p" "$LOG" 2>/dev/null || true); [ "${n:-0}" = "0" ] || { echo "  anomaly: $p=$n"; pass=0; }
done

[ "$pass" = 1 ] && { echo "PASS: FlexSPI + NOR enumerates and reads"; exit 0; }
echo "FAIL"; exit 1
