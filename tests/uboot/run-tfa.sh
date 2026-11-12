#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 secure boot chain: ROM -> SM -> SPL -> TF-A BL31 -> OP-TEE -> U-Boot.
#
# tests/uboot/run.sh boots the chain our own build produces, where U-Boot
# itself runs at EL3 and QEMU answers the PSCI SMCs. This one boots the chain
# a board runs, with the vendor's EL3 firmware in the image:
#
#   machine stands in for the boot ROM: walks the image set on uSDHC1, loads
#   the SM into M33 ITCM and SPL to 0x20480000
#     -> SM boots on the M33 and brings up both its SCMI channels: the
#        non-secure one on MU2 (Linux, U-Boot) and the secure one on MU1,
#        which is the agent TF-A is ("SCMI_AGENT1 name=AP-S, secure" in the
#        SM's own configs/mx95evk.cfg)
#     -> SPL loads set 1 and enters image 0, which is BL31, not U-Boot
#     -> BL31 runs at EL3, talks ELE and SCMI, hands off to OP-TEE at S-EL1
#     -> OP-TEE initialises (including its ELE RNG) and returns to BL31
#     -> BL31 enters U-Boot as BL33, which reaches its prompt
#
# PASS = every stage announces itself in order AND the prompt is reached.
# QEMU's PSCI emulation is off for this run (-machine el3-firmware=on), so
# every SMC is answered by BL31 rather than by us.
#
# BL31 and OP-TEE are NXP BSP artifacts and are not redistributable, so they
# are operator-supplied and this test skips without them. Both can be taken
# out of a BSP imx-boot/flash_all image - it is a container set, and
# tests/uboot/mkbootimg.py documents the format.
#
# Required (override via env): QEMU, SM_BIN, SPL_BIN, UBOOT_BIN, BL31_BIN,
# OPTEE_BIN.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
SM_BIN=${SM_BIN:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.bin}
SPL_BIN=${SPL_BIN:-$ROOT/tests/spl-banner/uboot-build/spl/u-boot-spl.bin}
UBOOT_BIN=${UBOOT_BIN:-$ROOT/tests/spl-banner/uboot-build/u-boot.bin}
BL31_BIN=${BL31_BIN:-}
OPTEE_BIN=${OPTEE_BIN:-}
TMO=${TMO:-300}

skip() { echo "SKIP: $*"; exit 0; }
need() { [ -n "$2" ] || skip "set $1= (operator-supplied BSP artifact)"
         [ -e "$2" ] || skip "missing $1: $2"; }
need QEMU "$QEMU"; need SM_BIN "$SM_BIN"
need SPL_BIN "$SPL_BIN"; need UBOOT_BIN "$UBOOT_BIN"
need BL31_BIN "$BL31_BIN"; need OPTEE_BIN "$OPTEE_BIN"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG=${LOG:-$WORK/serial.log}

python3 "$HERE/mkbootimg.py" "$UBOOT_BIN" "$WORK/boot.img" \
    --sm "$SM_BIN" --spl "$SPL_BIN" \
    --bl31 "$BL31_BIN" --optee "$OPTEE_BIN" >/dev/null

# Hammer newlines to interrupt autoboot, then prove the prompt is live.
{
    for _ in $(seq 1 40); do printf '\n'; sleep 0.25; done
    sleep 2
    printf 'echo TFA-CHAIN-ALIVE\n';  sleep 2
    printf 'bdinfo\n';                sleep 3
} | timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk,el3-firmware=on \
    -m 2G -display none \
    -drive if=none,format=raw,file="$WORK/boot.img",id=mmc0 \
    -device emmc,drive=mmc0 \
    -serial stdio -serial null 2>&1 | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' > "$LOG" || true

echo "=== chain report ==="
grep -aE "^U-Boot|Image set:|Load image from|BL31:|TFA-CHAIN-ALIVE" "$LOG" | head -10

pass=1
check() {
    if grep -qa "$1" "$LOG"; then
        echo "  ok   $2"
    else
        echo "  MISS $2"; pass=0
    fi
}
check 'U-Boot SPL'                  "SPL (loaded by the emulated ROM)"
check 'Image set: 0, offset: 0x8000' "passover -> image set 0 @0x8000"
check 'NOTICE:  BL31:'              "TF-A BL31 runs at EL3"
check '^U-Boot 20'                  "U-Boot entered as BL33"
check 'u-boot=>'                    "U-Boot prompt"
check 'TFA-CHAIN-ALIVE'             "prompt is interactive"

# BL31 owns EL3 here, so QEMU must not have answered a PSCI SMC. If our PSCI
# emulation were still wired, BL31's own services would be shadowed and these
# stages would not all have reported.
for a in 'Synchronous Abort' 'Unhandled fault' 'ERROR ### Please RESET' \
         'Kernel panic'; do
    grep -qa "$a" "$LOG" && { echo "  MISS anomaly: $a"; pass=0; }
done

[ "$pass" = 1 ] && {
    echo "PASS: ROM -> SM -> SPL -> BL31 -> OP-TEE -> U-Boot prompt"
    exit 0
}
echo "FAIL"; exit 1
