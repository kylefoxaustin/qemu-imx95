#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Audio card registration bar.
#
# The 19x19 EVK runs three ASoC cards:
#   - wm8962-audio : sai3 <-> WM8962 codec (on lpi2c4), the headphone/spk path
#   - micfil-audio : the PDM microphone (MICFIL + dmic codec)
#   - bt-sco-audio : sai1 <-> a dummy BT-SCO codec
# The SAI/MICFIL FIFOs are serviced by eDMA: sai1 + micfil on edma1
# (0x44000000), sai3 on edma2 (0x42000000). This boots Linux, loads the audio
# drivers, and checks all three cards register.
#
# PASS = /proc/asound/cards lists btscoaudio, wm8962audio and micfilaudio.
#
# What the machine models (hw/audio/, hw/dma/): the eDMA management + channel
# pages (so the fsl-edma driver probes and allocates channels), register-file
# SAI + MICFIL front-ends (so fsl-sai/fsl-micfil probe and the DAIs register),
# and the WM8962 I2C codec (16-bit regmap, device-id 0x6243). The wm8962 supply
# rail is gated by a PCAL6408A expander at 0x21 on lpi2c4, also modelled.
# Sample movement is not driven (no QEMU audio backend) - this is the
# registration bar, not playback/capture.
#
# Artifacts (NXP BSP, not redistributable):
#   KERNEL/DTB - stock NXP Image + imx95-19x19-evk.dtb
#   SM_ELF     - System Manager firmware m33_image.elf
#   SND_MODDIR - the BSP module tree with the snd-soc-* modules (all =m):
#                snd-soc-fsl-{utils,sai,micfil,asoc-card}, imx-pcm-dma,
#                snd-soc-{wm8962,dmic,imx-audmux,imx-card}.

set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
ART=${IMX95_ARTIFACTS:-$HOME/imx95-artifacts}
KERNEL=${KERNEL:-$ART/linux-build/arch/arm64/boot/Image}
DTB=${DTB:-$ART/linux-build/arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb}
SM_ELF=${SM_ELF:-$ART/m33_image.elf}
BUSYBOX=${BUSYBOX:-$REPO/tests/busybox-initramfs}
SND_MODDIR=${SND_MODDIR:-}
TMO=${TMO:-120}

need() { [ -e "$1" ] || { echo "missing: $1 (set $2)" >&2; exit 2; }; }
need "$QEMU" QEMU; need "$KERNEL" KERNEL; need "$DTB" DTB; need "$SM_ELF" SM_ELF
[ -n "$SND_MODDIR" ] || { echo "set SND_MODDIR to the BSP sound module dir" >&2; exit 2; }

MODS="snd-soc-fsl-utils imx-pcm-dma snd-soc-fsl-sai snd-soc-fsl-micfil \
      snd-soc-wm8962 snd-soc-dmic snd-soc-imx-audmux snd-soc-fsl-asoc-card \
      snd-soc-imx-card"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
root="$WORK/root"; mkdir -p "$root"/{proc,sys,dev,mods}
zcat "$BUSYBOX/busybox-initramfs.cpio.gz" | (cd "$root" && cpio -idmu 2>/dev/null)
for m in $MODS; do
    f=$(find "$SND_MODDIR" -name "$m.ko" | head -1)
    need "$f" "$m.ko under SND_MODDIR"; cp "$f" "$root/mods/"
done
cat > "$root/init" <<INIT
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
for m in $MODS; do insmod /mods/\$m.ko 2>&1; done
sleep 3
echo "=== ASOUND-CARDS ==="
cat /proc/asound/cards
echo "=== ASOUND-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$root/init"
( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.cpio.gz"

LOG="$WORK/serial.log"
timeout "$TMO" "$QEMU" -M imx95-19x19-evk -m 2G -display none \
  -kernel "$KERNEL" -dtb "$DTB" -initrd "$WORK/initrd.cpio.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -device loader,file="$SM_ELF",cpu-num=6 \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "--- /proc/asound/cards ---"; sed -n '/ASOUND-CARDS/,/ASOUND-DONE/p' "$LOG" | grep -vE 'ASOUND'
ok=0
for c in btscoaudio wm8962audio micfilaudio; do grep -qa "$c" "$LOG" && ok=$((ok+1)); done
if [ "$ok" = 3 ]; then
    echo "PASS: all three ASoC cards registered (bt-sco, wm8962, micfil)"; exit 0
fi
echo "FAIL: only $ok/3 audio cards registered"; exit 1
