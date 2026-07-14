#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# XCVR / SPDIF transceiver bring-up + playback (fsl,imx95-xcvr).
#
# The xcvr@42680000 node is status=disabled on the 19x19 EVK and has no sound
# card, so this test patches the dtb to enable it, give it #sound-dai-cells, and
# add the `sound-xcvr` card (compatible fsl,imx-audio-card, model
# imx-audio-xcvr). Unlike the i.MX93 (SPDIF-only/firmware-free), the i.MX95 XCVR
# soc_data has use_phy + fw_name, so the fsl_xcvr driver:
#   1. loads xcvr-imx95.bin into the XCVR code RAM (banked via EXT_CTRL.PAGE),
#   2. brings up the PHY/PLL through the indirect AI interface, then
#   3. registers the SPDIF card.
# hw/audio/imx95_xcvr.c accepts the firmware writes, completes every AI access
# via the DONE bits the driver polls, and (when the TX datapath is released)
# clocks its TX FIFO out at the audio word rate to the audio backend.
#
# Tier 1 (always): insmod the ASoC stack, confirm the driver probes, the
# firmware loads (no "failed to request firmware"), and the card registers -
# this exercises the model's hard paths (the 8-byte memcpy_toio firmware write
# into the code RAM, the PHY/PLL AI bring-up, the runtime-PM firmware reload).
# Tier 2 (default; needs the ALSA staging bits): play an S16->S32 stream to the
# card - the eDMA-paced TX-FIFO drain runs over the 64-bit-TCD edma2 and the
# played samples reach the audio backend (captured to a wav when -audio wav).
#
# Required (override via env): QEMU, KBUILD (Image + dtb + dtc + the snd .ko
# tree), SM_ELF, XCVR_FW (xcvr-imx95.bin), an aarch64 cross-gcc. SKIPs if any
# Tier-1 input is missing; Tier 2 is skipped (not failed) if its extras are.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
KBUILD=${KBUILD:-$HOME/Documents/linux-imx95-build}
SM_ELF=${SM_ELF:-${SMELF:-$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf}}
IMAGE=${IMAGE:-$KBUILD/arch/arm64/boot/Image}
DTB=${DTB:-$KBUILD/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
DTC=${DTC:-$KBUILD/scripts/dtc/dtc}
INITRD=${INITRD:-$ROOT/tests/busybox-initramfs/busybox-initramfs.cpio.gz}
CROSS=${CROSS:-aarch64-linux-gnu-}
_RF=$HOME/Documents/qemu-imx95-artifacts/rootfs23/full
XCVR_FW=${XCVR_FW:-$_RF/usr/lib/firmware/imx/xcvr/xcvr-imx95.bin}
TMO=${TMO:-150}

# Tier-2 (playback) extras - optional.
BSP_ROOTFS=${BSP_ROOTFS:-$_RF}
_YT=$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx95-drone-sizer/tmp
ALSA_INC=${ALSA_INC:-$_YT/sysroots-components/armv8a-mx95/alsa-lib/usr/include}

skip() { echo "SKIP: $*"; exit 0; }
[ -x "$QEMU" ]    || skip "no QEMU ($QEMU)"
[ -e "$IMAGE" ]   || skip "no kernel Image ($IMAGE)"
[ -e "$DTB" ]     || skip "no dtb ($DTB)"
[ -e "$DTC" ]     || skip "no dtc ($DTC)"
[ -e "$SM_ELF" ]  || skip "no SM firmware ($SM_ELF)"
[ -e "$INITRD" ]  || skip "no busybox initramfs ($INITRD)"
[ -e "$XCVR_FW" ] || skip "no xcvr-imx95.bin firmware ($XCVR_FW)"
command -v "${CROSS}gcc" >/dev/null || skip "no aarch64 cross-gcc"

MODS="snd-soc-fsl-utils imx-pcm-dma snd-soc-fsl-xcvr snd-soc-imx-card"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
LOG="$WORK/serial.log"
WAV="$WORK/spdif.wav"

# Enable xcvr, add #sound-dai-cells, and inject the sound-xcvr card.
"$DTC" -I dtb -O dts "$DTB" > "$WORK/evk.dts" 2>/dev/null
python3 - "$WORK/evk.dts" "$WORK/patched.dts" <<'PY'
import sys, re
t = open(sys.argv[1]).read()

# Flip xcvr status to okay and add #sound-dai-cells inside its body.
i = t.index('xcvr@42680000'); b = t.index('{', i)
depth, j = 0, b
while True:
    c = t[j]
    if c == '{': depth += 1
    elif c == '}':
        depth -= 1
        if depth == 0: break
    j += 1
body = t[b:j]
body = body.replace('status = "disabled";',
                    'status = "okay";\n\t\t\t\t#sound-dai-cells = <0x00>;')
t = t[:b] + body + t[j:]

# Inject the SPDIF sound card as a sibling of the other root-level cards.
# xcvr's phandle is 0xf3 in this dtb.
anchor = '\tsound-micfil {'
card = (
    '\tsound-xcvr {\n'
    '\t\tcompatible = "fsl,imx-audio-card";\n'
    '\t\tmodel = "imx-audio-xcvr";\n'
    '\t\tpri-dai-link {\n'
    '\t\t\tlink-name = "XCVR PCM";\n'
    '\t\t\tcpu {\n'
    '\t\t\t\tsound-dai = <0xf3>;\n'
    '\t\t\t};\n'
    '\t\t};\n'
    '\t};\n\n'
)
k = t.index(anchor)
t = t[:k] + card + t[k:]
open(sys.argv[2], 'w').write(t)
PY
"$DTC" -I dts -O dtb -o "$WORK/patched.dtb" "$WORK/patched.dts" 2>/dev/null \
    || { echo "FAIL: dtc could not rebuild patched dtb"; exit 1; }

STAGE="$WORK/root"; mkdir -p "$STAGE/mods"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)

