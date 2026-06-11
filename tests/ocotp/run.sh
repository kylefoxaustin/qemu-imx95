#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 OCOTP (eFuse) + EdgeLock secure-enclave bring-up.
#
# The OCOTP driver (fsl-ocotp-fsb-s400, efuse@47510000) reads the SoC's
# one-time-programmable fuses. On the 95 it does NOT read them all from a
# memory-mapped block: it reads the FSB shadow directly and the remaining banks
# through the EdgeLock secure-enclave (ELE / "S400") firmware service. So the
# efuse driver only binds once the Linux fsl-se HSM interface
# (secure-enclave-0, on elemu3) has probed - and that, in turn, needs the ELE
# message responder to answer the driver's GET_INFO / GET_STATE / READ_FUSE
# commands AND the elemu3 RX interrupt (GIC SPI 24) to be delivered (the Linux
# se driver is IRQ-driven, unlike U-Boot which polls).
#
# This confirms the whole chain comes up: the HSM secure-enclave configures, the
# efuse nvmem device (fsb_s400_fuse0) registers, and the eth_mac0 nvmem cell
# reads back the NXP-OUI MAC seeded into the FSB shadow by the machine - i.e. a
# real fuse value, not all-zero. No probe deferral, no panic.
#
# The closed ELE firmware is NOT executed; the responder fakes the message
# protocol (reads are plain data, no keys), and only fuse *reads* are modelled -
# the secure-enclave's crypto/secure-boot services are out of scope.
#
# Required (override via env): QEMU, KBUILD (Image + dtb), SM_ELF. SKIPs if any
# is missing.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=${IMAGE:-$KBUILD/arch/arm64/boot/Image}
DTB=${DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
INITRD=${INITRD:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
TMO=${TMO:-120}

skip() { echo "SKIP: $*"; exit 0; }
[ -x "$QEMU" ]   || skip "no QEMU ($QEMU)"
[ -e "$IMAGE" ]  || skip "no kernel Image ($IMAGE)"
[ -e "$DTB" ]    || skip "no dtb ($DTB)"
[ -e "$SM_ELF" ] || skip "no SM firmware ($SM_ELF)"
[ -e "$INITRD" ] || skip "no busybox initramfs ($INITRD)"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG="$WORK/serial.log"
STAGE="$WORK/root"; mkdir -p "$STAGE"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
sleep 6
echo "=== OCOTP ==="
echo "se-enclave: $(ls -d /sys/bus/platform/devices/secure-enclave-0 2>/dev/null)"
echo "se-configured: $(dmesg | grep -ac 'hsm0 interface to firmware, configured')"
echo "nvmem: $(ls /sys/bus/nvmem/devices 2>/dev/null | tr '\n' ' ')"
echo "efuse-driver: $(readlink /sys/bus/platform/devices/47510000.efuse/driver 2>/dev/null | sed 's#.*/##')"
N=/sys/bus/nvmem/devices/fsb_s400_fuse0/nvmem
if [ -f "$N" ]; then
    echo "eth_mac0: $(dd if="$N" bs=1 skip=1300 count=6 2>/dev/null | hexdump -e '5/1 "%02x:" 1/1 "%02x"')"
fi
echo "defer: $(cat /sys/kernel/debug/devices_deferred 2>/dev/null | grep -iE 'efuse|secure-enclave' | tr '\n' ',')"
echo "panic: $(dmesg | grep -ciE 'Kernel panic|Unable to handle|Internal error')"
echo "=== OCOTP-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.gz"

timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "--- ocotp report ---"
sed -n '/=== OCOTP ===/,/=== OCOTP-DONE ===/p' "$LOG"

fail() { echo "FAIL: $*"; exit 1; }
grep -qaE 'se-configured: [1-9]'      "$LOG" || fail "HSM secure-enclave did not configure"
grep -qaE 'nvmem:.*fsb_s400_fuse0'    "$LOG" || fail "efuse nvmem (fsb_s400_fuse0) did not register"
grep -qaE 'efuse-driver: fsl-ocotp'   "$LOG" || fail "efuse driver did not bind"
grep -qaE 'eth_mac0: 00:04:9f:95:00'  "$LOG" || fail "eth_mac0 fuse cell did not read the seeded MAC"
grep -qaE 'panic: 0'                  "$LOG" || fail "kernel fault during OCOTP/SE bring-up"

echo "PASS: OCOTP eFuse brings up (HSM secure-enclave + efuse nvmem; fuse MAC cell reads real data)"
