#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 DPU 2D blit engine - colour conversion end-to-end validation via NXP G2D.
#
# Sibling of run.sh (which validates copy/fill/blend). This one cross-compiles a
# small g2d exerciser (g2d_convert.c) that drives libg2d-dpu through an
# RGBA8888 -> YUYV conversion and back, then checks the round trip:
#
#   g2d_convert -> libg2d-dpu -> /dev/dri/renderD* (dpu95 SET_CMDLIST)
#       -> our imx95.dpu blit model (FetchRot9 colour conversion)
#       -> destination buffer; the program reads it back and prints PASS/FAIL.
#
# The exerciser is self-contained (inlined libg2d ABI) and links straight against
# the rootfs libg2d.so.2, so it needs only an aarch64 gcc + an imx-image-full BSP
# rootfs. SKIPs if either is absent.
#
# Required (override via env): QEMU, KBUILD (Image+dtb), SM_ELF, BSP_ROOTFS, CC.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
BSP_ROOTFS=${BSP_ROOTFS:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx95-drone-sizer/tmp/work/imx95_19x19_lpddr5_evk-poky-linux/imx-image-full/1.0/rootfs}
IMAGE=$KBUILD/arch/arm64/boot/Image
DTB=$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb
CC=${CC:-aarch64-linux-gnu-gcc}
TMO=${TMO:-180}

need() { [ -e "$2" ] || { echo "missing $1: $2"; exit 1; }; }
need QEMU "$QEMU"; need IMAGE "$IMAGE"; need DTB "$DTB"; need SM_ELF "$SM_ELF"

LIBG2D="$BSP_ROOTFS/usr/lib/libg2d.so.2"
if [ ! -e "$LIBG2D" ]; then
    echo "SKIP: no libg2d at $LIBG2D (set BSP_ROOTFS to an imx-image-full rootfs)"
    exit 0
fi
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "SKIP: no aarch64 cross compiler ($CC); set CC=... to build the exerciser"
    exit 0
fi

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/root"; mkdir -p "$STAGE/opt/g2d_samples"
LOG=${LOG:-$WORK/serial.log}
QERR=${QERR:-$WORK/qemu.err}

# Build the exerciser against the rootfs (libg2d.so.2 + libc); inlined ABI means
# no g2d dev header is needed. -l:libg2d.so.2 links the SONAME directly (the
# rootfs ships no libg2d.so linker symlink).
"$CC" --sysroot="$BSP_ROOTFS" -O2 -o "$STAGE/opt/g2d_samples/g2d_convert" \
    "$HERE/g2d_convert.c" -L"$BSP_ROOTFS/usr/lib" -l:libg2d.so.2 \
    || { echo "FAIL: could not cross-compile g2d_convert.c"; exit 1; }

# aarch64 busybox (the host one is x86); dpu95 is built into the BSP kernel.
zcat "$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz" | \
    (cd "$STAGE" && cpio -idmu 2>/dev/null)

# Harvest the exerciser + its recursive DT_NEEDED closure (libg2d-dpu, libdrm,
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
# the exerciser is staged (not under RF); seed the closure from its NEEDED libs.
EXE = os.path.join(STAGE, "opt/g2d_samples/g2d_convert")
copy_into(f"{RF}/lib/ld-linux-aarch64.so.1")
for n in needed(EXE):
    copy_into(find(n))
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
echo "=== G2D-CONVERT ==="
echo "dri nodes: $(ls /dev/dri/ 2>/dev/null | tr '\n' ' ')"
/opt/g2d_samples/g2d_convert > /tmp/x.log 2>&1
echo "rc=$?"
grep -aE 'RGB2YUV:|YUV2RGB:|G2D-CONVERT:' /tmp/x.log
echo "=== G2D-CONVERT-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

# Set BLIT_TRACE=1 to capture the blit command-stream trace (verbose) to $QERR.
TRACE_ENV=""
[ "${BLIT_TRACE:-0}" = 1 ] && TRACE_ENV="IMX95_DPU_TRACE=1 IMX95_DPU_TRACE_PIPE=1"
env $TRACE_ENV timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -serial file:"$LOG" -serial null >/dev/null 2>"$QERR" || true

echo "=== g2d colour-conversion e2e report ==="
sed -n '/G2D-CONVERT ===/,/G2D-CONVERT-DONE/p' "$LOG" | grep -vE '^\[ *[0-9].*\]'

pass=1
grep -aq 'RGB2YUV: PASS' "$LOG" || { echo "  MISS RGB->YUV self-check"; pass=0; }
grep -aq 'YUV2RGB: PASS' "$LOG" || { echo "  MISS YUV->RGB round-trip"; pass=0; }

[ "$pass" = 1 ] && {
    echo "PASS: 2D blit colour conversion validated through G2D"; exit 0; }
echo "FAIL"; exit 1
