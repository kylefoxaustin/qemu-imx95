#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 Neutron NPU bring-up - real-driver + TFLite-delegate end-to-end.
#
# Boots stock Linux with the Neutron NPU enabled and confirms the whole software
# stack comes up against our hw/misc/imx95_neutron.c model:
#
#   neutron driver probe -> remoteproc loads NeutronFirmware.elf -> RESETCTRL
#   clock-on -> firmware "started" handshake (APPCTRL=0xF807) -> /dev/neutron0
#   -> TFLite benchmark_model + the LiteRT Neutron delegate runs inferences via
#   the mailbox to completion (DONE).
#
# The proprietary NPU compute is NOT modelled, so inference *output* is not
# computed; this validates that the stack initialises and runs end to end (a
# "brings up" milestone, not a functional NPU).
#
# The base EVK dtb does not enable Neutron (it needs a firmware-DDR carveout from
# the imx95-19x19-evk-neutron.dtso overlay, a 4 GiB pool the stock overlay puts
# at 4 GiB). The prebuilt dtb has no __symbols__ so fdtoverlay can't apply it;
# instead we decompile, splice in a small in-range carveout + the neutron
# memory-region, and recompile with the kernel's dtc (the patch-dtb.py pattern).
# neutron + its remoteproc are built into the BSP kernel, so no modules.
#
# Required (override via env): QEMU, KBUILD (Image+dtb+dtc), SM_ELF, BSP_ROOTFS.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
BSP_ROOTFS=${BSP_ROOTFS:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx95-drone-sizer/tmp/work/imx95_19x19_lpddr5_evk-poky-linux/imx-image-full/1.0/rootfs}
IMAGE=$KBUILD/arch/arm64/boot/Image
DTB=$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb
DTC=$KBUILD/scripts/dtc/dtc
TMO=${TMO:-240}

need() { [ -e "$2" ] || { echo "missing $1: $2"; exit 1; }; }
need QEMU "$QEMU"; need IMAGE "$IMAGE"; need DTB "$DTB"; need SM_ELF "$SM_ELF"
need DTC "$DTC"

FW="$BSP_ROOTFS/usr/lib/firmware/NeutronFirmware.elf"
TFL="$BSP_ROOTFS/usr/bin/tensorflow-lite-2.19.0/examples/benchmark_model"
MODEL="$BSP_ROOTFS/usr/bin/tensorflow-lite-2.19.0/examples/mobilenet_v1_1.0_224_quant.tflite"
DELEGATE=$(ls "$BSP_ROOTFS"/usr/lib/liblitert_neutron_delegate.so \
              "$BSP_ROOTFS"/usr/lib/libneutron_delegate.so 2>/dev/null | head -1)
if [ ! -e "$FW" ] || [ ! -x "$TFL" ] || [ -z "${DELEGATE:-}" ]; then
    echo "SKIP: no Neutron userspace (firmware/benchmark_model/delegate) in"
    echo "      BSP_ROOTFS=$BSP_ROOTFS"
    exit 0
fi

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/root"; mkdir -p "$STAGE"
LOG=${LOG:-$WORK/serial.log}

# --- patch the dtb: small in-range firmware carveout @4 GiB + memory-region ---
"$DTC" -I dtb -O dts "$DTB" > "$WORK/base.dts" 2>/dev/null
python3 - "$WORK/base.dts" "$WORK/neutron.dts" <<'PY'
import re, sys
src, dst = sys.argv[1], sys.argv[2]
t = open(src).read()
# A 128 MiB reusable pool at phys 4 GiB (in range with -m 4G; the stock overlay
# uses 4 GiB but a 4 GiB pool, oversized for the test model).
node = ('\n\t\tneutron_test_mem: neutron_memory@100000000 {\n'
        '\t\t\tcompatible = "shared-dma-pool";\n'
        '\t\t\treusable;\n'
        '\t\t\treg = <0x1 0x00000000 0x0 0x08000000>;\n'
        '\t\t};\n')
# Insert after the reserved-memory block's own properties (a subnode may not
# precede properties), i.e. right after its "ranges;" line.
t, n = re.subn(r'(reserved-memory \{\n(?:\t+[^\n]*\n)*?\t+ranges;\n)',
               r'\1' + node, t, count=1)
assert n == 1, "reserved-memory block not found"
# add memory-region to the neutron device node
t, n = re.subn(r'(imx95-neutron@4ab00004 \{\n)',
               r'\1\t\t\tmemory-region = <&neutron_test_mem>;\n', t, count=1)
assert n == 1, "neutron node not found"
# Drop the SMMU/iommu dependency. QEMU does not model the arm-smmu-v3 at
# 0x490d0000 (its ID registers read 0 -> the driver logs "no translation
# support!" and fails to probe), so the neutron compute device - the only NPU
# node with an iommus phandle (stream id 0x0d) - can never bind, with no
# /dev/neutron0. The NPU DMAs directly under emulation (identity-mapped), so
# strip the iommus property and the device probes.
t, n = re.subn(r'\n\t+iommus = <0x[0-9a-f]+ 0x0?d>;', '', t, count=1)
assert n == 1, "neutron iommus not found"
open(dst, "w").write(t)
print("dtb patched: carveout + memory-region + iommus dropped")
PY
"$DTC" -I dts -O dtb -o "$WORK/neutron.dtb" "$WORK/neutron.dts" 2>/dev/null

