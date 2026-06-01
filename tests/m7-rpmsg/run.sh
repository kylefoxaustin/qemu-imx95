#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# v1.x Step 4: SM-orchestrated M7 + Linux rpmsg ping/pong over MU7.
#
# Boots the full stack - 6x A55 (Linux) + M33 (real SM) + M7 (NXP rpmsg-lite
# pingpong firmware). The SM boots the M7; Linux's imx_rproc attaches to the
# SM-managed core; the stock NXP imx_rpmsg_pingpong kernel module then
# exchanges 100 messages with the M7 rpmsg-lite client - doorbell kicks over
# the modelled MU7 cross-connect (hw/misc/imx_mu.c TR->RR peer forwarding),
# payload in the shared vrings of the M7 DRAM carveout.
#
# PASS = the module prints "goodbye!", which it does only after the 100th
# round-trip with the remote completes. (The matching functional test is
# tests/functional/aarch64/test_imx95_evk.py::...m7_rpmsg_pingpong.)
#
# Artifacts (all from NXP source/BSP, not redistributable):
#   M7_FW    - NXP's rpmsg_lite_pingpong_rtos_linux_remote, the raw M7 TCM
#              image. The BSP ships it prebuilt in the target rootfs at
#              /usr/lib/firmware/imx95-19x19-evk_m7_TCM_rpmsg_lite_pingpong_rtos_linux_remote.bin
#              (or build it from the MCUXpresso SDK multicore examples).
#   INITRD   - an initramfs that bundles the matching imx_rpmsg_pingpong.ko
#              and an /init that loads it (see this directory's README.md).
#   SM_ELF   - the System Manager firmware m33_image.elf.
#   KERNEL/DTB - the stock NXP BSP Linux Image + imx95-19x19-evk.dtb.
#
# Related: tests/m7-fault-recovery (SM cold-resets a faulting M7).

set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
SM_ELF=${SM_ELF:-${IMX95_ARTIFACTS:-$HOME/imx95-artifacts}/m33_image.elf}
KERNEL=${KERNEL:-${IMX95_ARTIFACTS:-$HOME/imx95-artifacts}/linux-build/arch/arm64/boot/Image}
DTB=${DTB:-${IMX95_ARTIFACTS:-$HOME/imx95-artifacts}/linux-build/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
M7_FW=${M7_FW:-}
INITRD=${INITRD:-}
TMO=${TMO:-90}

need() {
    [ -e "$2" ] && return 0
    echo "error: $3 not found:" >&2
    echo "    ${2:-<unset>}" >&2
    echo "  set \$$1 (see tests/m7-rpmsg/README.md)" >&2
    exit 1
}
need QEMU   "$QEMU"   "qemu-system-aarch64 (build it: see README 'Building')"
need SM_ELF "$SM_ELF" "System Manager firmware m33_image.elf"
need KERNEL "$KERNEL" "kernel Image"
need DTB    "$DTB"    "device tree imx95-19x19-evk.dtb"
need M7_FW  "$M7_FW"  "M7 rpmsg pingpong firmware (.bin, raw TCM image)"
need INITRD "$INITRD" "rpmsg initramfs (bundles imx_rpmsg_pingpong.ko + /init)"

A55_LOG=$(mktemp /tmp/imx95-rpmsg-a55.XXX.log)
M7_LOG=$(mktemp /tmp/imx95-rpmsg-m7.XXX.log)

cleanup() { rm -f "$A55_LOG" "$M7_LOG"; }
trap cleanup EXIT

CMDLINE="earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init"

# The M7 rpmsg firmware is a raw TCM image: load it at the M7 TCM alias the
# remoteproc carveout points at (0x203c0000), not via ELF with cpu-num. The
# SM boots the M7 from there.
timeout --signal=KILL "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$INITRD" -append "$CMDLINE" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -device loader,file="$M7_FW",addr=0x203c0000,force-raw=on \
    -serial file:"$A55_LOG" -serial null -serial file:"$M7_LOG" \
    -serial null -serial null -serial null -serial null -serial null \
    -monitor none >/dev/null 2>&1

fail=0

if grep -qE "SCMI Protocol v2.1 'NXP:IMX' Firmware version 0x333" "$A55_LOG"; then
    echo "PASS: SCMI handshake against the real SM (M33 <-> A55 MU2 up)"
else
    echo "FAIL: SCMI handshake not seen (SM/MU2 path)"; fail=1
fi

got=$(grep -oE "get [0-9]+ \(src" "$A55_LOG" | tail -1)
if grep -q "goodbye!" "$A55_LOG"; then
    echo "PASS: rpmsg ping/pong completed (last '${got:-?}', module said goodbye)"
else
    echo "FAIL: rpmsg ping/pong did not complete (no 'goodbye!'); last '${got:-<none>}'"
    grep -iE "rpmsg|pingpong|imx_rproc|remoteproc" "$A55_LOG" | tail -20 | sed 's/^/    /'
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "=== M7 rpmsg ping/pong verified: 100 messages over MU7 + vrings ==="
else
    echo "=== tests/m7-rpmsg FAILED ==="
    exit 1
fi
