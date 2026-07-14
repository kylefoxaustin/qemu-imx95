#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 DPU 2D blit engine - real-app end-to-end validation via NXP G2D.
#
# Boots the stock NXP g2d_basic_test against the real libg2d-dpu backend, which
# opens the dpu95 DRM render node and submits Command Sequencer cmdlists to our
# hw/misc/imx95_dpu.c blit engine:
#
#   g2d_basic_test -> libg2d-dpu (G2D_RGBA8888 blit/copy)
#       -> /dev/dri/renderD* (dpu95 DRM render node, IMX_DPU_SET_CMDLIST ioctl)
#       -> our imx95.dpu blit model -> destination buffer + completion IRQ
#
# g2d_basic_test self-verifies its copy/cache results, so a clean run proves the
# datapath end to end (the qtest only drives a synthetic cmdlist). dpu95 is
# built into the BSP kernel, so no modules are staged; the only external input
# is the g2d userspace, harvested (readelf closure) from an imx-image-full BSP
# rootfs. If the rootfs is absent the test SKIPs.
#
# Required (override via env): QEMU, KBUILD (Image+dtb), SM_ELF, BSP_ROOTFS.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
BSP_ROOTFS=${BSP_ROOTFS:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx95-drone-sizer/tmp/work/imx95_19x19_lpddr5_evk-poky-linux/imx-image-full/1.0/rootfs}
IMAGE=$KBUILD/arch/arm64/boot/Image
DTB=$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb
TMO=${TMO:-180}

need() { [ -e "$2" ] || { echo "missing $1: $2"; exit 1; }; }
need QEMU "$QEMU"; need IMAGE "$IMAGE"; need DTB "$DTB"; need SM_ELF "$SM_ELF"

G2D_TEST="$BSP_ROOTFS/opt/g2d_samples/g2d_basic_test"
if [ ! -x "$G2D_TEST" ]; then
    echo "SKIP: no G2D userspace at $G2D_TEST"
    echo "      (set BSP_ROOTFS to an imx-image-full rootfs with g2d_samples)"
    exit 0
fi

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/root"; mkdir -p "$STAGE"
LOG=${LOG:-$WORK/serial.log}
QERR=${QERR:-$WORK/qemu.err}

# aarch64 busybox (the host one is x86); dpu95 is built into the BSP kernel.
zcat "$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz" | \
    (cd "$STAGE" && cpio -idmu 2>/dev/null)

# Harvest g2d_basic_test + its recursive DT_NEEDED closure (libg2d-dpu, libdrm,
# libc) from the BSP rootfs (readelf reads aarch64 ELFs on an x86 host).
python3 - "$BSP_ROOTFS" "$STAGE" <<'PY'
import os, sys, subprocess, shutil, glob
RF, STAGE = sys.argv[1], sys.argv[2]
libdirs = [f"{RF}/lib", f"{RF}/lib64", f"{RF}/usr/lib", f"{RF}/usr/lib/aarch64-linux-gnu"]
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
    dst = os.path.join(STAGE, os.path.relpath(src, RF))   # keep NEEDED name
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if not os.path.exists(dst):
        shutil.copy2(src, dst)
    for n in needed(src):
        copy_into(find(n))
roots = [f"{RF}/lib/ld-linux-aarch64.so.1",
         f"{RF}/opt/g2d_samples/g2d_basic_test"]
for r in roots:
    copy_into(r)
# the g2d ELF interpreter is /usr/lib/ld-linux-..., not /lib.
ld = os.path.join(STAGE, "lib/ld-linux-aarch64.so.1")
if os.path.exists(ld):
    os.makedirs(os.path.join(STAGE, "usr/lib"), exist_ok=True)
    shutil.copy2(ld, os.path.join(STAGE, "usr/lib/ld-linux-aarch64.so.1"))
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
echo "=== G2D-E2E ==="
echo "dri nodes: $(ls /dev/dri/ 2>/dev/null | tr '\n' ' ')"
# g2d_basic_test self-verifies its copy + cache_op results. It also runs a 2D
# blend "clear mode" test (not in the copy+fill first cut), which prints a
# per-pixel warning; summarise in-guest so the serial log stays small.
/opt/g2d_samples/g2d_basic_test -s 256x256 -f rgba-rgba -t 1 > /tmp/g2d.log 2>&1
echo "rc=$?"
echo "copy_fail=$(grep -c 'not copied' /tmp/g2d.log)"
echo "cache_fail=$(grep -c 'cache_op error' /tmp/g2d.log)"
echo "blits_ran=$(grep -c 'RGBA->RGBA' /tmp/g2d.log)"
echo "blend_clear_warn=$(grep -c 'not zero in clear mode' /tmp/g2d.log)"
echo "=== G2D-E2E-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

# Set BLIT_TRACE=1 to capture the blit command-stream trace (verbose) to $QERR.
TRACE_ENV=""; TRACE_ARG=""
[ "${BLIT_TRACE:-0}" = 1 ] && { TRACE_ENV="IMX95_DPU_TRACE=1"; TRACE_ARG="-d unimp"; }
env $TRACE_ENV timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    $TRACE_ARG -serial file:"$LOG" -serial null >/dev/null 2>"$QERR" || true

echo "=== g2d blit e2e report ==="
sed -n '/G2D-E2E ===/,/G2D-E2E-DONE/p' "$LOG" | grep -vE '^\[ *[0-9].*\]'

get() { grep -a "^$1=" "$LOG" | tail -1 | cut -d= -f2 | tr -d '\r\n '; }
pass=1
[ "$(get rc)" = 0 ]         || { echo "  MISS g2d_basic_test rc=0"; pass=0; }
[ "$(get copy_fail)" = 0 ]  || { echo "  MISS g2d_copy self-check failed"; pass=0; }
[ "$(get cache_fail)" = 0 ] || { echo "  MISS g2d_cache_op self-check failed"; pass=0; }
[ "$(get blits_ran)" -ge 1 ] 2>/dev/null || { echo "  MISS no RGBA blit ran"; pass=0; }
# g2d verifies Porter-Duff CLEAR: every pixel must be zero after a clear-mode
# blend. A non-zero count means the alpha-blend math is wrong.
[ "$(get blend_clear_warn)" = 0 ] || { echo "  MISS blend clear-mode non-zero"; pass=0; }

[ "$pass" = 1 ] && {
    echo "PASS: 2D blit copy + fill + alpha-blend validated through G2D"; exit 0; }
echo "FAIL"; exit 1
