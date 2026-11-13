#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Linux SMP on top of the vendor's EL3 firmware, the whole way up:
#
#   emulated boot ROM -> SM on the M33 -> U-Boot SPL -> TF-A BL31 -> OP-TEE
#     -> U-Boot as BL33 -> booti -> Linux -> userspace, all six A55s online
#
# tests/uboot/run-tfa.sh takes that chain as far as the U-Boot prompt. This one
# carries on into Linux, and the point of it is the secondaries: with BL31 in
# the image QEMU's PSCI emulation is off, so a PSCI CPU_ON from Linux is
# answered by BL31, which asks the System Manager over its secure SCMI channel,
# which programs CA55_RVBARADDR<n> and clears that core's CA55_CPUWAIT bit. The
# machine turns that into a reset of the core at the vector it was given. None
# of that is QEMU's PSCI doing the work, so "6 processors activated" here is a
# statement about the firmware path, not about us.
#
# PASS = Linux reaches userspace AND every A55 came online AND OP-TEE's driver
# bound (it is the BL32 in this chain, so its absence would mean the secure
# payload never initialised).
#
# BL31 and OP-TEE are NXP BSP artifacts and are not redistributable, so they
# are operator-supplied and this test skips without them. Both can be taken out
# of a BSP imx-boot/flash_all image - it is a container set, and
# tests/uboot/mkbootimg.py documents the format.
#
# Required (override via env): QEMU, SM_BIN, SPL_BIN, UBOOT_BIN, UBOOT_ELF,
# BL31_BIN, OPTEE_BIN, IMAGE, DTB, INITRD.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
SM_BIN=${SM_BIN:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.bin}
SPL_BIN=${SPL_BIN:-$ROOT/tests/spl-banner/uboot-build/spl/u-boot-spl.bin}
UBOOT_BIN=${UBOOT_BIN:-$ROOT/tests/spl-banner/uboot-build/u-boot.bin}
UBOOT_ELF=${UBOOT_ELF:-$ROOT/tests/spl-banner/uboot-build/u-boot}
BL31_BIN=${BL31_BIN:-}
OPTEE_BIN=${OPTEE_BIN:-}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
IMAGE=${IMAGE:-$KBUILD/arch/arm64/boot/Image}
DTB=${DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
INITRD=${INITRD:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
NCPU=${NCPU:-6}
TMO=${TMO:-480}
KMEM=${KMEM:-1600M}

skip() { echo "SKIP: $*"; exit 0; }
need() { [ -n "$2" ] || skip "set $1= (operator-supplied BSP artifact)"
         [ -e "$2" ] || skip "missing $1: $2"; }
need QEMU "$QEMU"; need SM_BIN "$SM_BIN"
need SPL_BIN "$SPL_BIN"; need UBOOT_BIN "$UBOOT_BIN"
need UBOOT_ELF "$UBOOT_ELF"
need BL31_BIN "$BL31_BIN"; need OPTEE_BIN "$OPTEE_BIN"
need IMAGE "$IMAGE"; need DTB "$DTB"; need INITRD "$INITRD"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG=${LOG:-$WORK/serial.log}
TFTPDIR="$WORK/tftp"; mkdir -p "$TFTPDIR"

cp "$IMAGE" "$TFTPDIR/Image"; cp "$DTB" "$TFTPDIR/imx95.dtb"
rr="$WORK/rr"; mkdir -p "$rr"
zcat "$INITRD" | (cd "$rr" && cpio -idmu 2>/dev/null)
cat > "$rr/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc 2>/dev/null
/bin/busybox mount -t sysfs sysfs /sys 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "TFA-SMP-ALIVE uname=$(uname -sm)"
echo "TFA-SMP-ONLINE: $(cat /sys/devices/system/cpu/online)"
echo "TFA-SMP-COUNT: $(/bin/busybox grep -c ^processor /proc/cpuinfo)"
echo "TFA-SMP-DONE"
/bin/busybox poweroff -f
INIT
chmod +x "$rr/init"
RD=$TFTPDIR/rootfs.cpio.gz
( cd "$rr" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$RD"
RD_SZ=$(printf '%x' "$(stat -c%s "$RD")")
KVER=$(strings "$IMAGE" | grep -m1 -oE 'Linux version [0-9][^ ]*' |
       awk '{print $3}')

# The whole image set: SM + SPL in set 0, then BL31 (image 0, so SPL enters
# it), U-Boot and OP-TEE in set 1. No -device loader anywhere.
python3 "$HERE/mkbootimg.py" "$UBOOT_BIN" "$WORK/boot.img" \
    --sm "$SM_BIN" --spl "$SPL_BIN" \
    --bl31 "$BL31_BIN" --optee "$OPTEE_BIN" \
    --uboot-elf "$UBOOT_ELF" >/dev/null

BOOTARGS="earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200"
BOOTARGS="$BOOTARGS cpuidle.off=1 rdinit=/init mem=$KMEM"

drive() {
    for _ in $(seq 1 40); do printf '\n'; sleep 0.25; done
    sleep 2
    printf 'setenv ipaddr 10.0.2.15\n';            sleep 1
    printf 'setenv serverip 10.0.2.2\n';           sleep 1
    printf 'setenv ethaddr 00:04:9f:06:11:22\n';   sleep 1
    printf 'setenv bootargs %s\n' "$BOOTARGS";    sleep 1
    printf 'tftpboot 0x93000000 imx95.dtb\n';      sleep 6
    printf 'tftpboot 0x94000000 rootfs.cpio.gz\n'; sleep 8
    printf 'tftpboot 0x90400000 Image\n';          sleep 45
    printf 'booti 0x90400000 0x94000000:%s 0x93000000\n' "$RD_SZ"; sleep 60
}

drive | timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk,el3-firmware=on \
    -m 2G -display none \
    -drive if=none,format=raw,file="$WORK/boot.img",id=mmc0 \
    -device emmc,drive=mmc0 \
    -nic user,model=fsl-enetc,tftp="$TFTPDIR" \
    -serial stdio -serial null 2>&1 |
    sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' > "$LOG" || true

echo "=== chain report ==="
grep -aE "^U-Boot|BL31:|psci: PSCIv|Booting Linux|smp: Brought|SMP: Total" \
    "$LOG" | head -10
grep -aE "optee: revision|TFA-SMP" "$LOG" | head -6

pass=1
check() {
    if grep -qa "$1" "$LOG"; then echo "  ok   $2"
    else echo "  MISS $2"; pass=0; fi
}
check 'NOTICE:  BL31:'          "TF-A BL31 runs at EL3"
check '^U-Boot 20'              "U-Boot entered as BL33"
# Only that Linux found a PSCI implementation at all - the line reads the same
# whoever answers, so it is the BL31 banner above plus el3-firmware=on that
# say who did. Version-agnostic: a different BL31 may report a different one.
check 'psci: PSCIv'             "Linux probed PSCI"
check 'Booting Linux on physical CPU' "booti started the kernel"
check 'optee: revision'         "OP-TEE driver bound (BL32 is live)"
check 'TFA-SMP-ALIVE'           "Linux reached userspace"
check "SMP: Total of $NCPU processors activated" "all $NCPU A55s online"
check "TFA-SMP-COUNT: $NCPU"    "userspace sees $NCPU CPUs"
if [ -n "$KVER" ]; then
    check "Linux version $KVER" "running the kernel we served ($KVER)"
fi

for a in 'failed to come online' 'Kernel panic' 'Unable to handle' \
         'Synchronous Abort' 'Internal error'; do
    grep -qa "$a" "$LOG" && { echo "  MISS anomaly: $a"; pass=0; }
done

[ "$pass" = 1 ] && {
    echo "PASS: ROM -> SM -> SPL -> BL31 -> OP-TEE -> U-Boot -> Linux SMP," \
         "$NCPU CPUs"
    exit 0
}
echo "FAIL"; exit 1
