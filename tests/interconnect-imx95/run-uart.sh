#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# UART INTERCONNECT: pass real data over an LPUART between TWO separate QEMU
# i.MX 95 instances (board-farm mission - the holobench lab links boards this
# way). Two independent machines are joined by a QEMU socket chardev on LPUART3
# (ttyLP2): instance A's LPUART3 `-serial socket,server=on` and instance B's
# `-serial socket` (connect). A byte written to B's /dev/ttyLP2 crosses the
# socket into A's LPUART3 RX and vice versa.
#
# LPUART3 is a NON-console tty with DTB `dmas`, so Linux drives its RX through a
# cyclic eDMA-RX channel - exercising the LPUART RX-DMA datapath end to end
# (the model wires LPUART3 dma-req -> edma2; without that, received bytes are
# silently dropped). LPUART1 stays the console; LPUART2 is the SM's own console
# and is left alone.
#
# Proof: a byte-exact text payload transferred client->server over ttyLP2 (the
# server greps the received stream for the exact payload line). PASS only if the
# exact bytes crossed the UART.
#
# Required artifacts (env): QEMU, KBUILD (Image + dtb + scripts/dtc/dtc), SM_ELF.
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
PORT=${PORT:-12500}
TMO=${TMO:-300}
PAYLOAD="IMX95-INTERCONNECT-UART-PAYLOAD-0123456789-abcdef-byte-exact-OK"

skip() { echo "SKIP: $*"; exit 0; }
for f in "$QEMU" "$IMAGE" "$DTC" "$SM_ELF" "$DEFAULT_CPIO" "$DTB"; do
    [ -e "$f" ] || skip "missing $f"
done

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

# --- patched dtb: enable LPUART3 (serial@42570000, DTB serial2 -> ttyLP2) ---
# It ships status="disabled"; flip only that node's status to "okay", keeping its
# stock `dmas` (edma2) so Linux uses the cyclic eDMA-RX path.
"$DTC" -I dtb -O dts "$DTB" >"$WORK/base.dts" 2>/dev/null
awk '/serial@42570000 \{/{inblk=1}
     inblk && /status = "disabled";/{sub(/disabled/,"okay"); inblk=0}
     {print}' "$WORK/base.dts" >"$WORK/uart.dts"
grep -q 'serial@42570000' "$WORK/uart.dts" || skip "LPUART3 node not found in dtb"
"$DTC" -I dts -O dtb -o "$WORK/uart.dtb" "$WORK/uart.dts" 2>/dev/null || skip "dtb recompile failed"

# --- one initramfs, role from the kernel cmdline (ig_role=server|client) ---
root="$WORK/root"; mkdir -p "$root"/{bin,proc,sys,dev}
( cd "$WORK" && zcat "$DEFAULT_CPIO" | cpio -idmu --quiet 'bin/busybox' )
cp "$WORK/bin/busybox" "$root/bin/busybox"
printf '%s' "$PAYLOAD" > "$root/payload"
cat > "$root/init" <<INIT
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs devtmpfs /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
/bin/busybox mkdir -p /tmp
ROLE=\$(sed -n 's/.*ig_role=\\([a-z]*\\).*/\\1/p' /proc/cmdline)
TTY=/dev/ttyLP2
n=0; while [ ! -c \$TTY ] && [ \$n -lt 60 ]; do sleep 1; n=\$((n+1)); done
[ -c \$TTY ] || { echo "ZRESULT FAIL (no \$TTY)"; echo "ZTTY: \$(ls /dev/ttyLP* 2>/dev/null)"; sleep 1; poweroff -f; }
echo "ZTTY present: \$(ls /dev/ttyLP* 2>/dev/null | tr '\n' ' ')"
# raw mode: no line-discipline translation, no echo (byte-exact stream)
stty -F \$TTY 115200 raw -echo 2>/dev/null
echo "ZBOOT role=\$ROLE tty=\$TTY"
want=\$(cat /payload)
if [ "\$ROLE" = server ]; then
    # receive: drain the UART into /tmp/rx, wait for the exact payload line.
    cat \$TTY > /tmp/rx 2>/dev/null &
    m=0; while ! grep -qx "\$want" /tmp/rx 2>/dev/null && [ \$m -lt 90 ]; do sleep 1; m=\$((m+1)); done
    if grep -qx "\$want" /tmp/rx 2>/dev/null; then
        echo "ZRESULT PASS (rx byte-exact over ttyLP2 DMA-RX)"
    else
        echo "ZRESULT FAIL (got=[\$(tr -d '\0' < /tmp/rx | tr '\n' '|')] want=[\$want])"
    fi
else
    # send: repeat the payload line until the server has had time to catch a
    # clean one (the socket link + tty come up asynchronously across instances).
    s=0; while [ \$s -lt 20 ]; do printf '%s\n' "\$want" > \$TTY 2>/dev/null; echo "ZSENT try \$s"; sleep 1; s=\$((s+1)); done
fi
sleep 2; poweroff -f
INIT
chmod +x "$root/init"
( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/ic.cpio.gz"

boot() { # $1=role $2=chardev-spec (id=ul) $3=logfile
    # LPUART3 = serial_hd(2): console -> serial_hd(0), SM's LPUART2 -> null
    # (serial_hd(1)), the socket link -> serial_hd(2). The inline "-serial
    # socket,..." form is not accepted by this QEMU, so use an explicit
    # -chardev + "-serial chardev:ul".
    timeout --signal=KILL "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
        -kernel "$IMAGE" -dtb "$WORK/uart.dtb" -initrd "$WORK/ic.cpio.gz" \
        -append "earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init ig_role=$1" \
        -device loader,file="$SM_ELF",cpu-num=6 \
        -chardev "$2" \
        -serial file:"$3" -serial null -serial chardev:ul -monitor none >/dev/null 2>&1 || true
}

echo "== launching two i.MX 95 instances joined by an LPUART3 socket link (look for ZRESULT) =="
boot server "socket,id=ul,host=127.0.0.1,port=$PORT,server=on,wait=off" "$WORK/server.log" &
SPID=$!
# The client uses reconnect-ms so it rides out the listen-bind race (and any
# peer flap) without an explicit port wait. This relies on the char-socket
# reconnect-abort fix (chardev/char-socket.c): before it, a reconnect-ms client
# whose connect attempt errored against a vanishing peer aborted at teardown.
sleep 1
boot client "socket,id=ul,host=127.0.0.1,port=$PORT,reconnect-ms=1000" "$WORK/client.log" &
CPID=$!
wait "$SPID" "$CPID" 2>/dev/null

echo "--- server ---"; grep -aE 'ZTTY|ZBOOT|ZSENT|ZRESULT' "$WORK/server.log" | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'
echo "--- client ---"; grep -aE 'ZTTY|ZBOOT|ZSENT|ZRESULT' "$WORK/client.log" | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' | tail -4
if grep -aq 'ZRESULT PASS' "$WORK/server.log"; then
    echo "RESULT: PASS (byte-exact payload crossed the two-instance LPUART3 socket link, server RX via eDMA)"
    exit 0
fi
echo "RESULT: FAIL - see logs"
cp "$WORK/server.log" /tmp/ic-uart-server.log 2>/dev/null
cp "$WORK/client.log" /tmp/ic-uart-client.log 2>/dev/null
exit 1
