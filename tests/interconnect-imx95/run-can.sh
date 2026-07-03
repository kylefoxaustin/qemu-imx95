#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# CAN INTERCONNECT (i.MX 95): prove two QEMU i.MX 95 instances pass a real CAN
# frame over a board-to-board CAN link bridged by a QEMU socket - the shape a lab
# coordinator wires for a CAN segment (fleet mission #5). A known frame travels
# guest A's FlexCAN (can0) -> local can-bus -> can-host-chardev -> socket bridge
# -> guest B's can-host-chardev -> local can-bus -> FlexCAN (can0), verified
# byte-exact on the receiver.
#
# Each instance has a FlexCAN (can0) on its own can-bus 'cb', joined to the peer
# by can-host-chardev (net/can/can_host_chardev.c) over a chardev socket - so no
# host-kernel vcan/SocketCAN (root) is needed, unlike can-host-socketcan. The
# stock EVK DT disables CAN, so the flexcan DT overlay enables both flexcan nodes;
# both are wired to 'cb' (canbus0=cb,canbus1=cb) so that whichever the guest
# enumerates as can0 is on the bridged bus. The CAN stack (can/can-dev/can-raw/
# flexcan) loads as modules from the kernel build.
#
# can-host-chardev is the fleet-shared CAN transport (generic, upstream-shaped).
# Required artifacts (env): QEMU, KBUILD (Image + dtb + CAN .ko + dtc/fdtoverlay),
# SM_ELF, an aarch64 cross gcc.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
FLEX="$ROOT/tests/flexcan"
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=$KBUILD/arch/arm64/boot/Image
DTB=$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb
DTC=$KBUILD/scripts/dtc/dtc
FDTOVERLAY=$KBUILD/scripts/dtc/fdtoverlay
DEFAULT_CPIO=${DEFAULT_CPIO:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
CROSS=${CROSS:-aarch64-linux-gnu-gcc}
TMO=${TMO:-220}

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }
for f in "$QEMU" "$IMAGE" "$DTC" "$FDTOVERLAY" "$SM_ELF" "$DEFAULT_CPIO" "$DTB" \
         "$FLEX/flexcan-overlay.dtso"; do
    [ -e "$f" ] || skip "missing $f"
done
command -v "$CROSS" >/dev/null || skip "no cross compiler ($CROSS)"
for ko in net/can/can.ko drivers/net/can/dev/can-dev.ko net/can/can-raw.ko \
          drivers/net/can/flexcan/flexcan.ko; do
    [ -e "$KBUILD/$ko" ] || skip "missing CAN module $ko"
done

SOCK=${SOCK:-$(mktemp -u /tmp/imx95-canlink.XXXXXX.sock)}
WORK=$(mktemp -d); trap 'rm -rf "$WORK" "$SOCK"; kill ${SPID:-} ${CPID:-} 2>/dev/null' EXIT
SPID=; CPID=

# ---- build the SocketCAN oracle (static aarch64) ----------------------------
"$CROSS" -O2 -static -o "$WORK/canlink" "$HERE/canlink.c" || die "canlink build failed"

# ---- CAN-enabled DTB: reuse the flexcan overlay (enables flexcan1 = can0) ----
"$DTC" -@ -I dts -O dtb -o "$WORK/ov.dtbo" "$FLEX/flexcan-overlay.dtso" 2>/dev/null \
    || die "overlay compile failed"
"$FDTOVERLAY" -i "$DTB" -o "$WORK/can.dtb" "$WORK/ov.dtbo" || die "fdtoverlay failed"
DTB2="$WORK/can.dtb"

# ---- stage a role-based initramfs -------------------------------------------
build_initrd() {            # $1=role  -> echoes path
    local role=$1
    local stage="$WORK/$role"
    mkdir -p "$stage/mod"
    zcat "$DEFAULT_CPIO" | (cd "$stage" && cpio -idmu 2>/dev/null)
    install -m755 "$WORK/canlink" "$stage/canlink"
    for ko in net/can/can.ko drivers/net/can/dev/can-dev.ko net/can/can-raw.ko \
              drivers/net/can/flexcan/flexcan.ko; do
        cp "$KBUILD/$ko" "$stage/mod/$(basename "$ko")"
    done
    printf 'ROLE=%s\n' "$role" > "$stage/linkenv"
    cat > "$stage/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys 2>/dev/null
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
. /linkenv
for m in can can-dev can-raw flexcan; do insmod /mod/$m.ko 2>/dev/null; done
n=0; while [ ! -e /sys/class/net/can0 ] && [ $n -lt 30 ]; do sleep 1; n=$((n+1)); done
[ -e /sys/class/net/can0 ] || { echo "CANLINK:FAIL:no can0 (flexcan1 not bound)"; \
                                dmesg | grep -iE 'can|flexcan' | tail -5; busybox poweroff -f; }
echo "=== INTERCONNECT can ($ROLE on can0) ==="
if [ "$ROLE" = recv ]; then
    /canlink recv can0
else
    sleep 8                       # let the receiver bring up + the socket connect
    /canlink send can0 15         # then resend across the receiver's window
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
        -object can-bus,id=cb -machine canbus0=cb,canbus1=cb \
        $2 -object can-host-chardev,id=canh,canbus=cb,chardev=canl \
        -serial file:"$3" -serial null -monitor none >/dev/null 2>&1 &
}

# ---- receiver (socket server) first, then sender (client) -------------------
RLOG="$WORK/recv.log"; SLOG="$WORK/send.log"
echo "== booting i.MX 95 RECEIVER (can-host-chardev socket listen) =="
boot "$RECV_IRD" "-chardev socket,id=canl,path=$SOCK,server=on,wait=off" "$RLOG"; SPID=$!
sleep 3
echo "== booting i.MX 95 SENDER (can-host-chardev socket connect) =="
boot "$SEND_IRD" "-chardev socket,id=canl,path=$SOCK,server=off,reconnect-ms=1000" "$SLOG"; CPID=$!

wait $SPID $CPID 2>/dev/null

echo "================== CAN LINK =================="
grep -aE 'INTERCONNECT|CANLINK:' "$RLOG" "$SLOG" | grep -avE '^\[ *[0-9]+\.[0-9]+\]' | \
    sed 's/\x1b\[[0-9;]*[a-zA-Z]//g; s#.*/##'
if grep -aq 'CANLINK:PASS' "$RLOG"; then
    echo "RESULT: PASS (CAN frame crossed FlexCAN<->can-bus<->can-host-chardev<->socket<->...<->FlexCAN byte-exact between two i.MX 95 guests)"
    exit 0
fi
die "can link did not complete"