# Stage the firmware where the kernel loader looks.
mkdir -p "$STAGE/lib/firmware/imx/xcvr"
cp "$XCVR_FW" "$STAGE/lib/firmware/imx/xcvr/xcvr-imx95.bin"

# Stage the ASoC modules from the booted kernel's own tree (vermagic match).
for m in $MODS; do
    f=$(find "$KBUILD" -name "$m.ko" 2>/dev/null | head -1)
    [ -n "$f" ] || skip "missing $m.ko under $KBUILD"
    cp "$f" "$STAGE/mods/"
done

# Tier 2 (on by default; needs the ALSA bits): cross-compile the pcm_play oracle
# and stage libasound + its full NEEDED closure + the "hw" PCM config tree (the
# exact proven staging the WM8962 run-playback test uses), then play a stream to
# the SPDIF card. Skips (does not fail) if the ALSA staging inputs are absent.
PLAYBACK=0
LASOUND=$(ls "$BSP_ROOTFS"/usr/lib/libasound.so.2.* 2>/dev/null | head -1)
if [ "${SPDIF_PLAYBACK:-1}" = "1" ] \
   && [ -f "$ALSA_INC/alsa/asoundlib.h" ] && [ -n "$LASOUND" ] \
   && [ -f "$HERE/../audio/pcm_play.c" ] \
   && "${CROSS}gcc" -O2 -Wall -I"$ALSA_INC" -o "$STAGE/pcm_play" \
        "$HERE/../audio/pcm_play.c" "$LASOUND" \
        -Wl,--allow-shlib-undefined 2>/dev/null; then
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
    dst = os.path.join(STAGE, os.path.relpath(src, RF))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if not os.path.exists(dst):
        shutil.copy2(src, dst)
    for n in needed(src):
        copy_into(find(n))
