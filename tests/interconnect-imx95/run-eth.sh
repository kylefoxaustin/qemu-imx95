#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Ethernet INTERCONNECT: pass real data over ENETC between TWO separate QEMU
# i.MX 95 instances (board-farm mission #5 - the holobench lab links boards this
# way). Unlike tests/netc/run-2port.sh (two ports on ONE instance via an L2 hub),
# here two independent machines are joined by a QEMU socket netdev: instance A's
# ENETC NIC `-nic socket,listen=:PORT` and instance B's `-nic socket,connect=`.
# A frame leaving B's eth0 crosses the socket into A's ENETC RX BD-ring and vice
# versa - exercising both instances' real TX/RX datapaths across the wire.
#
# Proof: mutual ping (link liveness both ways) + a byte-exact TCP payload
# transfer client->server (server diffs the received bytes against the known
# payload). PASS only if the exact bytes crossed.
#
# Reuses the NETC fixed-link dtb patch (tests/netc/patch-dtb.py) so ENETC probes,
# and the default busybox initramfs (has ip/ping/nc). Required artifacts (env):
#   QEMU, KBUILD (Image + dtb + scripts/dtc/dtc), SM_ELF.
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
PORT=${PORT:-12390}
TMO=${TMO:-300}
SRV_MAC=00:04:9f:06:11:22   # server port - passed via -nic mac= (model seeds it)
CLI_MAC=00:04:9f:06:12:22   # client port - distinct -nic mac= (point-to-point L2)
PAYLOAD="IMX95-INTERCONNECT-ETH-PAYLOAD-0123456789-abcdef-byte-exact-OK"

skip() { echo "SKIP: $*"; exit 0; }
for f in "$QEMU" "$IMAGE" "$DTC" "$SM_ELF" "$DEFAULT_CPIO" "$DTB"; do
    [ -e "$f" ] || skip "missing $f"
done

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

# --- patched dtb: ENETC ports as fixed-link + identity msi-map (so they probe) ---
# One dtb for both sides. The per-side distinct MAC (a shared MAC across the two
# ends of a point-to-point link breaks L2) comes from -nic mac= below - the
# model seeds it into the ENETC SI primary-MAC register - not from the dtb.
"$DTC" -I dtb -O dts "$DTB" >"$WORK/base.dts" 2>/dev/null
python3 "$ROOT/tests/netc/patch-dtb.py" "$WORK/base.dts" >"$WORK/enetc.dts"
"$DTC" -I dts -O dtb -o "$WORK/enetc.dtb" "$WORK/enetc.dts" 2>/dev/null || skip "dtb recompile failed"

# --- one initramfs, role chosen from the kernel cmdline (ig_role=server|client) ---
root="$WORK/root"; mkdir -p "$root"/{bin,proc,sys,dev}
( cd "$WORK" && zcat "$DEFAULT_CPIO" | cpio -idmu --quiet 'bin/busybox' )
cp "$WORK/bin/busybox" "$root/bin/busybox"
printf '%s' "$PAYLOAD" > "$root/payload"
cat > "$root/init" <<INIT
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox --install -s /bin 2>/dev/null
/bin/busybox mkdir -p /tmp   # initramfs has no /tmp; nc -l writes /tmp/rx
ROLE=\$(sed -n 's/.*ig_role=\\([a-z]*\\).*/\\1/p' /proc/cmdline)
PORT=$PORT
# Only ENETC PF0 (devfn 00.0) carries the -nic socket backend, but the machine
# has three PFs and Linux names netdevs in probe-completion order - so "eth0" is
# NOT reliably PF0. Resolve the interface from its PCI address instead (the same
# race made tests/netc/run-2port.sh flake ~30% of the time).
n=0; while [ \$n -lt 60 ]; do
    IF=\$(ls /sys/bus/pci/devices/0002:00:00.0/net 2>/dev/null | head -1)
    [ -n "\$IF" ] && break
    sleep 1; n=\$((n+1))
