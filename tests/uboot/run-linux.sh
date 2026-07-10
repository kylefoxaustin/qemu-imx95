#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 U-Boot, all the way: boot off eMMC, prove the eMMC write path, then
# TFTP a kernel + dtb + initramfs and booti to a Linux userspace shell.
#
# This is the full "board stand-in" story:
#   ROM tables -> SM -> SPL -> U-Boot proper (loaded from eMMC)
#     -> mmc write/read round-trip (a scratch sector, byte-verified)
#     -> tftp kernel/dtb/rootfs over ENETC
#     -> booti -> Linux mounts the initramfs and runs /init to a marker
#
# PASS = the eMMC write is read back byte-identical AND Linux prints its
#        boot marker with the kernel version we served.
#
# Two knobs that are U-Boot-image quirks, not machine bugs, and are documented
# where they are used:
#   - mkbootimg --no-optee: the vendor U-Boot fabricates an optee reserved-
#     memory node from rom_pointer[], which SPL only fills when it loads OP-TEE
#     as BL32. This machine models no OP-TEE, so the fixup is neutralised (what
#     CONFIG_OPTEE=n does).
#   - mem= in bootargs: this U-Boot build asks the SM for the DRAM size over
#     SCMI; the prebuilt SM does not answer (it would, on silicon, from real DDR
#     training), so U-Boot falls back to a compile-time size far larger than the
#     RAM the machine maps. mem= tells the kernel the real size - exactly what a
#     developer does when firmware misreports memory.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
SPL_BIN=${SPL_BIN:-$ROOT/tests/spl-banner/uboot-build/spl/u-boot-spl.bin}
UBOOT_BIN=${UBOOT_BIN:-$ROOT/tests/spl-banner/uboot-build/u-boot.bin}
UBOOT_ELF=${UBOOT_ELF:-$ROOT/tests/spl-banner/uboot-build/u-boot}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
IMAGE=${IMAGE:-$KBUILD/arch/arm64/boot/Image}
DTB=${DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
INITRD=${INITRD:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
TMO=${TMO:-360}
MEM=${MEM:-2G}
KMEM=${KMEM:-1600M}

skip() { echo "SKIP: $*"; exit 0; }
need() { [ -e "$2" ] || skip "missing $1: $2"; }
need QEMU "$QEMU"; need SM_ELF "$SM_ELF"; need SPL_BIN "$SPL_BIN"
need UBOOT_BIN "$UBOOT_BIN"; need UBOOT_ELF "$UBOOT_ELF"
need IMAGE "$IMAGE"; need DTB "$DTB"; need INITRD "$INITRD"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG=${LOG:-$WORK/serial.log}
TFTPDIR="$WORK/tftp"; mkdir -p "$TFTPDIR"

# --- serve kernel + dtb + a marker initramfs ---
cp "$IMAGE" "$TFTPDIR/Image"
cp "$DTB" "$TFTPDIR/imx95.dtb"
rr="$WORK/rr"; mkdir -p "$rr"
zcat "$INITRD" | (cd "$rr" && cpio -idmu 2>/dev/null)
cat > "$rr/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc 2>/dev/null
/bin/busybox mount -t sysfs sysfs /sys 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "UBOOT-BOOTI-LINUX-ALIVE uname=$(uname -sm)"
echo "UBOOT-BOOTI-DONE"
/bin/busybox poweroff -f
INIT
chmod +x "$rr/init"
( cd "$rr" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$TFTPDIR/rootfs.cpio.gz"
RD_SZ=$(printf '%x' "$(stat -c%s "$TFTPDIR/rootfs.cpio.gz")")

KVER=$(strings "$IMAGE" | grep -m1 -oE 'Linux version [0-9][^ ]*' | awk '{print $3}')

# --- boot media (with the optee fixup neutralised) ---
python3 "$HERE/mkbootimg.py" "$UBOOT_BIN" "$WORK/boot.img" \
    --uboot-elf "$UBOOT_ELF" --no-optee >/dev/null


drive() {
    for _ in $(seq 1 30); do printf '\n'; sleep 0.25; done
    sleep 2
    # --- eMMC write path: fill a page with a recognisable pattern, write it
    #     to a scratch sector, clobber the source, read the sector back, and
    #     dump it. If the pattern survives the round-trip, write+read work. ---
    printf 'mw.l 0x92000000 0xb0075a5a 0x80\n';            sleep 1
    printf 'mmc dev 0\n';                                  sleep 1
    printf 'mmc write 0x92000000 0x3000 1\n';              sleep 2
    printf 'mw.l 0x92000000 0xdeadc0de 0x80\n';            sleep 1
    printf 'mmc read 0x92000000 0x3000 1\n';               sleep 2
    printf 'echo EMMC-READBACK: && md.l 0x92000000 4\n';   sleep 2
    # --- network kernel boot ---
    printf 'setenv ipaddr 10.0.2.15\n';                    sleep 1
    printf 'setenv serverip 10.0.2.2\n';                   sleep 1
    printf 'setenv ethaddr 00:04:9f:06:11:22\n';           sleep 1
    printf 'setenv bootargs earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init mem=%s\n' "$KMEM"; sleep 1
    printf 'tftpboot 0x93000000 imx95.dtb\n';              sleep 6
    printf 'tftpboot 0x94000000 rootfs.cpio.gz\n';         sleep 8
    printf 'tftpboot 0x90400000 Image\n';                  sleep 45
    printf 'booti 0x90400000 0x94000000:%s 0x93000000\n' "$RD_SZ"; sleep 90
}

drive | timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m "$MEM" -display none \
    -device loader,file="$SPL_BIN",addr=0x20480000,cpu-num=0,force-raw=on \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -drive if=none,format=raw,file="$WORK/boot.img",id=mmc0 -device emmc,drive=mmc0 \
    -nic user,model=fsl-enetc,tftp="$TFTPDIR" \
    -serial stdio -serial null 2>&1 | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' > "$LOG" || true

echo "=== report ==="
grep -aE "^U-Boot |Trying to boot|EMMC-READBACK|Booting Linux|Run /init|UBOOT-BOOTI" "$LOG" | head -12

pass=1
grep -qaE '^U-Boot 20' "$LOG"           && echo "  ok   U-Boot proper (from eMMC)" || { echo "  MISS U-Boot"; pass=0; }
# eMMC write path: the 0xb0075a5a pattern we wrote to the sector reappears in
# the read-back dump, even though the source page was clobbered in between.
if sed -n '/EMMC-READBACK:/,/^u-boot=>/p' "$LOG" | grep -qai 'b0075a5a'; then
    echo "  ok   eMMC write -> read round-trip (scratch sector verified)"
else
    echo "  MISS eMMC write/read (pattern b0075a5a not read back)"; pass=0
fi
grep -qa 'Booting Linux on physical CPU' "$LOG" && echo "  ok   booti started the kernel" || { echo "  MISS kernel start"; pass=0; }
grep -qa 'UBOOT-BOOTI-LINUX-ALIVE' "$LOG" && echo "  ok   Linux reached userspace (/init ran)" || { echo "  MISS userspace"; pass=0; }
if [ -n "$KVER" ]; then
    grep -qa "Linux version $KVER" "$LOG" && echo "  ok   running the kernel we served ($KVER)" || { echo "  MISS kernel version"; pass=0; }
fi
for a in 'Kernel panic' 'Synchronous Abort' 'Unable to handle'; do
    grep -qa "$a" "$LOG" && { echo "  MISS anomaly: $a"; pass=0; }
done

[ "$pass" = 1 ] && { echo "PASS: U-Boot writes eMMC and booti's a TFTP'd kernel to Linux userspace"; exit 0; }
echo "FAIL"; exit 1
