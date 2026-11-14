#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95: boot NXP's released imx-boot image unmodified.
#
# Every other test here boots a container set this tree builds (mkbootimg.py).
# This one takes a BSP imx-boot/flash_all exactly as NXP ships it and puts it
# on the card at the 32 KiB offset the ROM reads, with nothing extracted,
# repacked or rebuilt. What it exercises that the others cannot:
#
#   * a version-2 container header and the 0x4000 header stride, which a
#     released image uses and ours does not
#   * the three-container shape - ELE, V2X, boot images - and so the V2X
#     walk-on rule, which is the only way a set gets a third container
#   * NXP's own SM firmware on the M33, not the one built from imx-sm source
#   * NXP's own SPL, which finds the second image set by its own arithmetic
#   * NXP's own second image set: BL31, OP-TEE and U-Boot proper, all three
#     loaded by that SPL rather than placed by mkbootimg.py
#
# The stride is a function of the SoC revision: U-Boot's
# container_hdr_alignment() returns 0x400 below rev 2.0 and 0x4000 from rev
# 2.0 up, and the emulated ROM follows the same rule off the same "soc-rev"
# property. A released image needs the 0x4000 one, so this test runs the
# machine at soc-rev=0xB0 (rev 2.0) - and the U-Boot it ends at prints
# "Rev2.0" back, which is the guest confirming the arithmetic rather than us
# asserting it.
#
# el3-firmware=on is not optional here. The released second image set carries
# BL31 and OP-TEE, so EL3 belongs to the guest, and QEMU's own PSCI must be
# off or it answers OP-TEE's init SMC itself: OP-TEE then never hands back to
# BL31 and the chain stops after BL31's banner with every image correctly
# loaded. That failure prints nothing and leaves CPU 0 parked in secure EL1.
#
# PASS = the ROM read the released set, the vendor SM, SPL, BL31 and U-Boot
# proper all ran, and the prompt answers. The checks are independent greps.
#
# What this still does not do is boot Linux on top. At rev 2.0 U-Boot's
# disable_smmu_node() no longer strips iommu@490d0000 out of the dtb (it only
# does that for A-revisions, and this image's SM reports cfgname "mx95evk",
# not the "mx95alt" that would also disable it), so the dtb handed to Linux
# has an SMMU node this machine does not model. That is the parked SMMU work,
# not something to paper over here.
#
# The image is an NXP BSP artifact and is not redistributable, so it is
# operator-supplied and this test skips without it. In a BSP build it is
#   imx-boot-imx95-19x19-lpddr5-evk-sd.bin-flash_all
#
# Required (override via env): QEMU, VENDOR_IMX_BOOT.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
VENDOR_IMX_BOOT=${VENDOR_IMX_BOOT:-}
SOC_REV=${SOC_REV:-0xb0}
# The vendor chain is long - ELE, SM, SPL, BL31, OP-TEE, then a U-Boot that
# probes PCIe and two ENETC ports before it offers a prompt. 240 s leaves
# headroom over that on a host with other guests on it.
TMO=${TMO:-240}

skip() { echo "SKIP: $*"; exit 0; }
need() { [ -e "$2" ] || skip "missing $1: $2"; }
[ -n "$VENDOR_IMX_BOOT" ] ||
    skip "VENDOR_IMX_BOOT is not set (a BSP imx-boot/flash_all)"
need QEMU "$QEMU"; need VENDOR_IMX_BOOT "$VENDOR_IMX_BOOT"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
AP=${AP:-$WORK/ap.log}
SM=${SM:-$WORK/sm.log}
TRACE=${TRACE:-$WORK/trace.log}

# The card: the image at the 32 KiB offset the ROM reads (CONTAINER_HDR_MMCSD),
# untouched otherwise. Nothing else is on it, so there is no rootfs to find -
# the prompt is the endpoint.
truncate -s 64M "$WORK/card.img"
dd if="$VENDOR_IMX_BOOT" of="$WORK/card.img" \
   bs=1k seek=32 conv=notrunc status=none

# Serial 0 is the AP console (SPL, BL31, U-Boot), serial 1 the M33's (the SM).
# Hammer newlines to interrupt autoboot whenever it arrives, then prove the
# prompt is live. "-serial stdio" needs this stdin: with nothing behind it the
# log comes out empty, which reads as a dead guest rather than no instrument.
{
    for _ in $(seq 1 220); do printf '\n'; sleep 0.5; done
    printf 'echo VENDOR-UBOOT-ALIVE\n'; sleep 3
    printf 'version\n';                 sleep 3
} | timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk,el3-firmware=on \
    -m 2G -display none \
    -global "fsl-imx95.soc-rev=$SOC_REV" \
    -drive if=none,format=raw,file="$WORK/card.img",id=mmc0 \
    -device emmc,drive=mmc0 \
    -trace imx95_rom_container -trace imx95_rom_image -D "$TRACE" \
    -serial stdio -serial "file:$SM" 2>&1 |
    sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' > "$AP" || true

echo "=== rom walk ==="
grep -aE "imx95_rom_(container|image)" "$TRACE" ||
    echo "  (nothing: the ROM read no container)"
echo "=== vendor chain ==="
sed '/^$/d' "$SM"
grep -aE "^U-Boot|Trying to boot from|Image set:|Load image from|BL31:|^CPU: " \
    "$AP" || true

pass=1
ck() {
    if grep -qa "$2" "$3"; then
        echo "  ok   $1"
    else
        echo "  MISS $1"; pass=0
    fi
}

# The ROM stepped past the first container, which only a 0x4000 stride and an
# accepted version-2 header can do: at the 0x400 stride the walk ends on the
# released set's first header.
ck "ROM walked the released set to the boot container" \
   "imx95_rom_container container at 0x10000" "$TRACE"
ck "ROM loaded the SM to M33 ITCM"   "to 0x1ffc0000" "$TRACE"
ck "ROM loaded SPL to OCRAM"         "to 0x20480000" "$TRACE"
ck "vendor SM runs on the M33"       "Hello from SM" "$SM"
ck "vendor SPL runs on the A55"      "U-Boot SPL"    "$AP"
ck "SPL read the ROM passover"       "Image set: 0, offset: 0x8000" "$AP"
ck "SPL found the second image set"  "Load image from MMC/SD" "$AP"
ck "TF-A BL31 runs at EL3"           "NOTICE:  BL31:" "$AP"
ck "vendor U-Boot entered as BL33"   "^U-Boot 20"    "$AP"
ck "U-Boot read back rev 2.0"        "i.MX95 Rev2.0" "$AP"
ck "U-Boot prompt"                   "u-boot=>"      "$AP"
ck "prompt is interactive"           "VENDOR-UBOOT-ALIVE" "$AP"

# BL31 owns EL3 here, so QEMU must not have answered a PSCI SMC, and OP-TEE
# must have handed back: "api uid mismatch" is what U-Boot's tee-optee driver
# prints when the SMC it issues is not answered by an OP-TEE.
for a in 'Synchronous Abort' 'Unhandled fault' 'ERROR ### Please RESET' \
         'api uid mismatch'; do
    grep -qa "$a" "$AP" && { echo "  MISS anomaly: $a"; pass=0; }
done

if [ "$pass" = 1 ]; then
    echo "PASS: released imx-boot -> ROM -> vendor SM -> vendor SPL" \
         "-> BL31 -> vendor U-Boot prompt"
else
    echo "FAIL"
    exit 1
fi