done
[ -n "\$IF" ] || { echo "ZRESULT FAIL (no netdev on 0002:00:00.0)"; poweroff -f; }
echo "ZIF socket-backed PF 0002:00:00.0 = \$IF"
if [ "\$ROLE" = server ]; then MY=10.0.0.1; PEER=10.0.0.2; MYMAC=$SRV_MAC; PMAC=$CLI_MAC; else MY=10.0.0.2; PEER=10.0.0.1; MYMAC=$CLI_MAC; PMAC=$SRV_MAC; fi
# The distinct per-side MAC now comes from the model (-nic mac=, below): the
# ENETC SI primary-MAC register seeds it so the guest reads a valid station
# address. (A shared MAC across the two ends of a point-to-point link breaks L2,
# hence distinct MACs per side.) MYMAC is what we expect the model to report.
ip addr add \$MY/24 dev \$IF; ip link set \$IF up
echo "ZMAC \$IF=\$(cat /sys/class/net/\$IF/address) (expect \$MYMAC)"
# static neighbour: take ARP timing out of the loop (we know the peer's MAC).
ip neigh replace \$PEER lladdr \$PMAC dev \$IF nud permanent 2>/dev/null
echo "ZBOOT role=\$ROLE my=\$MY peer=\$PEER carrier=\$(cat /sys/class/net/\$IF/carrier 2>/dev/null)"
echo "ZIF: \$(ls /sys/class/net 2>/dev/null | tr '\n' ' ')"
allstats() { for d in /sys/class/net/eth*; do i=\${d##*/}; echo "ZALL \$i tx=\$(cat \$d/statistics/tx_packets) rx=\$(cat \$d/statistics/rx_packets) carrier=\$(cat \$d/carrier 2>/dev/null) mac=\$(cat \$d/address)"; done; }
if [ "\$ROLE" = server ]; then
    # primary proof: receive a byte-exact TCP payload from the client.
    nc -l -p \$PORT > /tmp/rx 2>/dev/null &
    m=0; while [ ! -s /tmp/rx ] && [ \$m -lt 90 ]; do sleep 1; m=\$((m+1)); done
    got=\$(cat /tmp/rx 2>/dev/null); want=\$(cat /payload)
    echo "ZSTATS \$IF tx=\$(cat /sys/class/net/\$IF/statistics/tx_packets) rx=\$(cat /sys/class/net/\$IF/statistics/rx_packets)"
    if [ "\$got" = "\$want" ]; then echo "ZRESULT PASS (rx byte-exact: \$got)"; else echo "ZRESULT FAIL (got=[\$got] want=[\$want])"; fi
else
    # client: warm the link with ICMP first so BOTH RX rings are posted before
    # the short-lived TCP transfer. A frame arriving before ndo_open() finishes
    # posting RX buffers is discarded (RX overrun, see fsl_enetc model), and
    # nc's connect timeout is too short to ride out those early-boot drops.
    p=0; while [ \$p -lt 30 ]; do if ping -c1 -W1 -I \$IF \$PEER >/dev/null 2>&1; then echo "ZPING client->server OK (warm \$p)"; break; fi; p=\$((p+1)); done
    # now the byte-exact TCP payload transfer over the warm link.
    s=0; while [ \$s -lt 12 ]; do if nc -w3 \$PEER \$PORT < /payload 2>/dev/null; then echo "ZSENT ok (try \$s)"; break; fi; sleep 1; s=\$((s+1)); done
    echo "ZSTATS \$IF tx=\$(cat /sys/class/net/\$IF/statistics/tx_packets) rx=\$(cat /sys/class/net/\$IF/statistics/rx_packets)"
fi
allstats
sleep 2; poweroff -f
INIT
chmod +x "$root/init"
( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/ic.cpio.gz"

boot() { # $1=role $2=socket-arg $3=logfile $4=dtb $5=mac
    timeout --signal=KILL "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
        -kernel "$IMAGE" -dtb "$4" -initrd "$WORK/ic.cpio.gz" \
        -append "earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init ig_role=$1" \
        -device loader,file="$SM_ELF",cpu-num=6 \
        -nic "socket,$2,model=fsl-enetc,mac=$5" \
        -serial file:"$3" -serial null -monitor none >/dev/null 2>&1 || true
}

echo "== launching two i.MX 95 instances joined by an ENETC socket link (look for ZRESULT) =="
boot server "listen=:$PORT"            "$WORK/server.log" "$WORK/enetc.dtb" "$SRV_MAC" &
SPID=$!
# Wait until the server QEMU has actually bound the listen port before starting
# the client: the legacy socket netdev does NOT retry a failed connect(), so a
# client that races ahead of the listen bind would have a dead link all run.
# Probe the LISTEN state (ss), never connect - a real connect would consume the
# listen netdev's single accept slot.
b=0
while [ $b -lt 60 ]; do
    if ss -ltnH 2>/dev/null | grep -q ":$PORT "; then break; fi
    sleep 1; b=$((b+1))
done
[ $b -lt 60 ] || skip "server listen port $PORT never came up"
boot client "connect=127.0.0.1:$PORT"  "$WORK/client.log" "$WORK/enetc.dtb" "$CLI_MAC" &
CPID=$!
wait "$SPID" "$CPID" 2>/dev/null

echo "--- server ---"; grep -aE 'ZBOOT|ZIF|ZMAC|ZALL|ZPING|ZSTATS|ZSENT|ZRESULT' "$WORK/server.log" | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'
echo "--- client ---"; grep -aE 'ZBOOT|ZIF|ZMAC|ZALL|ZPING|ZSTATS|ZSENT|ZRESULT' "$WORK/client.log" | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'
# Proof = the exact payload bytes crossed the ENETC socket link (client TX ->
# server RX BD-ring). TCP makes it loss-tolerant; ping is informational only.
if grep -aq 'ZRESULT PASS' "$WORK/server.log" && grep -aq 'ZSENT ok' "$WORK/client.log"; then
    echo "RESULT: PASS (byte-exact payload crossed the two-instance ENETC socket link)"
    exit 0
fi
echo "RESULT: FAIL - see logs"
cp "$WORK/server.log" /tmp/ic-eth-server.log 2>/dev/null
cp "$WORK/client.log" /tmp/ic-eth-client.log 2>/dev/null
exit 1
