#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 Neutron NPU - converted-model submission datapath (Tier-2 scaffold).
#
# This is the integration gate for a *neutron-converted* model (the artifact the
# eIQ Neutron Converter produces: a .tflite carrying a "NeutronGraph" custom op
# with embedded microcode/weights/kernels blobs). Unlike run.sh - which boots a
# STOCK, unconverted MobileNet that the delegate cannot offload (0 nodes -> CPU
# fallback) - this drives a converted model so the delegate actually claims the
# NeutronGraph node and submits it to /dev/neutron0 over the mailbox RUN command.
#
# What this asserts (the SUBMISSION datapath):
#   - the delegate claims/offloads the NeutronGraph node (non-zero delegate
#     kernels), i.e. the converter output reached the NPU path, not CPU fallback;
#   - benchmark_model runs to completion (rc=0) with no fault/abort, i.e. the
#     mailbox RUN -> DONE round-trip works for a real converted program.
#
# What this does NOT assert: numerical output correctness. The current model is a
# mailbox responder with no execution engine, so a converted node returns
# uncomputed output (there is no CPU fallback for a CLAIMED NeutronGraph node).
# Output correctness is the job of (a) the Tier-1 qtest (bit-exact, no Linux) and
# (b) this test's optional VERIFY hook once the execution engine exists - see
# docs/neutron-full-execution-integration.md (the integration spec).
#
# So this gate is useful in two stages:
#   stage 1 (today, no engine): drop a converted .tflite in -> it goes GREEN,
#            proving the eIQ artifact flows through delegate -> driver -> mailbox;
#   stage 2 (engine landed):    set VERIFY_CMD=... (a golden-output check) -> it
#            additionally proves the engine computes the right answer.
#
# It SKIPs cleanly (exit 0) until a converted fixture is provided, so it never
# goes red in CI before NXP drops a model in.
#
# Provide the converted model via env or drop it at the default path:
#   CONVERTED_MODEL  path to a neutron-converted .tflite
#                    (default: tests/neutron/fixtures/converted.tflite)
#   VERIFY_CMD       optional shell command run INSIDE the guest after the
#                    benchmark; non-zero exit fails the test. Use it to assert a
#                    golden output once the engine exists (e.g. a label_image
#                    run grepped for the expected class). Receives no args.
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
CONVERTED_MODEL=${CONVERTED_MODEL:-$HERE/fixtures/converted.tflite}
VERIFY_CMD=${VERIFY_CMD:-}
TMO=${TMO:-240}

skip() { echo "SKIP: $*"; exit 0; }

# --- the one gate that keeps this inert until NXP drops a converted model in ---
[ -e "$CONVERTED_MODEL" ] || skip \
  "no converted model (set CONVERTED_MODEL= or drop one at
      $CONVERTED_MODEL).
      This is the neutron-converter output: a .tflite with a NeutronGraph custom
      op. Until one exists this scaffold cannot run - by design."

need() { [ -e "$2" ] || { echo "missing $1: $2"; exit 1; }; }
need QEMU "$QEMU"; need IMAGE "$IMAGE"; need DTB "$DTB"; need SM_ELF "$SM_ELF"
need DTC "$DTC"

FW="$BSP_ROOTFS/usr/lib/firmware/NeutronFirmware.elf"
TFL="$BSP_ROOTFS/usr/bin/tensorflow-lite-2.19.0/examples/benchmark_model"
DELEGATE=$(ls "$BSP_ROOTFS"/usr/lib/liblitert_neutron_delegate.so \
              "$BSP_ROOTFS"/usr/lib/libneutron_delegate.so 2>/dev/null | head -1)
if [ ! -e "$FW" ] || [ ! -x "$TFL" ] || [ -z "${DELEGATE:-}" ]; then
    skip "no Neutron userspace (firmware/benchmark_model/delegate) in
      BSP_ROOTFS=$BSP_ROOTFS"
fi

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/root"; mkdir -p "$STAGE"
LOG=${LOG:-$WORK/serial.log}

# --- patch the dtb: firmware carveout @4 GiB + memory-region + drop iommus ------
# (identical to run.sh - QEMU models no arm-smmu-v3, so the neutron compute node
# can only bind with its iommus phandle stripped; the NPU DMAs identity-mapped.)
"$DTC" -I dtb -O dts "$DTB" > "$WORK/base.dts" 2>/dev/null
python3 - "$WORK/base.dts" "$WORK/neutron.dts" <<'PY'
import re, sys
src, dst = sys.argv[1], sys.argv[2]
t = open(src).read()
node = ('\n\t\tneutron_test_mem: neutron_memory@100000000 {\n'
        '\t\t\tcompatible = "shared-dma-pool";\n'
        '\t\t\treusable;\n'
        '\t\t\treg = <0x1 0x00000000 0x0 0x08000000>;\n'
        '\t\t};\n')
t, n = re.subn(r'(reserved-memory \{\n(?:\t+[^\n]*\n)*?\t+ranges;\n)',
               r'\1' + node, t, count=1)
assert n == 1, "reserved-memory block not found"
t, n = re.subn(r'(imx95-neutron@4ab00004 \{\n)',
               r'\1\t\t\tmemory-region = <&neutron_test_mem>;\n', t, count=1)
