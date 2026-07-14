#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# virtio-mmio transports + virtio-9p host file share.
#
# The fsl-imx95 SoC instantiates a few generic virtio-mmio transports and the
# board injects matching /virtio_mmio@ nodes into the supplied dtb, so the guest
# enumerates them on a normal boot (no cmdline-devices, no hand-edited dtb).
# This boots with a virtio-9p-device backed by a host directory and confirms the
# full path: the guest sees virtio0, 9p mounts over trans=virtio, and a file
# written on the host is visible + readable in the guest.
#
# Required (override via env): QEMU, KBUILD (Image + base dtb), SM_ELF. SKIPs if
# any is missing.
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
SHARE="$WORK/share"; mkdir -p "$SHARE"
MARK="HELLO_FROM_HOST_9P_$$"
echo "$MARK" > "$SHARE/hostfile.txt"

STAGE="$WORK/root"; mkdir -p "$STAGE"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "=== 9P ==="
sleep 3
echo "--- virtio devices ---"
ls /sys/bus/virtio/devices 2>/dev/null
echo "--- mount 9p hostshare ---"
mkdir -p /mnt
mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt 2>&1
echo "mount rc=$?"
echo "--- host file in guest ---"
cat /mnt/hostfile.txt 2>/dev/null
echo "=== 9P-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.gz"

timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -fsdev local,id=fsdev0,path="$SHARE",security_model=none \
  -device virtio-9p-device,fsdev=fsdev0,mount_tag=hostshare \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "--- 9p report ---"
sed -n '/=== 9P ===/,/=== 9P-DONE ===/p' "$LOG"

fail() { echo "FAIL: $*"; exit 1; }
grep -qaE '^virtio0'        "$LOG" || fail "guest did not enumerate the virtio-mmio transport"
grep -qaE 'mount rc=0'      "$LOG" || fail "9p mount over trans=virtio failed"
grep -qaE "$MARK"           "$LOG" || fail "host file not visible/readable in the guest"
grep -qaiE 'external abort|Unhandled fault|Internal error' "$LOG" \
    && fail "fault during virtio-9p bring-up"

echo "PASS: virtio-mmio guest-discoverable; 9p host share mounts and the host file is readable in the guest"
