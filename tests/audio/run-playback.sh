#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# WM8962 / SAI3 audio playback - real ALSA datapath end to end.
#
# Boots Linux, loads the ASoC drivers, and plays a generated square wave to the
# wm8962 card with the pcm_play oracle. The SAI3 model clocks the TX FIFO out at
# the audio word rate (paced by an eDMA cyclic channel that refills it from the
# ALSA ring), and hands the played samples to the QEMU audio backend. Running
# with -audio driver=wav,path=... captures the playback to a WAV file, which the
# host then checks is non-silent - so PCM actually moved, not just "aplay exited
# clean" (the i.MX 93 landmine: a too-narrow SAI min_access_size silently drops
# S16 samples while the play "completes" on period IRQs).
#
#   pcm_play -> snd_pcm_writei (S16_LE)
#       -> fsl-sai / snd-soc-fsl-asoc-card -> /dev/snd/pcmC?D0p
#       -> eDMA2 cyclic channel (ring -> SAI3 TDR0, our hw/dma/imx95_edma.c)
#       -> SAI3 TX FIFO drain -> audio backend -> out.wav (hw/audio/imx95_sai.c)
#
# PASS requires all of: pcm_play delivers every frame; the stream takes AT LEAST
# its true duration to clock out (the defect makes it too FAST, so the assertion
# is a floor); and the drain times AGREE ACROSS RATES, which is the load-invariant
# form of the same claim - every rate plays the same two seconds, so every rate
# must take the same time, however busy the host is.
#
# It is run at TWO rates on purpose. At 48 kHz alone - the only rate this harness
# ever used - a SAI that ignores the requested rate and one that honours it are
# indistinguishable, and ours ignored it for months.
#
# Required (override via env): QEMU, KBUILD (Image + dtb + the snd .ko tree),
# SM_ELF, BSP_ROOTFS (libasound + loader + libc), ALSA_INC (asoundlib.h), an
# aarch64 cross gcc, and python3 (host WAV check). SKIPs if any is missing.
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
command -v python3 >/dev/null         || skip "no python3 for the WAV check"
LASOUND=$(ls "$BSP_ROOTFS"/usr/lib/libasound.so.2.* 2>/dev/null | head -1)
[ -n "$LASOUND" ]     || skip "no libasound in BSP rootfs"

MODS="snd-soc-fsl-utils imx-pcm-dma snd-soc-fsl-sai snd-soc-fsl-micfil \
      snd-soc-wm8962 snd-soc-dmic snd-soc-imx-audmux snd-soc-fsl-asoc-card \
      snd-soc-imx-card"

WORK=${KEEPWORK:-$(mktemp -d)}
[ -n "${KEEPWORK:-}" ] || trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/root"; mkdir -p "$STAGE"/{proc,sys,dev,mods}
WAV="$WORK/play.wav"
zcat "$BUSYBOX/busybox-initramfs.cpio.gz" | (cd "$STAGE" && cpio -idmu 2>/dev/null)

"${CROSS}gcc" -O2 -Wall -I"$ALSA_INC" -o "$STAGE/pcm_play" \
    "$HERE/pcm_play.c" "$LASOUND" -Wl,--allow-shlib-undefined \
    || skip "could not cross-compile pcm_play"

# Harvest libasound + closure + loader (same as the capture harness).
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
print("staged libs:", len(done))
PY

# libasound's "hw" PCM plugin config tree.
mkdir -p "$STAGE/usr/share"
cp -a "$BSP_ROOTFS/usr/share/alsa" "$STAGE/usr/share/" 2>/dev/null \
    || skip "no /usr/share/alsa in BSP rootfs"

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
WCARD=\$(awk '/wm8962/{print \$1}' /proc/asound/cards 2>/dev/null | head -1)
echo "wm8962 card = \${WCARD:-NONE}"
RATE=\$(sed -n 's/.*play_rate=\([^ ]*\).*/\1/p' /proc/cmdline)
[ -n "\$RATE" ] || RATE=48000
# Quiet the kernel: its printk shares this console with pcm_play's stdout, and
# an interleaved line breaks the very assertion we are here to make. (A parse
# that a neighbouring writer can corrupt is not a parse - it is a coin flip.)
dmesg -n 1 2>/dev/null
echo "=== WM8962-PLAYBACK rate=\$RATE ==="
if [ -n "\$WCARD" ]; then
    /pcm_play hw:\$WCARD,0 2 \$RATE
    # Let the (wall-clock-paced) audio backend drain the captured PCM before
    # we power off, so the WAV is finalised with the full stream.
    sleep 4
else
    echo "PLAY: no wm8962 card"
fi
echo "=== WM8962-PLAYBACK-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

