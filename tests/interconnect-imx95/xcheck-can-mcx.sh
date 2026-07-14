#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# CROSS-SoC CAN check: i.MX95 (Linux FlexCAN) <-> MCXN947 (bare-metal Cortex-M33
# FlexCAN firmware), both on the generic can-host-chardev backend, over a TCP
# socket - byte-exact BOTH directions. Proves the fleet-shared CAN transport
# joins a Linux SocketCAN node and a bare-metal M33 node on ONE bus:
#
#   MCX -> 95 : MCX firmware sends std 0x321 (DE AD BE EF CA FE BA BE), the 95
#               Linux FlexCAN receives + verifies it (canlink respond)
#   95 -> MCX : the 95 replies std 0x322 (11 22 33 44 55 66 77 88), the MCX
#               RX ISR verifies it -> "CAN LINK PASS"
#
# The MCX firmware resends until the peer connects, so the (slow) 95 responder is
# booted first, then the (fast) MCX client. Needs the mcxn947qemu tree built
# locally (qemu-system-arm + canlink.elf); env-overridable, skip()s cleanly when
# the MCX side is absent.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CROSS=${CROSS:-aarch64-linux-gnu-gcc}
PORT=${CAN_PORT:-$(( 15000 + RANDOM % 2000 ))}
TMO=${TMO:-240}

