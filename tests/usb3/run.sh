#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# USB3 SuperSpeed (DWC3) host-controller bring-up.
#
# The snps,dwc3 core (usb@4c100000) is modelled with QEMU's usb_dwc3 (which
# wraps a sysbus xHCI). Boots Linux and confirms the dwc3-imx glue + dwc3-core
# bind and the xHCI registers as a USB 3.0 SuperSpeed host (two root hubs), with
# no external abort - the reads-as-0 stub it replaces faulted dwc3_core_init.
#
# The stock EVK dtb tags usb3 with iommus=<&smmu 0xe>; with no SMMU model the
# core would never probe (deferred), so the test strips that phandle (identity
# DMA), the same approach the Neutron NPU test uses.
#
# NOTE: device enumeration on the SuperSpeed host is a follow-on - cold-plugged
# devices attach in QEMU but the guest xHCI roothub does not yet see them (an
# xHCI-model detail); USB device enumeration today is via the USB2 ChipIdea
# host (tests + the usb-kbd display path).
#
# Required (override via env): QEMU, KBUILD (Image + dtb + dtc), SM_ELF. SKIPs
# if any is missing.
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
TMO=${TMO:-120}

skip() { echo "SKIP: $*"; exit 0; }
[ -x "$QEMU" ]   || skip "no QEMU ($QEMU)"
[ -e "$IMAGE" ]  || skip "no kernel Image ($IMAGE)"
[ -e "$DTB" ]    || skip "no dtb ($DTB)"
[ -e "$DTC" ]    || skip "no dtc ($DTC)"
[ -e "$SM_ELF" ] || skip "no SM firmware ($SM_ELF)"
[ -e "$INITRD" ] || skip "no busybox initramfs ($INITRD)"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG="$WORK/serial.log"

# Strip usb3's iommus phandle (no SMMU model) so dwc3-core probes.
"$DTC" -I dtb -O dts "$DTB" > "$WORK/evk.dts" 2>/dev/null
python3 - "$WORK/evk.dts" "$WORK/patched.dts" <<'PY'
import sys, re
t = open(sys.argv[1]).read()
i = t.index('usb@4c100000'); j = t.index('};', i); node = t[i:j]
open(sys.argv[2], 'w').write(t[:i] + re.sub(r'\n\s*iommus = <[^;]*>;', '', node) + t[j:])
PY
"$DTC" -I dts -O dtb -o "$WORK/patched.dtb" "$WORK/patched.dts" 2>/dev/null

STAGE="$WORK/root"; mkdir -p "$STAGE"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "=== USB3 ==="
sleep 4
dmesg | grep -aiE 'dwc3 4c100000|xHCI Host Controller|SuperSpeed|external abort' | head
echo "=== USB3-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$WORK/patched.dtb" -initrd "$WORK/initrd.cpio.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "--- usb3 report ---"
sed -n '/USB3 ===/,/USB3-DONE/p' "$LOG" | grep -vE 'USB3 ==='
if grep -qaE 'xHCI Host Controller' "$LOG" && grep -qa 'SuperSpeed' "$LOG" \
   && ! grep -qa 'external abort' "$LOG"; then
    echo "PASS: DWC3 xHCI SuperSpeed host registered (no abort)"
    exit 0
fi
echo "FAIL: DWC3 xHCI host did not come up"; exit 1