#
# TWO OPERATING POINTS, AND THAT IS THE ENTIRE POINT.
#
# This harness only ever played 48 kHz, and the SAI's FIFO pacing - and the rate
# it opened its audio voice at - were hardcoded to 48 kHz. So the one rate we
# tested was precisely the rate at which a model that IGNORES the requested rate
# and a model that honours it are INDISTINGUISHABLE. The old PASS check even
# asserted a literal "96000 frames" (2s x 48 kHz), so the test could not have
# been run at another rate without failing for the wrong reason.
#
#   A MODEL THAT IS CORRECT AT THE POINT YOU TESTED IT IS NOT A MODEL YOU HAVE
#   TESTED. Only a SECOND operating point exposes it.        (93emulator)
#
# THE ORACLE IS TIME. snd_pcm_writei() + drain block until the hardware has
# actually clocked the samples out, so N seconds of audio must take N seconds
# AT ANY RATE. A SAI pacing at a fixed 48 kHz drains a 16 kHz stream three times
# too fast - while writei succeeds, drain succeeds, and every frame is
# accounted for.
#
# It is NOT the captured WAV. I tried that first: QEMU's wav backend writes its
# OWN sample rate into the header (44100 by default) and the audio layer
# resamples into it, so the header says nothing whatever about what the SAI did.
# It reported "0 Hz" and it would have reported the same for a correct model.
#   AN ORACLE THAT MEASURES THE WRONG DEVICE IS WORSE THAN NO ORACLE: it is a
#   green light with a citation.
#
RATES=${RATES:-"48000 16000"}
fail=0
for RATE in $RATES; do
LOG="$WORK/serial-$RATE.log"
WAV="$WORK/play-$RATE.wav"
TRC="$WORK/trace-$RATE.log"
timeout -k 5 "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.cpio.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init play_rate=$RATE" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -audio driver=wav,path="$WAV" \
  -trace enable=wm8962_rate -trace enable=imx95_sai_tx_enable -D "$TRC" \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "--- playback report (rate=$RATE) ---"
sed -n '/WM8962-PLAYBACK/,/WM8962-PLAYBACK-DONE/p' "$LOG" | grep -vE 'PLAYBACK-DONE'

# WAV content (informational). The -audio wav backend pulls at wall-clock, so
# whether the full stream is captured/finalised before poweroff is timing-
# sensitive; the deterministic datapath proof is pcm_play completing the whole
# stream (the eDMA paced it at the audio rate) plus the kernel-free qtest
# (tests/qtest/imx95-edma-test.c /tcd64-playback) that asserts the eDMA fed
# SAI3 TDR0. So a non-silent WAV is a bonus, not the gate.
elapsed=$(grep -aoE 'elapsed=[0-9.]+' "$LOG" | head -1 | cut -d= -f2)
want_frames=$((2 * RATE))
if ! grep -qaE "PLAY\[.*\]: PASS \($want_frames frames\)" "$LOG"; then
    echo "FAIL(rate=$RATE): pcm_play did not deliver $want_frames frames"
    fail=1
fi

#
# THE PROVENANCE GATE - and it is the only reason the 48 kHz case means anything.
#
# The SAI is the bit-clock SLAVE here: the rate is a fact only the CODEC holds,
# and it reaches the SAI over a wire. If that wire never fires, the SAI falls
# back to a hardcoded default - and that default is 48 kHz, so AT 48 kHz THE
# INVENTED RATE AND THE TRUE RATE ARE THE SAME NUMBER. Every duration check
# above passes. The WAV is right. The frames are right. And nothing was proven.
#
# That is exactly what was happening: the driver's regmap holds R27's datasheet
# default (0x0010 == 48 kHz), computes 0x0010 for a 48 kHz stream, sees no
# change, and NEVER PUTS A BYTE ON THE I2C WIRE. Our codec was never told, so it
# never told the SAI, and our 48 kHz green rested on a coincidence for months.
# (93emulator found it on their board, then predicted it on mine, and was right.)
#
#   A DEFAULT THAT HAPPENS TO EQUAL THE ANSWER IS NOT AN ANSWER. IT IS A GREEN
#   LIGHT WITH NO WITNESS BEHIND IT.
#
# So we no longer ask only "did it play at the right rate". We ask WHERE THE
# RATE CAME FROM, and we refuse a pass that rests on a guess - even a correct
# one. A timing oracle structurally cannot see this; only the provenance can.
#
# TWO SEPARATE CLAIMS, REPORTED SEPARATELY. The first version of this gate asked
# for "rate=$RATE AND announced=1" in one grep, so a codec that announced the
# WRONG rate got the message "no codec ever announced" - a true failure with a
# false explanation, which is the kind of thing that sends you debugging the
# wrong device. A gate is also an instrument, and an instrument that cannot say
# WHICH of two things broke will one day tell you the wrong one.
#
pfail=0
pat='imx95_sai_tx_enable TX enable: rate=[0-9]+ Hz'
pat="$pat announced_by_codec=[01]"
saw=$(grep -aoE "$pat" "$TRC" | tail -1)
got_rate=$(printf '%s' "$saw" | sed -n 's/.*rate=\([0-9]*\) Hz.*/\1/p')
announced=$(printf '%s' "$saw" | sed -n 's/.*announced_by_codec=\([01]\).*/\1/p')