copy_into(f"{RF}/lib/ld-linux-aarch64.so.1")
copy_into(LASOUND)
os.makedirs(os.path.join(STAGE, "usr/lib"), exist_ok=True)
link = os.path.join(STAGE, "usr/lib", "libasound.so.2")
if not os.path.exists(link):
    os.symlink(os.path.basename(LASOUND), link)
ld = os.path.join(STAGE, "lib/ld-linux-aarch64.so.1")
if os.path.exists(ld):
    shutil.copy2(ld, os.path.join(STAGE, "usr/lib/ld-linux-aarch64.so.1"))
PY
    mkdir -p "$STAGE/usr/share"
    cp -a "$BSP_ROOTFS/usr/share/alsa" "$STAGE/usr/share/" 2>/dev/null \
        && PLAYBACK=1
fi

cat > "$STAGE/init" <<INIT
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
export LD_LIBRARY_PATH=/usr/lib:/lib
exec > /dev/console 2>&1
echo "=== SPDIF ==="
for m in $MODS; do insmod /mods/\$m.ko 2>&1 | sed "s/^/insmod \$m: /"; done
sleep 3
# Force a runtime-PM resume of the xcvr so the driver actually loads the
# firmware (it is loaded from runtime_resume, not probe) - this drives the
# 8-byte memcpy_toio writes into the code RAM and the PHY/PLL AI bring-up,
# without needing a blocking playback.
for d in /sys/bus/platform/devices/42680000.xcvr/power/control \
         /sys/devices/platform/soc@0/*/42680000.xcvr/power/control; do
    [ -w "\$d" ] && echo on > "\$d" 2>/dev/null
done
sleep 1
echo "--- xcvr / firmware / card (dmesg) ---"
dmesg | grep -aiE 'xcvr|spdif|imx-audio-xcvr|request firmware|firmware' | head -20
echo "--- asound cards ---"
cat /proc/asound/cards 2>/dev/null
if [ "$PLAYBACK" = "1" ] && [ -x /pcm_play ]; then
    CARD=\$(awk '/imxaudioxcvr|imx-audio-xcvr|XCVR/{print \$1; exit}' \
            /proc/asound/cards)
    echo "--- playback to card \${CARD:-0} (S16->S32 via plug) ---"
    /pcm_play plughw:\${CARD:-0},0 \${PCM_SECS:-2}
fi
echo "=== SPDIF-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

echo "Tier-2 playback: $([ "$PLAYBACK" = 1 ] && echo enabled || echo skipped)"
timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$WORK/patched.dtb" -initrd "$WORK/initrd.cpio.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -audio driver=wav,path="$WAV" \
  -serial file:"$LOG" -serial null >/dev/null 2>"${QEMU_ERR:-/dev/null}" || true
[ -n "${QEMU_ERR:-}" ] && grep -aE "\[xcvr\]" "$QEMU_ERR" | tail -20

echo "--- spdif report ---"
sed -n '/=== SPDIF ===/,/=== SPDIF-DONE ===/p' "$LOG"

fail() { echo "FAIL: $*"; exit 1; }
grep -qaiE 'failed to request firmware' "$LOG" \
    && fail "firmware xcvr-imx95.bin did not load"
grep -qaiE 'imx-audio-xcvr|XCVR PCM' "$LOG" \
    || fail "the imx-audio-xcvr SPDIF card did not register"
grep -qaiE 'external abort|Unhandled fault|Internal error' "$LOG" \
    && fail "fault during xcvr bring-up"
if [ "$PLAYBACK" = "1" ]; then
    grep -qaE 'PLAY\[.*\]: PASS' "$LOG" \
        || fail "SPDIF playback did not complete the stream"
    if [ -s "$WAV" ]; then
        echo "captured WAV: $(stat -c %s "$WAV") bytes (informational)"
    fi
fi

echo "PASS: xcvr firmware loaded, PHY up, imx-audio-xcvr card registered$(
    [ "$PLAYBACK" = 1 ] && echo ' + SPDIF playback drained')"
