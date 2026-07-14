#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Silvaco I3C master bring-up (i3c1@44330000, i3c2@42520000).
#
# Both I3C nodes are status=disabled on the 19x19 EVK, so this test patches the
# dtb to enable them (Kyle's directive: model every SoC peripheral, EVK pin-out
# notwithstanding) and declares a legacy-I2C tmp105 child under i3c2 - the model
# attaches a matching tmp105 at 0x48 on i3c2's built-in I2C bus.
#
# The svc-i3c-master driver brings each bus up, runs DAA (finds no I3C targets),
# and registers an I2C adapter for the declared legacy device. A tiny static
# helper then does a SMBus write/read round-trip to the tmp105 at 0x48 over that
# adapter (i2c-dev): a successful round-trip means a real transfer traversed the
# I3C master (MCTRL START/addr -> MWDATAB -> MRDATAB), proving the whole I3C
# datapath end to end - not just that the master registered. (This kernel has no
# lm75 hwmon driver, so userspace i2c-dev is the readback path.)
#
# Required (override via env): QEMU, KBUILD (Image + dtb + dtc), SM_ELF, an
# aarch64 cross-gcc (CROSS=aarch64-linux-gnu-). SKIPs if any is missing.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=${IMAGE:-$KBUILD/arch/arm64/boot/Image}
DTB=${DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
DTC=${DTC:-$KBUILD/scripts/dtc/dtc}
INITRD=${INITRD:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
CROSS=${CROSS:-aarch64-linux-gnu-}
TMO=${TMO:-120}

skip() { echo "SKIP: $*"; exit 0; }
[ -x "$QEMU" ]   || skip "no QEMU ($QEMU)"
[ -e "$IMAGE" ]  || skip "no kernel Image ($IMAGE)"
[ -e "$DTB" ]    || skip "no dtb ($DTB)"
[ -e "$DTC" ]    || skip "no dtc ($DTC)"
[ -e "$SM_ELF" ] || skip "no SM firmware ($SM_ELF)"
[ -e "$INITRD" ] || skip "no busybox initramfs ($INITRD)"
command -v "${CROSS}gcc" >/dev/null || skip "no aarch64 cross-gcc (${CROSS}gcc)"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG="$WORK/serial.log"

# Enable both i3c nodes (status disabled -> okay) and add a legacy-I2C tmp105
# child under i3c2 so the lm75 driver has a target to read.
"$DTC" -I dtb -O dts "$DTB" > "$WORK/evk.dts" 2>/dev/null
python3 - "$WORK/evk.dts" "$WORK/patched.dts" <<'PY'
import sys, re
t = open(sys.argv[1]).read()

def enable(node):
    global t
    i = t.index(node)
    # find this node's body { ... }; flip its own status to okay
    b = t.index('{', i)
    depth, j = 0, b
    while True:
        c = t[j]
        if c == '{': depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0: break
        j += 1
    body = t[b:j]
    body = re.sub(r'status\s*=\s*"disabled"\s*;', 'status = "okay";', body)
    t = t[:b] + body + t[j:]

enable('i3c@44330000')
enable('i3c@42520000')

# Inject a tmp105 (lm75-compatible) legacy-I2C child into i3c2, at the END of
# its body (DTS requires properties precede subnodes). The i3c node has
# #address-cells=3, so reg is <addr 0 i2c-flags>; 0x100 = I2C_FM.
i = t.index('i3c@42520000')
b = t.index('{', i)
depth, j = 0, b
while True:
    c = t[j]
    if c == '{': depth += 1
    elif c == '}':
        depth -= 1
        if depth == 0: break
    j += 1
child = (
    '\t\t\t\ttmp105@48 {\n'
    '\t\t\t\t\tcompatible = "ti,tmp105";\n'
    '\t\t\t\t\treg = <0x48 0x0 0x100>;\n'
    '\t\t\t\t};\n\t\t\t'
)
t = t[:j] + child + t[j:]
open(sys.argv[2], 'w').write(t)
PY
"$DTC" -I dts -O dtb -o "$WORK/patched.dtb" "$WORK/patched.dts" 2>/dev/null \
    || { echo "FAIL: dtc could not rebuild patched dtb"; exit 1; }

STAGE="$WORK/root"; mkdir -p "$STAGE"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
"${CROSS}gcc" -O2 -static -o "$STAGE/i3c_i2c_probe" "$HERE/i3c_i2c_probe.c" \
    || { echo "FAIL: could not build i3c_i2c_probe"; exit 1; }
cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "=== I3C ==="
sleep 4
echo "--- i3c masters (dmesg) ---"
dmesg | grep -aiE 'i3c|svc-i3c|silvaco' | head
echo "--- i3c sysfs devices ---"
ls /sys/bus/i3c/devices 2>/dev/null
echo "--- i2c adapters ---"
for a in /sys/bus/i2c/devices/i2c-*; do
    [ -e "$a/name" ] && echo "$(basename "$a"): $(cat "$a/name")"
done
echo "--- tmp105 round-trip over i3c2 legacy-i2c ---"
/i3c_i2c_probe
echo "=== I3C-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$WORK/patched.dtb" -initrd "$WORK/initrd.cpio.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "--- i3c report ---"
sed -n '/=== I3C ===/,/=== I3C-DONE ===/p' "$LOG"

fail() { echo "FAIL: $*"; exit 1; }
# Both Silvaco masters must register: the i3c core only creates
# /sys/bus/i3c/devices/i3c-N once the driver drove MCONFIG/reset/DAA through
# the model successfully, so two i3c-N nodes is the bring-up proof.
n_i3c=$(grep -aoE 'i3c-[0-9]+' "$LOG" | sort -u | wc -l)
[ "$n_i3c" -ge 2 ] || fail "expected 2 i3c masters in sysfs, saw $n_i3c"
# End-to-end legacy-I2C transfer proof.
grep -qa 'I3C_I2C_OK' "$LOG" \
    || fail "tmp105 SMBus round-trip did not complete over the i3c legacy-i2c path"
grep -qaiE 'external abort|Unhandled fault|Internal error' "$LOG" \
    && fail "fault during i3c bring-up"

echo "PASS: 2 i3c masters up; tmp105 write/read round-trip over i3c2 legacy-i2c"