# ---- i.MX95 (this tree) artifacts ----
QEMU95=${QEMU95:-${QEMU:-$ROOT/build/qemu-system-aarch64}}
KBUILD95=${KBUILD95:-${KBUILD:-$HOME/Documents/linux-imx95-build}}
IMG95=${IMG95:-$KBUILD95/arch/arm64/boot/Image}
DTC95=${DTC95:-$KBUILD95/scripts/dtc/dtc}
FDT95=${FDT95:-$KBUILD95/scripts/dtc/fdtoverlay}
DTB95=${DTB95:-$KBUILD95/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
SM95=${SM_ELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}
CPIO95=${CPIO95:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
FLEX=${FLEX:-$ROOT/tests/flexcan}

# ---- MCXN947 (peer) artifacts ----
QEMU_MCX=${QEMU_MCX:-$HOME/Documents/GitHub/mcxn947qemu/build/qemu-system-arm}
MCX_ELF=${MCX_ELF:-$HOME/Documents/GitHub/mcxn947qemu/tests/mcxn-can-link/canlink.elf}

skip() { echo "SKIP: $*"; exit 0; }
for f in "$QEMU95" "$IMG95" "$DTC95" "$FDT95" "$DTB95" "$SM95" "$CPIO95" \
         "$FLEX/flexcan-overlay.dtso" "$QEMU_MCX" "$MCX_ELF"; do
    [ -e "$f" ] || skip "missing $f (need both the i.MX95 and MCXN947 trees)"
done
command -v "$CROSS" >/dev/null || skip "no cross compiler ($CROSS)"
for ko in net/can/can.ko drivers/net/can/dev/can-dev.ko net/can/can-raw.ko \
          drivers/net/can/flexcan/flexcan.ko; do
    [ -e "$KBUILD95/$ko" ] || skip "missing CAN module $ko"
done

WORK=$(mktemp -d); R95=$WORK/resp95.log; MCXLOG=$WORK/mcx.log
P95=; PMCX=
trap 'rm -rf "$WORK"; kill ${P95:-} ${PMCX:-} 2>/dev/null' EXIT

"$CROSS" -O2 -static -o "$WORK/canlink" "$HERE/canlink.c" || { echo "FAIL build"; exit 1; }
"$DTC95" -@ -I dts -O dtb -o "$WORK/ov.dtbo" "$FLEX/flexcan-overlay.dtso" 2>/dev/null
"$FDT95" -i "$DTB95" -o "$WORK/can.dtb" "$WORK/ov.dtbo" || { echo "FAIL fdtoverlay"; exit 1; }

root=$WORK/root; mkdir -p "$root"/{mod,proc,sys,dev}
( cd "$root" && zcat "$CPIO95" | cpio -idmu --quiet 2>/dev/null )
cp "$WORK/canlink" "$root/canlink"
for ko in net/can/can.ko drivers/net/can/dev/can-dev.ko net/can/can-raw.ko \
          drivers/net/can/flexcan/flexcan.ko; do
    cp "$KBUILD95/$ko" "$root/mod/$(basename "$ko")"
done
cat > "$root/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc; /bin/busybox mount -t sysfs sys /sys 2>/dev/null; /bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null; exec >/dev/console 2>&1
for m in can can-dev can-raw flexcan; do insmod /mod/$m.ko 2>/dev/null; done
n=0; while [ ! -e /sys/class/net/can0 ] && [ $n -lt 30 ]; do sleep 1; n=$((n+1)); done
[ -e /sys/class/net/can0 ] || { echo "CANLINK:FAIL:no can0"; busybox poweroff -f; }
echo "=== XCAN respond on can0 ==="; /canlink respond can0
echo "=== XCAN-DONE ==="; /bin/busybox poweroff -f
INIT
chmod +x "$root/init"; ( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/ird.gz"

echo "== booting i.MX95 RESPONDER (Linux FlexCAN can0, TCP server :$PORT) =="
timeout -k 5 "$TMO" "$QEMU95" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMG95" -dtb "$WORK/can.dtb" -initrd "$WORK/ird.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM95",cpu-num=6 \
  -object can-bus,id=cb -machine canbus0=cb,canbus1=cb \
  -chardev socket,id=canl,host=127.0.0.1,port=$PORT,server=on,wait=off \
  -object can-host-chardev,id=canh,canbus=cb,chardev=canl \
  -serial file:"$R95" -serial null -monitor none >/dev/null 2>&1 &
P95=$!

echo "   waiting for the 95 responder..."
for i in $(seq 1 90); do grep -qa 'CANLINK:RESPOND ready' "$R95" 2>/dev/null && break; sleep 1; done
grep -qa 'CANLINK:RESPOND ready' "$R95" 2>/dev/null && echo "   95 ready" || echo "   (not seen yet; starting MCX anyway)"

echo "== booting MCXN947 (bare-metal FlexCAN0, TCP client -> :$PORT) =="
# Fleet-standard wiring: the frdm-mcxn947 board is a custom machine type that
# takes -machine canbus0=cb, the same incantation as imx91/93/95 (mcxn947qemu
# 2ee08e14d5). No MCX special-casing.
timeout -k 5 90 "$QEMU_MCX" -M frdm-mcxn947 -display none -monitor none \
  -object can-bus,id=cb -machine canbus0=cb \
  -chardev socket,id=canl,host=127.0.0.1,port=$PORT,server=off,reconnect-ms=1000 \
  -object can-host-chardev,id=h0,canbus=cb,chardev=canl \
  -kernel "$MCX_ELF" -no-reboot -serial file:"$MCXLOG" >/dev/null 2>&1 &
PMCX=$!

for i in $(seq 1 90); do
    grep -qaE 'CAN LINK (PASS|FAIL)' "$MCXLOG" 2>/dev/null &&
        grep -qaE 'CANLINK:(PASS|FAIL)' "$R95" 2>/dev/null && break
    sleep 1
done
kill $PMCX $P95 2>/dev/null; wait 2>/dev/null

echo "============== 95<->MCX CAN CROSS-CHECK =============="
echo "  MCX (bare-metal M33): $(grep -aE 'CAN LINK' "$MCXLOG" 2>/dev/null | tr -d '\r' | tail -1)"
echo "  95  (Linux FlexCAN):  $(grep -aE 'CANLINK:(PASS|FAIL)' "$R95" 2>/dev/null | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' | tail -1)"
if grep -qa 'CAN LINK PASS' "$MCXLOG" 2>/dev/null && grep -qa 'CANLINK:PASS' "$R95" 2>/dev/null; then
    echo "RESULT: PASS (i.MX95 FlexCAN <-> MCXN947 FlexCAN byte-exact both ways over one can-host-chardev)"; exit 0
fi
echo "RESULT: FAIL"; exit 1
