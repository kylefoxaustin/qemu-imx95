#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# SPI INTERCONNECT (i.MX 95): prove two QEMU i.MX 95 instances pass real data
# over an SPI board-to-board link, bridged by a QEMU socket - the shape a lab
# coordinator wires for an SPI segment (fleet mission #5). A known payload
# travels guest A's LPSPI (via spidev) -> spi-link peripheral -> socket bridge ->
# guest B's spi-link -> LPSPI -> spidev, verified byte-exact on the receiver.
#
# SPI is master-driven, so each side is an LPSPI *master* with a spi-link SSI
# peripheral on its bus (-device spi-link,bus=lpspi7,chardev=...). spi-link
# forwards each shifted-out byte (MOSI) to the chardev and returns peer bytes on
# MISO from an rx FIFO - the sender clocks the payload out, the receiver clocks
# dummy bytes to shift it in.
#
# On the 19x19 EVK, lpspi7 (spi@42710000) is the enabled SPI master (an lwn,bk4
# node); the harness re-points its child at a plain spidev (rohm,dh2228fv is in
# the kernel spidev allow-list) so the guest exposes /dev/spidev0.0. lpspi7 keeps
# its DTB dmas because the 95 fsl-lpspi driver runs transfers over eDMA (usedma):
# the eDMA bursts each 8-bit word to TDR one BYTE at a time, so imx95_lpspi must
# accept byte-width MMIO (fixed: lpspi_ops .valid/.impl min_access_size=1; a
# 4-byte-only window silently dropped every DMA burst and nothing reached the SSI
# bus - the bug this test caught). Then the eDMA's TDR writes hit lpspi_transfer
# -> ssi_transfer -> spi-link exactly like a PIO write would on i.MX91/93 (which
# run the same driver in PIO). spi-link + the LPSPI SSI-master model are the
# fleet-shared transport (authored on i.MX 91, hw/ssi/spi_link.c ported verbatim).
# Required artifacts (env):
#   QEMU, KBUILD (Image + dtb + scripts/dtc/dtc), SM_ELF, an aarch64 cross gcc.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=$KBUILD/arch/arm64/boot/Image
DTB=$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb
DTC=$KBUILD/scripts/dtc/dtc
DEFAULT_CPIO=${DEFAULT_CPIO:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
CROSS=${CROSS:-aarch64-linux-gnu-gcc}
PAYLOAD=${PAYLOAD:-IMX95-SPI-LINK-payload-0123456789}
TMO=${TMO:-220}

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }
for f in "$QEMU" "$IMAGE" "$DTC" "$SM_ELF" "$DEFAULT_CPIO" "$DTB"; do
    [ -e "$f" ] || skip "missing $f"
done
command -v "$CROSS" >/dev/null || skip "no cross compiler ($CROSS)"

SOCK=${SOCK:-$(mktemp -u /tmp/imx95-spilink.XXXXXX.sock)}
WORK=$(mktemp -d); trap 'rm -rf "$WORK" "$SOCK"; kill ${SPID:-} ${CPID:-} 2>/dev/null' EXIT
SPID=; CPID=

# ---- build the spidev oracle (static aarch64) -------------------------------
"$CROSS" -O2 -static -o "$WORK/spilink" "$HERE/spilink.c" || die "spilink build failed"

# ---- patch the DTB: give lpspi7 (spi@42710000) a plain spidev child ---------
# lpspi7 is already status=okay on the EVK with an lwn,bk4 child; swap that child's
# compatible for rohm,dh2228fv (kernel spidev allow-list) so the guest binds
# spidev and exposes /dev/spidev0.0. Keep the node's dmas: the fsl-lpspi driver
# runs the transfer over eDMA, which the imx95_lpspi byte-access fix now services.
"$DTC" -I dtb -O dts "$DTB" 2>/dev/null > "$WORK/base.dts" || die "dtc decompile failed"
awk '
  /spi@42710000 \{/ { inn = 1 }
  inn && /compatible = "lwn,bk4"/ {
    print "\t\t\t\t\tcompatible = \"rohm,dh2228fv\";"; next
  }
  inn && /^\t\t\t\};/ { inn = 0 }
  { print }