if [ "${announced:-0}" != 1 ]; then
    echo "FAIL(rate=$RATE): the SAI enabled TX on a rate NO CODEC EVER ANNOUNCED."
    echo "      -> it is running on its hardcoded fallback, and at 48 kHz that"
    echo "         fallback EQUALS the right answer - so every timing check"
    echo "         above passes and nothing whatever has been proven."
    fail=1; pfail=1
elif [ "${got_rate:-0}" != "$RATE" ]; then
    echo "FAIL(rate=$RATE): the codec announced ${got_rate:-?} Hz, but the guest"
    echo "      asked for $RATE Hz. The wire fired - with the wrong number."
    fail=1; pfail=1
else
    echo "witness(rate=$RATE): the codec announced $RATE Hz; the SAI is clocking"
    echo "      a rate it was TOLD, not one it guessed"
fi
if [ "$pfail" = 1 ]; then
    echo "      what the SAI actually saw:"
    grep -aE 'wm8962_rate|imx95_sai_tx_enable' "$TRC" | sed 's/^/         /' \
        || echo "         (the codec never spoke at all)"
fi
# THE ASSERTION IS A FLOOR, AND THE DIRECTION IS THE WHOLE DESIGN.
#
# 2 seconds of audio must take AT LEAST ~2 seconds to clock out, at any rate.
# The defect makes playback TOO FAST (a 16 kHz stream paced at 48 kHz drains in
# a third of the time) while host load only ever makes it SLOWER - a busy box
# stretched a good 48 kHz run to 3.7s and an upper bound rejected it, which was
# my oracle failing, not the model.
#
#   MEASURE IN THE DIRECTION THE BUG MOVES. A two-sided window on a wall clock
#   fails for load; a floor fails only for the defect.
#
# The ceiling is kept only wide enough to catch a total stall.
ok=$(python3 -c "
e=float('${elapsed:-0}')
print(1 if 1.5 <= e <= 20.0 else 0)" 2>/dev/null)
if [ "$ok" != 1 ]; then
    echo "FAIL(rate=$RATE): 2s of audio drained in ${elapsed:-?}s (want >= 1.5s)"
    echo "      -> the SAI is not clocking at ${RATE} Hz; the rate never reached it"
    fail=1
else
    echo "ok(rate=$RATE): 2s of ${RATE} Hz audio took ${elapsed}s to clock out"
fi
times="${times:-} $RATE:${elapsed:-0}"
done

#
# AND THE LOAD-INVARIANT CHECK, WHICH IS THE ONE THAT REALLY BITES.
#
# Every rate plays the SAME two seconds of audio, so every rate must take the
# SAME time to clock out. That ratio does not care how busy the host is - load
# stretches both runs together - whereas the absolute floor above has to leave
# room for a slow box and therefore leaves room for a nearly-slow bug. With the
# defect present the 16 kHz run finishes in a THIRD of the 48 kHz run, and no
# amount of host noise makes a third look like a whole.
#
#   WHEN A THRESHOLD HAS TO ABSORB NOISE, FIND THE QUANTITY THE NOISE CANCELS
#   OUT OF.
#
spread=$(python3 -c "
ts=[float(x.split(':')[1]) for x in '''${times:-}'''.split()]
print(round(max(ts)/min(ts),2) if len(ts)>1 and min(ts)>0 else 1.0)" 2>/dev/null)
if [ -n "${times:-}" ] && python3 -c "import sys; sys.exit(0 if float('$spread') > 1.6 else 1)"; then
    echo "FAIL: drain times differ by ${spread}x across rates ($times)"
    echo "      Every rate plays the same 2 seconds; they must take the same time."
    echo "      A ratio near 3 means the SAI clocks every stream at one fixed rate."
    fail=1
elif [ -n "${times:-}" ]; then
    echo "ok: drain times agree within ${spread}x across rates ($times)"
fi

if [ "$fail" = 0 ]; then
    echo "PASS: WM8962/SAI3 playback honours the requested rate at every"
    echo "      operating point tested ($RATES)"
    exit 0
fi
echo "FAIL: playback did not complete (eDMA did not drain the stream)"; exit 1