assert n == 1, "neutron node not found"
t, n = re.subn(r'\n\t+iommus = <0x[0-9a-f]+ 0x0?d>;', '', t, count=1)
assert n == 1, "neutron iommus not found"
open(dst, "w").write(t)
print("dtb patched: carveout + memory-region + iommus dropped")
PY
"$DTC" -I dts -O dtb -o "$WORK/neutron.dtb" "$WORK/neutron.dts" 2>/dev/null

# --- initramfs: busybox + firmware + TFLite app + delegate + .so closure --------
zcat "$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz" | \
    (cd "$STAGE" && cpio -idmu 2>/dev/null)
mkdir -p "$STAGE/lib/firmware" "$STAGE/usr/lib/firmware"
cp "$FW" "$STAGE/lib/firmware/NeutronFirmware.elf"
cp "$FW" "$STAGE/usr/lib/firmware/NeutronFirmware.elf"
cp "$CONVERTED_MODEL" "$STAGE/model.tflite"

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
shutil.copy2(TFL, os.path.join(STAGE, "benchmark_model"))
shutil.copy2(DELEGATE, os.path.join(STAGE, "neutron_delegate.so"))
print("staged libs:", len(done))
PY

# Make the optional in-guest VERIFY command available to the init script.
printf '%s' "$VERIFY_CMD" > "$STAGE/verify_cmd"

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
echo "=== NEUTRON-CONV ==="
echo "dev: $(ls /dev/neutron* 2>/dev/null | tr '\n' ' ')"
chmod +x /benchmark_model 2>/dev/null
# Verbose so the delegate prints how many nodes it claimed (delegate kernels).
/benchmark_model --graph=/model.tflite --external_delegate_path=/neutron_delegate.so \
    --num_runs=1 --warmup_runs=0 --verbose=true > /tmp/bm.log 2>&1
echo "benchmark rc=$?"
# Surface the delegation summary + any neutron lines for the assertion + report.
grep -iE 'delegate|neutron|kernel|partition|node|inference' /tmp/bm.log | tail -20
VC=$(cat /verify_cmd 2>/dev/null)
if [ -n "$VC" ]; then
    echo "--- VERIFY ---"
    sh -c "$VC"; echo "verify rc=$?"
fi
echo "=== NEUTRON-CONV-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

# -m 4G so the @4 GiB carveout is backed.
timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk -m 4G -display none \
    -kernel "$IMAGE" -dtb "$WORK/neutron.dtb" -initrd "$WORK/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "=== neutron converted-model report ==="
sed -n '/NEUTRON-CONV ===/,/NEUTRON-CONV-DONE/p' "$LOG" | grep -vE '^\[ *[0-9].*\]'

fail() { echo "FAIL: $*"; exit 1; }

grep -qa '/dev/neutron'  "$LOG" || fail "/dev/neutron0 absent (driver did not bind)"
grep -qa 'benchmark rc=0' "$LOG" || fail "benchmark_model did not complete (rc!=0)"
# Anomaly check. Match kernel fault signatures only: a bare case-insensitive
# "timeout" also hits benign boot chatter ("SCMI max-rx-timeout: 5000ms", the
# mali "*_TIMEOUT is capped ..." lines), which every boot prints - so it used to
# fail unconditionally the moment a converted fixture was supplied.
grep -qaE 'external abort|Unhandled fault|Internal error|Kernel panic|Oops|BUG:' "$LOG" \
    && fail "kernel fault during the converted-model run"
grep -qaiE 'neutron.*(timed out|send timeout)|rproc.*timed out' "$LOG" \
    && fail "neutron mailbox timeout during the converted-model run"

# Delegation assertion: the delegate must have CLAIMED the NeutronGraph node, not
# fallen back to CPU. TFLite/LiteRT prints "... executed by the delegate w/ N
# delegate kernels"; a converted model offloads N>=1, the stock model offloads 0.
# (The exact string can vary by delegate/benchmark version - if it changes, widen
# this grep; the captured report above shows what actually printed.)
KERNELS=$(grep -oiE 'w/ +[0-9]+ +delegate kernels' "$LOG" | grep -oE '[0-9]+' | head -1)
if [ -n "${KERNELS:-}" ]; then
    [ "$KERNELS" -ge 1 ] || fail \
        "delegate claimed 0 nodes - is the model actually neutron-converted?"
    echo "delegate kernels: $KERNELS"
elif ! grep -qaiE 'NeutronGraph|neutron.*node|delegate.*partition' "$LOG"; then
    fail "no sign the NeutronGraph node was offloaded (CPU fallback?); see report"
fi

# Optional correctness gate (stage 2): only when a VERIFY_CMD was supplied.
if [ -n "$VERIFY_CMD" ]; then
    grep -qa 'verify rc=0' "$LOG" || fail "VERIFY_CMD failed (output mismatch)"
    echo "PASS: converted model offloaded to Neutron AND output verified"
    exit 0
fi

echo "PASS: converted model reaches the NPU end to end" \
     "(NeutronGraph offloaded -> /dev/neutron0 -> mailbox RUN -> DONE)."
echo "NOTE: output correctness is NOT checked here - it needs the execution" \
     "engine + a golden (Tier-1 qtest, or set VERIFY_CMD). See" \
     "docs/neutron-full-execution-integration.md."
exit 0