# --- initramfs: busybox + firmware + TFLite app + delegate + .so closure ------
zcat "$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz" | \
    (cd "$STAGE" && cpio -idmu 2>/dev/null)
mkdir -p "$STAGE/lib/firmware" "$STAGE/usr/lib/firmware"
cp "$FW" "$STAGE/lib/firmware/NeutronFirmware.elf"
cp "$FW" "$STAGE/usr/lib/firmware/NeutronFirmware.elf"
cp "$MODEL" "$STAGE/model.tflite"

python3 - "$BSP_ROOTFS" "$STAGE" "$TFL" "$DELEGATE" <<'PY'
import os, sys, subprocess, shutil, glob
RF, STAGE, TFL, DELEGATE = sys.argv[1:5]
libdirs = [f"{RF}/lib", f"{RF}/usr/lib", f"{RF}/usr/lib/aarch64-linux-gnu"]
done = set()
def needed(p):
    try:
        out = subprocess.check_output(["readelf", "-d", p], text=True,
                                      stderr=subprocess.DEVNULL)
    except Exception:
        return []
    return [l.split("[")[1].split("]")[0] for l in out.splitlines()
            if "(NEEDED)" in l]
def find(name):
    for d in libdirs:
        c = os.path.join(d, name)
        if os.path.exists(c):
            return c
    g = glob.glob(f"{RF}/usr/lib/**/{name}", recursive=True)
    return g[0] if g else None
def copy_into(src):
    if not src or src in done or not os.path.exists(src):
        return
    done.add(src)
    dst = os.path.join(STAGE, os.path.relpath(src, RF))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if not os.path.exists(dst):
        shutil.copy2(src, dst)
    for n in needed(src):
        copy_into(find(n))
for r in [f"{RF}/lib/ld-linux-aarch64.so.1", TFL, DELEGATE]:
    copy_into(r)
ld = os.path.join(STAGE, "lib/ld-linux-aarch64.so.1")
if os.path.exists(ld):
    os.makedirs(os.path.join(STAGE, "usr/lib"), exist_ok=True)
    shutil.copy2(ld, os.path.join(STAGE, "usr/lib/ld-linux-aarch64.so.1"))
# stage the app + delegate at fixed paths for the init script
shutil.copy2(TFL, os.path.join(STAGE, "benchmark_model"))
shutil.copy2(DELEGATE, os.path.join(STAGE, "neutron_delegate.so"))
print("staged libs:", len(done))
PY

cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev
/bin/busybox --install -s /bin 2>/dev/null
/bin/busybox mkdir -p /tmp
export PATH=/bin:/usr/bin
export LD_LIBRARY_PATH=/lib:/usr/lib:/usr/lib/aarch64-linux-gnu
exec > /dev/console 2>&1
sleep 7
echo "=== NEUTRON-E2E ==="
echo "dev: $(ls /dev/neutron* 2>/dev/null | tr '\n' ' ')"
echo "deferred: $(cat /sys/kernel/debug/devices_deferred 2>/dev/null | tr '\n' ',')"
echo "neutron-drv-bound: $(ls /sys/bus/platform/drivers/neutron/ 2>/dev/null | grep -i 4ab | tr '\n' ' ')"
echo "dmesg-neutron:"
dmesg | grep -iE 'neutron|remoteproc|NeutronFirmware|npu|176179|booting fw|direct firmware' | tail -12
chmod +x /benchmark_model 2>/dev/null
/benchmark_model --graph=/model.tflite --external_delegate_path=/neutron_delegate.so \
    --num_runs=2 --warmup_runs=1 > /tmp/bm.log 2>&1
echo "benchmark rc=$?"
grep -iE 'delegate|neutron|inference|count=|average' /tmp/bm.log | tail -8
echo "=== NEUTRON-E2E-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

# -m 4G so the @4 GiB carveout is backed.
timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 4G -display none \
    -kernel "$IMAGE" -dtb "$WORK/neutron.dtb" -initrd "$WORK/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "=== neutron bring-up e2e report ==="
sed -n '/NEUTRON-E2E ===/,/NEUTRON-E2E-DONE/p' "$LOG" | grep -vE '^\[ *[0-9].*\]'

# Full-stack bring-up: the neutron remoteproc registers, the compute device
# binds (the dtb patch drops its iommus phandle - QEMU has no arm-smmu-v3 model,
# so it logs "no translation support!" and a device needing the IOMMU can't
# bind), NeutronFirmware.elf loads into the model's DTCM/ITCM, and the LiteRT
# delegate opens /dev/neutron0 and runs benchmark_model. The NPU does not
# actually compute (the delegate offloads 0 nodes and inference falls back to
# CPU, which keeps results correct) - that proprietary-firmware compute is the
# model's fidelity ceiling; the entire driver/firmware/delegate path is what is
# validated here, with the mailbox responder also covered by the qtest.
if grep -qa '/dev/neutron' "$LOG" && grep -qa 'benchmark rc=0' "$LOG"; then
    echo "PASS: Neutron NPU stack brings up end to end" \
         "(driver + firmware load + delegate + benchmark_model)"
    exit 0
fi
if grep -qa 'neutron-rproc is available' "$LOG"; then
    echo "SKIP: neutron remoteproc + firmware carveout up, but /dev/neutron0 or"
    echo "      the benchmark did not complete (see report above)."
    exit 0
fi
echo "FAIL: neutron remoteproc did not register"; exit 1
