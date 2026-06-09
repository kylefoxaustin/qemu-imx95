#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# MICFIL (PDM microphone) capture - real ALSA datapath end to end.
#
# Boots Linux, loads the ASoC drivers, and runs the pcm_capture oracle against
# the MICFIL card. The MICFIL model synthesises a sawtooth into its data FIFO
# and pulses a DMA request as the FIFO passes its watermark; a cyclic (ESG)
# eDMA1 channel reads MICFIL DATACH0 into the ALSA ring buffer one minor loop
# per request. So a real snd_pcm_readi() returns non-silent, varying S32
# samples - the capture path actually moves data, not just registers the card.
#
#   pcm_capture -> snd_pcm_readi (S32_LE)
#       -> fsl-micfil / snd-soc-imx-card  -> /dev/snd/pcmC?D0c
#       -> eDMA1 cyclic channel (DATACH0 -> ring, our hw/dma/imx95_edma.c)
#       <- MICFIL FIFO synth + dma-req     (our hw/audio/imx95_micfil.c)
#
# PASS = "CAP[...]: PASS (non-silent)".
#
# There is no arecord in a busybox initramfs, so this cross-compiles the tiny
# pcm_capture oracle against an ALSA sysroot and harvests libasound (+ its libc
# closure) and the dynamic loader out of a BSP rootfs. The snd-soc-* modules are
# taken from the BOOTED kernel's own build tree so their vermagic matches Image.
# SKIPs if any artifact is missing.
#
# Required (override via env): QEMU, KBUILD (Image + dtb + the snd .ko tree),
# SM_ELF, BSP_ROOTFS (libasound + loader + libc), ALSA_INC (asoundlib.h).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=${IMAGE:-$KBUILD/arch/arm64/boot/Image}
DTB=${DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
_YT=$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx95-drone-sizer/tmp
_YW=$_YT/work/imx95_19x19_lpddr5_evk-poky-linux
BSP_ROOTFS=${BSP_ROOTFS:-$_YW/imx-image-full/1.0/rootfs}
ALSA_INC=${ALSA_INC:-$_YT/sysroots-components/armv8a-mx95/alsa-lib/usr/include}
BUSYBOX=${BUSYBOX:-$ROOT/tests/busybox-initramfs}
CROSS=${CROSS:-aarch64-linux-gnu-}
TMO=${TMO:-150}

skip() { echo "SKIP: $*"; exit 0; }
[ -x "$QEMU" ]        || skip "no QEMU ($QEMU)"
[ -e "$IMAGE" ]       || skip "no kernel Image ($IMAGE)"
[ -e "$DTB" ]         || skip "no dtb ($DTB)"
[ -e "$SM_ELF" ]      || skip "no SM firmware ($SM_ELF)"
[ -d "$BSP_ROOTFS" ]  || skip "no BSP rootfs ($BSP_ROOTFS) for libasound/loader"
[ -e "$ALSA_INC/alsa/asoundlib.h" ] || skip "no ALSA headers ($ALSA_INC)"
command -v "${CROSS}gcc" >/dev/null  || skip "no ${CROSS}gcc"
LASOUND=$(ls "$BSP_ROOTFS"/usr/lib/libasound.so.2.* 2>/dev/null | head -1)
[ -n "$LASOUND" ]     || skip "no libasound in BSP rootfs"

MODS="snd-soc-fsl-utils imx-pcm-dma snd-soc-fsl-sai snd-soc-fsl-micfil \
      snd-soc-wm8962 snd-soc-dmic snd-soc-imx-audmux snd-soc-fsl-asoc-card \
      snd-soc-imx-card"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/root"; mkdir -p "$STAGE"/{proc,sys,dev,mods}
zcat "$BUSYBOX/busybox-initramfs.cpio.gz" | (cd "$STAGE" && cpio -idmu 2>/dev/null)

# Cross-compile the capture oracle against the ALSA sysroot + the BSP libasound.
"${CROSS}gcc" -O2 -Wall -I"$ALSA_INC" -o "$STAGE/pcm_capture" \
    "$HERE/pcm_capture.c" "$LASOUND" -Wl,--allow-shlib-undefined \
    || skip "could not cross-compile pcm_capture"

# Harvest libasound + its DT_NEEDED closure (libc, ...) and the dynamic loader.
python3 - "$BSP_ROOTFS" "$STAGE" "$LASOUND" <<'PY'
import os, sys, subprocess, shutil, glob
RF, STAGE, LASOUND = sys.argv[1], sys.argv[2], sys.argv[3]
libdirs = [f"{RF}/lib", f"{RF}/lib64", f"{RF}/usr/lib"]
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
copy_into(f"{RF}/lib/ld-linux-aarch64.so.1")
copy_into(LASOUND)
# libasound is referenced by its soname; add the soname symlink.
soname = "libasound.so.2"
os.makedirs(os.path.join(STAGE, "usr/lib"), exist_ok=True)
link = os.path.join(STAGE, "usr/lib", soname)
if not os.path.exists(link):
    os.symlink(os.path.basename(LASOUND), link)
# Some toolchains emit the interpreter as /usr/lib/ld-linux-...; mirror it.
ld = os.path.join(STAGE, "lib/ld-linux-aarch64.so.1")
if os.path.exists(ld):
    shutil.copy2(ld, os.path.join(STAGE, "usr/lib/ld-linux-aarch64.so.1"))
print("staged libs:", len(done))
PY

# libasound resolves the "hw" PCM plugin from its config tree - stage it.
mkdir -p "$STAGE/usr/share"
cp -a "$BSP_ROOTFS/usr/share/alsa" "$STAGE/usr/share/" 2>/dev/null \
    || skip "no /usr/share/alsa in BSP rootfs"

# Stage the snd-soc modules from the booted kernel's own tree (vermagic match).
for m in $MODS; do
    f=$(find "$KBUILD" -name "$m.ko" 2>/dev/null | head -1)
    [ -n "$f" ] || skip "missing $m.ko under $KBUILD"
    cp "$f" "$STAGE/mods/"
done

cat > "$STAGE/init" <<INIT
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
export LD_LIBRARY_PATH=/usr/lib:/lib
for m in $MODS; do insmod /mods/\$m.ko 2>&1 | sed "s/^/insmod \$m: /"; done
sleep 3
echo "=== /proc/asound/cards ==="
cat /proc/asound/cards
MCARD=\$(awk '/micfil/{print \$1}' /proc/asound/cards 2>/dev/null | head -1)
echo "micfil card = \${MCARD:-NONE}"
echo "=== MICFIL-CAPTURE ==="
if [ -n "\$MCARD" ]; then
    for ch in 2 8; do
        echo "--- pcm_capture hw:\$MCARD,0 S32 \${ch}ch ---"
        /pcm_capture hw:\$MCARD,0 4096 S32 \$ch && break
    done
else
    echo "CAP: no micfil card"
fi
echo "=== MICFIL-CAPTURE-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

LOG="$WORK/serial.log"
timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.cpio.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "--- capture report ---"
sed -n '/MICFIL-CAPTURE ===/,/MICFIL-CAPTURE-DONE/p' "$LOG" | grep -vE 'CAPTURE ==='
if grep -qaE 'CAP\[.*\]: PASS \(non-silent\)' "$LOG"; then
    echo "PASS: MICFIL PDM capture delivered non-silent samples to userspace"
    exit 0
fi
echo "FAIL: no non-silent MICFIL capture (see log)"; exit 1
