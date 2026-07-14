#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 U-Boot end-to-end: ROM handover -> SM -> SPL -> U-Boot proper ->
# fetch a file over Ethernet.
#
# This is the boot path a developer actually uses on a board, and the only one
# that exercises early-boot ordering (a "-kernel" boot skips the whole chain):
#
#   machine writes the ROM handover + passover tables
#     -> SM boots on the M33, brings up SCMI, releases the A55 from CPUWAIT
#     -> SPL runs, probes SCMI, learns from the passover that it booted from
#        eMMC, walks image set 0 and loads U-Boot proper from the next container
#     -> U-Boot proper reaches its prompt, brings up ENETC through the EMDIO
#        clause-22 PHY, and TFTPs a file into DRAM
#
# PASS = U-Boot prompt reached AND a byte-exact file arrives over TFTP
#        (verified by CRC32 against the host's own copy).
#
# The boot media is generated here by mkbootimg.py - no imx-mkimage needed; the
# container format is a 16-byte header plus 128-byte image entries.
#
# Required (override via env): QEMU, SM_ELF, SPL_BIN, UBOOT_BIN.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
SPL_BIN=${SPL_BIN:-$ROOT/tests/spl-banner/uboot-build/spl/u-boot-spl.bin}
UBOOT_BIN=${UBOOT_BIN:-$ROOT/tests/spl-banner/uboot-build/u-boot.bin}
TMO=${TMO:-240}

skip() { echo "SKIP: $*"; exit 0; }
need() { [ -e "$2" ] || skip "missing $1: $2"; }
need QEMU "$QEMU"; need SM_ELF "$SM_ELF"
need SPL_BIN "$SPL_BIN"; need UBOOT_BIN "$UBOOT_BIN"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG=${LOG:-$WORK/serial.log}
TFTPDIR="$WORK/tftp"; mkdir -p "$TFTPDIR"

# A payload big enough to need many frames, so this exercises the RX ring
# wrapping and the driver's batched buffer-descriptor recycling, not just one
# lucky packet.
head -c 200000 /dev/urandom > "$TFTPDIR/payload.bin"
WANT_CRC=$(python3 -c "import zlib,sys;print('%08x' % (zlib.crc32(open(sys.argv[1],'rb').read()) & 0xffffffff))" "$TFTPDIR/payload.bin")
WANT_SIZE=$(stat -c%s "$TFTPDIR/payload.bin")

python3 "$HERE/mkbootimg.py" "$UBOOT_BIN" "$WORK/boot.img" >/dev/null

# Hammer newlines to interrupt autoboot, then drive the prompt.
{
    for _ in $(seq 1 30); do printf '\n'; sleep 0.25; done
    sleep 2
    printf 'setenv ipaddr 10.0.2.15\n';                sleep 1
    printf 'setenv serverip 10.0.2.2\n';               sleep 1
    printf 'setenv ethaddr 00:04:9f:06:11:22\n';       sleep 1
    printf 'ping 10.0.2.2\n';                          sleep 8
    printf 'tftpboot 0x91000000 payload.bin\n';        sleep 40
    printf 'crc32 0x91000000 ${filesize}\n';           sleep 6
    printf 'printenv filesize\n';                      sleep 3
} | timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -device loader,file="$SPL_BIN",addr=0x20480000,cpu-num=0,force-raw=on \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -drive if=none,format=raw,file="$WORK/boot.img",id=mmc0 -device emmc,drive=mmc0 \
    -nic user,model=fsl-enetc,tftp="$TFTPDIR" \
    -serial stdio -serial null 2>&1 | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' > "$LOG" || true

echo "=== u-boot report ==="
grep -aE "^U-Boot|Trying to boot from|Boot stage:|Load image from|is alive|Bytes transferred|crc32 for|filesize=" "$LOG" | head -12

pass=1
grep -qa 'U-Boot SPL' "$LOG"          && echo "  ok   SPL banner" || { echo "  MISS SPL banner"; pass=0; }
grep -qa 'Trying to boot from MMC1' "$LOG" && echo "  ok   passover -> boot device MMC1" || { echo "  MISS boot device"; pass=0; }
grep -qaE '^U-Boot 20' "$LOG"         && echo "  ok   U-Boot proper loaded from the container" || { echo "  MISS U-Boot proper"; pass=0; }
grep -qa 'u-boot=>' "$LOG"            && echo "  ok   U-Boot prompt" || { echo "  MISS prompt"; pass=0; }
grep -qa 'host 10.0.2.2 is alive' "$LOG" && echo "  ok   ENETC link + ping" || { echo "  MISS ping"; pass=0; }
grep -qa "Bytes transferred = $WANT_SIZE " "$LOG" && echo "  ok   TFTP transferred $WANT_SIZE bytes" || { echo "  MISS tftp byte count"; pass=0; }
if grep -qa "==> $WANT_CRC" "$LOG"; then
    echo "  ok   CRC32 $WANT_CRC matches the host file (byte-exact over the wire)"
else
    echo "  MISS crc mismatch (wanted $WANT_CRC)"; pass=0
fi
for a in 'Kernel panic' 'Synchronous Abort' 'Unhandled fault' 'ERROR ### Please RESET'; do
    grep -qa "$a" "$LOG" && { echo "  MISS anomaly: $a"; pass=0; }
done

[ "$pass" = 1 ] && { echo "PASS: U-Boot boots off eMMC and fetches a byte-exact file over Ethernet"; exit 0; }
echo "FAIL"; exit 1
