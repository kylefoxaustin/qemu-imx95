#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# PCIe Root Complex - arbitrary endpoint bind + MSI + DMA, via virtio-9p-pci.
#
# The DWC PCIe RC (pcie0, hw/pci-host/imx95_pcie.c) now brings up an arbitrary
# endpoint end to end - not just enumeration (tests/pcie covers e1000e link-up +
# BAR assignment). This attaches a virtio-9p-pci device backed by a host
# directory and confirms the full path: the device's driver binds, MSI-X is
# allocated and delivered through the GICv3 ITS, the endpoint DMAs the virtqueue
# from guest RAM, and a 9p mount over the PCIe device reads a host file in the
# guest. That exercises the root-port bridge forwarding (CPU -> BAR MMIO), the
# pci_setup_iommu DMA path (endpoint -> guest RAM + the MSI doorbell), and the
# requester-ID LUT.
#
# Two dtb adjustments are needed - the same "QEMU doesn't model that block" kind
# the USB3/Neutron tests make:
#   - strip the pcie iommu-map: the 95 doesn't model the SMMU, so without this
#     every endpoint defers forever waiting for the IOMMU supplier;
#   - make the pcie msi-map identity: QEMU's ITS keys on the raw PCI
#     requester-ID, whereas the stock map translates it via the controller LUT
#     (which QEMU does not apply), so the MSI device-ID would not match.
#
# Required (override via env): QEMU, KBUILD (Image + base dtb + dtc), SM_ELF.
# SKIPs if any is missing.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=${IMAGE:-$KBUILD/arch/arm64/boot/Image}
BASE_DTB=${BASE_DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
DTC=${DTC:-$KBUILD/scripts/dtc/dtc}
INITRD=${INITRD:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
TMO=${TMO:-130}

skip() { echo "SKIP: $*"; exit 0; }
[ -x "$QEMU" ]     || skip "no QEMU ($QEMU)"
[ -e "$IMAGE" ]    || skip "no kernel Image ($IMAGE)"
[ -e "$BASE_DTB" ] || skip "no base dtb ($BASE_DTB)"
[ -x "$DTC" ]      || skip "no dtc ($DTC)"
[ -e "$SM_ELF" ]   || skip "no SM firmware ($SM_ELF)"
[ -e "$INITRD" ]   || skip "no busybox initramfs ($INITRD)"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG="$WORK/serial.log"
SHARE="$WORK/share"; mkdir -p "$SHARE"
MARK="PCIE_9P_HOSTFILE_$$"
echo "$MARK" > "$SHARE/hostfile.txt"

# Patch the pcie node: drop iommu-map, make msi-map identity.
"$DTC" -I dtb -O dts "$BASE_DTB" > "$WORK/evk.dts" 2>/dev/null
python3 - "$WORK/evk.dts" "$WORK/patched.dts" <<'PY'
import sys, re
t = open(sys.argv[1]).read()
i = t.index('pcie@4c300000'); b = t.index('{', i)
depth, j = 0, b
while True:
    c = t[j]
    if c == '{': depth += 1
    elif c == '}':
        depth -= 1
        if depth == 0: break
    j += 1
body = t[b:j]
body = re.sub(r'\n\s*iommu-map(-mask)? = <[^;]*>;', '', body)
m = re.search(r'msi-map = <0x0 (0x[0-9a-f]+) ', body)
its = m.group(1) if m else '0x2c'
body = re.sub(r'msi-map = <[^;]*>;', f'msi-map = <0x0 {its} 0x0 0x10000>;', body)
t = t[:b] + body + t[j:]
open(sys.argv[2], 'w').write(t)
PY
"$DTC" -I dts -O dtb -o "$WORK/patched.dtb" "$WORK/patched.dts" 2>/dev/null \
    || { echo "FAIL: dtc could not rebuild the patched dtb"; exit 1; }

STAGE="$WORK/root"; mkdir -p "$STAGE"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
cat > "$STAGE/init" <<INIT
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "=== PCIE9P ==="
sleep 8
echo "endpoint: drv=\$(readlink /sys/bus/pci/devices/0000:01:00.0/driver 2>/dev/null | sed 's#.*/##') msi=\$(ls /sys/bus/pci/devices/0000:01:00.0/msi_irqs 2>/dev/null)"
echo "virtio: \$(ls /sys/bus/virtio/devices 2>/dev/null)"
mkdir -p /mnt
timeout 12 mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt
echo "mount rc=\$?"
echo "hostfile: [\$(cat /mnt/hostfile.txt 2>/dev/null)]"
echo "=== PCIE9P-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.gz"

timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$WORK/patched.dtb" -initrd "$WORK/initrd.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -device virtio-9p-pci,bus=imx95-pcie,fsdev=fs0,mount_tag=hostshare \
  -fsdev local,id=fs0,path="$SHARE",security_model=none \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "--- pcie-9p report ---"
sed -n '/=== PCIE9P ===/,/=== PCIE9P-DONE ===/p' "$LOG"

fail() { echo "FAIL: $*"; exit 1; }
grep -qaE 'drv=virtio-pci'   "$LOG" || fail "virtio-pci did not bind to the endpoint"
grep -qaE '^virtio: virtio0' "$LOG" || fail "no virtio device created (MSI/probe failed)"
grep -qaE 'mount rc=0'       "$LOG" || fail "9p mount over the PCIe endpoint failed"
grep -qaE "$MARK"            "$LOG" || fail "host file not readable through the PCIe 9p mount"
grep -qaiE 'external abort|Unhandled fault|Internal error' "$LOG" \
    && fail "fault during the PCIe endpoint datapath"

echo "PASS: PCIe RC binds a virtio-9p-pci endpoint (MSI-X via ITS + DMA); 9p host file read end to end"
