#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# CROSS-SoC SPI check: i.MX95 <-> i.MX91, one spi_link.c over a unix socket,
# BOTH directions, byte-exact. This proves the fleet-shared SPI transport bridges
# two DIFFERENT LPSPI datapaths transparently: the i.MX95 fsl-lpspi runs the
# transfer over eDMA (byte-wide TDR bursts; see run-spi.sh) while the i.MX91 runs
# it in PIO (32-bit TDR writes) - the same spi_link.c carries both.
#
#   91 -> 95 : i.MX91 PIO master (lpspi1) sends, i.MX95 eDMA master (lpspi7) recvs
#   95 -> 91 : i.MX95 eDMA master (lpspi7) sends, i.MX91 PIO master (lpspi1) recvs
#
# Needs BOTH trees built locally (a peer i.MX91 qemu + BSP). Everything is
# env-overridable and the check skip()s cleanly when the i.MX91 side is absent,
# so it is a no-op on a 95-only host. Runs self-contained (both qemus on one box,
# one side server / one side reconnect-ms client).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CROSS=${CROSS:-aarch64-linux-gnu-gcc}
PAYLOAD=${PAYLOAD:-FLEET-SPI-XCHECK-9591}
TMO=${TMO:-200}

# ---- i.MX95 (this tree) artifacts ----
QEMU95=${QEMU95:-${QEMU:-$ROOT/build/qemu-system-aarch64}}
KBUILD95=${KBUILD95:-${KBUILD:-$HOME/Documents/linux-imx95-build}}
IMG95=${IMG95:-$KBUILD95/arch/arm64/boot/Image}
DTC95=${DTC95:-$KBUILD95/scripts/dtc/dtc}
DTB95=${DTB95:-$KBUILD95/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
SM95=${SM_ELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}
CPIO95=${CPIO95:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}

# ---- i.MX91 (peer) artifacts ----
QEMU91=${QEMU91:-$HOME/Documents/GitHub/91emulator/build/qemu-system-aarch64}
DEPLOY91=${DEPLOY91:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
IMG91=${IMG91:-$DEPLOY91/Image}
DTB91=${DTB91:-$DEPLOY91/imx91-11x11-evk.dtb}
DTC91=${DTC91:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/work-shared/imx91evk/kernel-build-artifacts/scripts/dtc/dtc}
CPIO91=${CPIO91:-$HOME/Documents/GitHub/91emulator/tests/busybox-imx91/busybox-imx91.cpio.gz}

skip() { echo "SKIP: $*"; exit 0; }
for f in "$QEMU95" "$IMG95" "$DTC95" "$DTB95" "$SM95" "$CPIO95" \
         "$QEMU91" "$IMG91" "$DTB91" "$DTC91" "$CPIO91"; do
    [ -e "$f" ] || skip "missing $f (need both the i.MX95 and i.MX91 trees)"
done
command -v "$CROSS" >/dev/null || skip "no cross compiler ($CROSS)"

WORK=$(mktemp -d); SOCK=$(mktemp -u /tmp/xspi.XXXX.sock)
SPID=; CPID=
trap 'rm -rf "$WORK" "$SOCK"; kill ${SPID:-} ${CPID:-} 2>/dev/null' EXIT

"$CROSS" -O2 -static -o "$WORK/spilink" "$HERE/spilink.c" || { echo "FAIL build"; exit 1; }

# 95 dtb: lpspi7 (spi@42710000) bk4 -> rohm spidev (keep dmas -> eDMA path)
"$DTC95" -I dtb -O dts "$DTB95" 2>/dev/null | awk '
  /spi@42710000 \{/ { inn = 1 }
  inn && /compatible = "lwn,bk4"/ { print "\t\t\t\t\tcompatible = \"rohm,dh2228fv\";"; next }
  inn && /^\t\t\t\};/ { inn = 0 } { print }' > "$WORK/d95.dts"
"$DTC95" -I dts -O dtb "$WORK/d95.dts" 2>/dev/null > "$WORK/d95.dtb"
# 91 dtb: lpspi1 (spi@44360000) enable + strip dmas (PIO) + spidev child
"$DTC91" -I dtb -O dts "$DTB91" 2>/dev/null | awk '
  /spi@44360000 \{/ { inn = 1 }
  inn && /dmas =|dma-names =/ { next }
  inn && /status = "disabled"/ { print "\t\t\t\tstatus = \"okay\";"; next }
  inn && /^\t\t\t\};/ {
    print "\t\t\t\tspidev@0 { compatible = \"rohm,dh2228fv\"; reg = <0x00>; spi-max-frequency = <0xf4240>; };";
    print; inn = 0; next } { print }' > "$WORK/d91.dts"
"$DTC91" -I dts -O dtb "$WORK/d91.dts" 2>/dev/null > "$WORK/d91.dtb"

# staged initramfs per (tree,role); tree busybox differs
mkird() {   # $1=tag $2=cpio $3=role
    local st="$WORK/$1-$3"
    mkdir -p "$st"; ( cd "$st" && zcat "$2" | cpio -idmu --quiet 2>/dev/null )
    cp "$WORK/spilink" "$st/spilink"
    printf 'ROLE=%s\nPAYLOAD=%s\n' "$3" "$PAYLOAD" > "$st/linkenv"
    cat > "$st/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc; /bin/busybox mount -t devtmpfs dev /dev 2>/dev/null; /bin/busybox mount -t sysfs sys /sys 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null; exec >/dev/console 2>&1
. /linkenv
n=0; while [ ! -c "$(ls /dev/spidev* 2>/dev/null|head -1)" ] && [ $n -lt 30 ]; do sleep 1; n=$((n+1)); done
S=$(ls /dev/spidev* 2>/dev/null|head -1)
[ -c "$S" ] || { echo "SPILINK:FAIL:no /dev/spidev"; busybox poweroff -f; }
echo "=== XSPI $ROLE on $S ==="
if [ "$ROLE" = recv ]; then /spilink recv "$S" "$PAYLOAD"
else sleep 8; n=0; while [ "$n" -lt 12 ]; do /spilink send "$S" "$PAYLOAD"; sleep 2; n=$((n+1)); done; fi
echo "=== XSPI-DONE ==="; /bin/busybox poweroff -f
INIT
    chmod +x "$st/init"; ( cd "$st" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/$1-$3.gz"
}
mkird 95 "$CPIO95" recv; mkird 95 "$CPIO95" send
mkird 91 "$CPIO91" recv; mkird 91 "$CPIO91" send

boot95() { timeout -k 5 "$TMO" "$QEMU95" -M imx95-19x19-evk -m 2G -display none \
    -kernel "$IMG95" -dtb "$WORK/d95.dtb" -initrd "$1" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -device loader,file="$SM95",cpu-num=6 $2 \
    -device spi-link,bus=lpspi7,chardev=spil -serial file:"$3" -serial null -monitor none >/dev/null 2>&1 & }
boot91() { timeout -k 5 "$TMO" "$QEMU91" -M imx91-11x11-evk -smp 1 -m 2G -display none \
    -kernel "$IMG91" -dtb "$WORK/d91.dtb" -initrd "$1" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" $2 \
    -device spi-link,bus=lpspi1,chardev=spil -serial file:"$3" -nic none >/dev/null 2>&1 & }

SRV="-chardev socket,id=spil,path=$SOCK,server=on,wait=off"
CLI="-chardev socket,id=spil,path=$SOCK,server=off,reconnect-ms=1000"
pass1=FAIL; pass2=FAIL

# ---- direction 1: 91 (PIO) -> 95 (eDMA); 95 receiver is the server ----
rm -f "$SOCK"; RA="$WORK/d1_recv95.log"; SA="$WORK/d1_send91.log"
echo "== 91->95: booting i.MX95 RECEIVER (lpspi7 eDMA, server) =="
boot95 "$WORK/95-recv.gz" "$SRV" "$RA"; SPID=$!
sleep 4
echo "== 91->95: booting i.MX91 SENDER (lpspi1 PIO, client) =="
boot91 "$WORK/91-send.gz" "$CLI" "$SA"; CPID=$!
wait $SPID $CPID 2>/dev/null
grep -aq 'SPILINK:PASS' "$RA" && pass1=PASS

# ---- direction 2: 95 (eDMA) -> 91 (PIO); 91 receiver is the server ----
rm -f "$SOCK"; RB="$WORK/d2_recv91.log"; SB="$WORK/d2_send95.log"
echo "== 95->91: booting i.MX91 RECEIVER (lpspi1 PIO, server) =="
boot91 "$WORK/91-recv.gz" "$SRV" "$RB"; SPID=$!
sleep 4
echo "== 95->91: booting i.MX95 SENDER (lpspi7 eDMA, client) =="
boot95 "$WORK/95-send.gz" "$CLI" "$SB"; CPID=$!
wait $SPID $CPID 2>/dev/null
grep -aq 'SPILINK:PASS' "$RB" && pass2=PASS

echo "============== 95<->91 SPI CROSS-CHECK =============="
echo "  91 PIO  -> 95 eDMA : $pass1  ($(grep -aoE 'SPILINK:PASS: [0-9]+ bytes' "$RA" | head -1))"
echo "  95 eDMA -> 91 PIO  : $pass2  ($(grep -aoE 'SPILINK:PASS: [0-9]+ bytes' "$RB" | head -1))"
[ "$pass1" = PASS ] && [ "$pass2" = PASS ] && {
    echo "RESULT: PASS (i.MX95 <-> i.MX91 SPI byte-exact both ways over one spi_link.c)"; exit 0; }
echo "RESULT: FAIL"; exit 1
