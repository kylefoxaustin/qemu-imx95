#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 JPEG decode - real-app end-to-end validation via GStreamer.
#
# This boots a real GStreamer pipeline and proves hw/misc/imx95_jpeg.c works
# through the WHOLE media stack, not just the qtest:
#
#   gst-launch-1.0 filesrc ! jpegparse ! v4l2jpegdec ! videoconvert ! filesink
#       -> the real mxc-jpeg V4L2 mem2mem driver
#       -> our imx95_jpeg model
#       -> a decoded NV12 frame
#
# It exercises the driver's buffer queueing / STREAMON / IRQ-completion and the
# NV12 output path that the BGR-only qtest never touches.
#
# Unlike the other tests this needs an external GStreamer userspace, which is
# NOT in the repo: it harvests gst-launch + the needed plugins + their shared-
# library closure (via readelf) from a BSP "imx-image-full" rootfs, and pairs
# them with OUR vermagic-matching mxc-jpeg modules from the kernel build (the
# rootfs's own modules are built for a different kernel). If the rootfs is not
# present the test SKIPs.
#
# Required (override via env):
#   QEMU        qemu-system-aarch64                (default: ../../build/...)
#   KBUILD      kernel build dir (Image + dtb + in-tree mxc-jpeg .ko)
#   SM_ELF      System Manager m33_image.elf
#   BSP_ROOTFS  an imx-image-full rootfs with gst-launch-1.0 (the GStreamer
#               userspace source). Default points at the drone-sizer BSP build.
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

if [ ! -x "$BSP_ROOTFS/usr/bin/gst-launch-1.0" ]; then
    echo "SKIP: no GStreamer userspace at BSP_ROOTFS=$BSP_ROOTFS"
    echo "      (set BSP_ROOTFS to an imx-image-full rootfs with gst-launch-1.0)"
    exit 0
fi

MXC_JPEG=$(find "$KBUILD" -name 'mxc-jpeg-encdec.ko' | head -1)
V4L2_JPEG=$(find "$KBUILD" -name 'v4l2-jpeg.ko' | head -1)
need "mxc-jpeg-encdec.ko (KBUILD)" "$MXC_JPEG"
need "v4l2-jpeg.ko (KBUILD)" "$V4L2_JPEG"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/root"; mkdir -p "$STAGE"
LOG=${LOG:-$WORK/serial.log}

# aarch64 busybox (the host one is x86) + the kernel modules + the test image.
zcat "$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz" | \
    (cd "$STAGE" && cpio -idmu 2>/dev/null)
cp "$MXC_JPEG" "$STAGE/mxc-jpeg-encdec.ko"
cp "$V4L2_JPEG" "$STAGE/v4l2-jpeg.ko"
cp "$HERE/test-256.jpg" "$STAGE/test.jpg"

# Harvest gst-launch + the plugins + their recursive DT_NEEDED closure from the
# BSP rootfs (readelf reads aarch64 ELFs on an x86 host).
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
    dst = os.path.join(STAGE, os.path.relpath(src, RF))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if not os.path.exists(dst):
        shutil.copy2(src, dst)
    for n in needed(src):
        copy_into(find(n))
roots = [f"{RF}/lib/ld-linux-aarch64.so.1",
         f"{RF}/usr/bin/gst-launch-1.0",
         f"{RF}/usr/libexec/gstreamer-1.0/gst-plugin-scanner"]
for pat in ["libgstvideo4linux2", "libgstjpegformat", "libgstjpeg.so",
            "libgstcoreelements", "libgstvideoconvertscale",
            "libgsttypefindfunctions", "libgstapp", "libgstvideoparsersbad",
            "libgstvideo-", "libgstpbutils", "libgstallocators"]:
    roots += glob.glob(f"{RF}/usr/lib/gstreamer-1.0/{pat}*")
    roots += glob.glob(f"{RF}/usr/lib/{pat}*")
for r in roots:
    copy_into(r)
# The gst-launch ELF interpreter is /usr/lib/ld-linux-..., not /lib.
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
export PATH=/bin:/usr/bin
export LD_LIBRARY_PATH=/lib:/usr/lib:/usr/lib/aarch64-linux-gnu
export GST_PLUGIN_PATH=/usr/lib/gstreamer-1.0
export GST_PLUGIN_SCANNER=/usr/libexec/gstreamer-1.0/gst-plugin-scanner
export GST_REGISTRY=/tmp/gst-registry.bin
exec > /dev/console 2>&1
sleep 7
echo "=== GST-E2E ==="
insmod /v4l2-jpeg.ko 2>&1
insmod /mxc-jpeg-encdec.ko 2>&1
echo "video nodes: $(ls /dev/video* 2>/dev/null | tr '\n' ' ')"
gst-launch-1.0 -v filesrc location=/test.jpg ! jpegparse ! v4l2jpegdec ! \
    videoconvert ! filesink location=/tmp/out.raw 2>&1 | \
    grep -iE 'EOS|ERROR|width or height|not-negotiated|Setting pipeline'
echo "out.raw size: $(/bin/busybox stat -c%s /tmp/out.raw 2>/dev/null)"
echo "Y-plane[0..7]: $(/bin/busybox od -An -tu1 -N8 /tmp/out.raw 2>/dev/null)"
echo "=== GST-E2E-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
    -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -device loader,file="$SM_ELF",cpu-num=6 \
    -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "=== gstreamer e2e report ==="
sed -n '/GST-E2E ===/,/GST-E2E-DONE/p' "$LOG" | grep -vE '^\[ *[0-9].*\]|GST-E2E ==='

pass=1
grep -qa 'Got EOS' "$LOG" && echo "  ok   pipeline reached EOS" || { echo "  MISS pipeline EOS"; pass=0; }
# 256x256 NV12 = 256*256*3/2 = 98304 bytes.
grep -qa 'out.raw size: 98304' "$LOG" && echo "  ok   decoded a 256x256 NV12 frame" || { echo "  MISS NV12 frame size"; pass=0; }
# Solid RGB(200,50,80) -> luma Y ~= 98; the NV12 Y plane starts with it.
y=$(grep -a 'Y-plane' "$LOG" | sed 's/.*://' | grep -oE '[0-9]+' | head -1)
{ [ "${y:-0}" -ge 96 ] && [ "${y:-0}" -le 100 ]; } \
    && echo "  ok   decoded content correct (Y=$y ~ 98)" \
    || { echo "  MISS Y-plane luma ($y)"; pass=0; }

[ "$pass" = 1 ] && { echo "PASS: JPEG decode validated through GStreamer + real driver"; exit 0; }
echo "FAIL"; exit 1
