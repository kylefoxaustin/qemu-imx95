#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Second PCIe Root Complex (pcie1) - endpoint bind + MSI + DMA via virtio-9p-pci.
#
# The i.MX95 has two general-purpose DWC PCIe controllers. pcie0 (pcie@4c300000,
# PCI domain 0) is covered by tests/pcie + tests/pcie-9p; this exercises pcie1
# (pcie@4c380000, PCI domain 1), the second instance of the same
# hw/pci-host/imx95_pcie.c host. The EVK dtb enables pcie1 ("status = okay"), so
# this attaches a virtio-9p-pci device to pcie1's secondary bus (imx95-pcie1)
# and confirms the full path on the SECOND controller: link-up in domain 0001,
# the device's driver binds, MSI-X is delivered through the GICv3 ITS, the
# endpoint DMAs the virtqueue, and a 9p mount over it reads a host file. This
# also proves the two controllers coexist (distinct bus names + PCI domains).
#
# The same two dtb adjustments pcie0 needs apply to pcie1's node (the 95 models
# neither the SMMU nor the controller requester-ID->stream-ID LUT translation):
#   - strip the iommu-map (no SMMU supplier, else the endpoint defers forever);
#   - make the msi-map identity (QEMU's ITS keys on the raw requester-ID).
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
MARK="PCIE1_9P_HOSTFILE_$$"
echo "$MARK" > "$SHARE/hostfile.txt"

# Patch the pcie1 node: drop iommu-map, make msi-map identity.
"$DTC" -I dtb -O dts "$BASE_DTB" > "$WORK/evk.dts" 2>/dev/null
python3 - "$WORK/evk.dts" "$WORK/patched.dts" <<'PY'
import sys, re
t = open(sys.argv[1]).read()
i = t.index('pcie@4c380000'); b = t.index('{', i)
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
its = m.group(1) if m else '0x98'
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
echo "=== PCIE1 ==="
sleep 8
echo "pci-devices: \$(ls /sys/bus/pci/devices 2>/dev/null | tr '\n' ' ')"
echo "endpoint: drv=\$(readlink /sys/bus/pci/devices/0001:01:00.0/driver 2>/dev/null | sed 's#.*/##')"
echo "virtio: \$(ls /sys/bus/virtio/devices 2>/dev/null)"
mkdir -p /mnt
timeout 12 mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt
echo "mount rc=\$?"
echo "hostfile: [\$(cat /mnt/hostfile.txt 2>/dev/null)]"
echo "=== PCIE1-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.gz"

timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$WORK/patched.dtb" -initrd "$WORK/initrd.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -device virtio-9p-pci,bus=imx95-pcie1,fsdev=fs0,mount_tag=hostshare \
  -fsdev local,id=fs0,path="$SHARE",security_model=none \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "--- pcie1 report ---"
sed -n '/=== PCIE1 ===/,/=== PCIE1-DONE ===/p' "$LOG"

fail() { echo "FAIL: $*"; exit 1; }
grep -qaE '4c380000.pcie.*link up'  "$LOG" || fail "pcie1 link did not come up"
grep -qaE 'drv=virtio-pci'          "$LOG" || fail "virtio-pci did not bind on pcie1"
grep -qaE '^virtio: virtio0'        "$LOG" || fail "no virtio device (MSI/probe failed)"
grep -qaE 'mount rc=0'              "$LOG" || fail "9p mount over the pcie1 endpoint failed"
grep -qaE "$MARK"                   "$LOG" || fail "host file not readable through pcie1 9p mount"
grep -qaiE 'external abort|Unhandled fault|Internal error' "$LOG" \
    && fail "fault during the pcie1 endpoint datapath"

echo "PASS: pcie1 (2nd RC, PCI domain 1) binds a virtio-9p-pci endpoint (MSI-X via ITS + DMA); 9p host file read end to end"