' "$WORK/base.dts" > "$WORK/spi.dts"
"$DTC" -I dts -O dtb "$WORK/spi.dts" 2>/dev/null > "$WORK/spi.dtb" || die "dtc recompile failed"
DTB2="$WORK/spi.dtb"

# ---- stage a role-based initramfs -------------------------------------------
build_initrd() {            # $1=role  -> echoes path
    local role=$1
    local stage="$WORK/$role"
    mkdir -p "$stage"
    zcat "$DEFAULT_CPIO" | (cd "$stage" && cpio -idmu 2>/dev/null)
    install -m755 "$WORK/spilink" "$stage/spilink"
    printf 'ROLE=%s\nPAYLOAD=%s\n' "$role" "$PAYLOAD" > "$stage/linkenv"
    cat > "$stage/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox mount -t sysfs sys /sys 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
. /linkenv
n=0; while [ ! -c "$(ls /dev/spidev* 2>/dev/null | head -1)" ] && [ $n -lt 30 ]; do
    sleep 1; n=$((n+1)); done
S=$(ls /dev/spidev* 2>/dev/null | head -1)
[ -c "$S" ] || { echo "SPILINK:FAIL:no /dev/spidev (lpspi7 not bound)"; \
                 dmesg | grep -iE 'spi|lpspi' | tail -5; busybox poweroff -f; }
echo "=== INTERCONNECT spi ($ROLE on $S) ==="
if [ "$ROLE" = recv ]; then
    /spilink recv "$S" "$PAYLOAD"
else
    sleep 8                       # let the receiver open + the socket connect,
    n=0                           # then resend across the receiver's clock window
    while [ "$n" -lt 15 ]; do     # (the peer boots slower; generous overlap)
        /spilink send "$S" "$PAYLOAD"
        sleep 2
        n=$((n + 1))
    done
fi
echo "=== INTERCONNECT-DONE ==="
/bin/busybox poweroff -f
INIT
    chmod +x "$stage/init"
    ( cd "$stage" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/$role.gz"
    echo "$WORK/$role.gz"
}

RECV_IRD=$(build_initrd recv)
SEND_IRD=$(build_initrd send)

boot() {                    # $1=initrd  $2=chardev-args  $3=logfile
    timeout --signal=KILL "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
        -kernel "$IMAGE" -dtb "$DTB2" -initrd "$1" \
        -append "earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
        -device loader,file="$SM_ELF",cpu-num=6 \
        $2 -device spi-link,bus=lpspi7,chardev=spil \
        -serial file:"$3" -serial null -monitor none >/dev/null 2>&1 &
}

# ---- receiver (socket server) first, then sender (client) -------------------
RLOG="$WORK/recv.log"; SLOG="$WORK/send.log"
echo "== booting i.MX 95 RECEIVER (spi-link socket listen) =="
boot "$RECV_IRD" "-chardev socket,id=spil,path=$SOCK,server=on,wait=off" "$RLOG"; SPID=$!
sleep 3
echo "== booting i.MX 95 SENDER (spi-link socket connect) =="
boot "$SEND_IRD" "-chardev socket,id=spil,path=$SOCK,server=off,reconnect-ms=1000" "$SLOG"; CPID=$!

wait $SPID $CPID 2>/dev/null

echo "================== SPI LINK =================="
grep -aE 'INTERCONNECT|SPILINK:' "$RLOG" "$SLOG" | grep -avE '^\[ *[0-9]+\.[0-9]+\]' | \
    sed 's/\x1b\[[0-9;]*[a-zA-Z]//g; s#.*/##'
if grep -aq 'SPILINK:PASS' "$RLOG"; then
    echo "RESULT: PASS (payload crossed LPSPI<->spi-link<->socket<->spi-link<->LPSPI byte-exact between two i.MX 95 guests)"
    exit 0
fi
die "spi link did not complete"
