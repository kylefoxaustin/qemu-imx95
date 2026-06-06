# Audio card registration (done-at-bar)

The 19x19 EVK runs three ALSA/ASoC sound cards:

| card           | path                                   | DMA                |
| -------------- | -------------------------------------- | ------------------ |
| `wm8962-audio` | sai3 ↔ WM8962 codec (lpi2c4 @0x1a)     | edma2 (0x42000000) |
| `micfil-audio` | MICFIL PDM mic ↔ dmic codec            | edma1 (0x44000000) |
| `bt-sco-audio` | sai1 ↔ dummy BT-SCO codec              | edma1 (0x44000000) |

`run.sh` boots Linux, loads the audio drivers, and asserts all three register
in `/proc/asound/cards`:

```
 0 [btscoaudio  ]: simple-card   - bt-sco-audio
 1 [wm8962audio ]: fsl-asoc-card - wm8962-audio
 2 [micfilaudio ]: micfil-audio  - micfil-audio
```

## What the machine models

- **eDMA** (`hw/dma/imx95_edma.c`) — the management block + per-channel pages,
  so the `fsl-edma` driver probes and the SAI/MICFIL DAIs can allocate DMA
  channels. edma1 (`fsl,imx93-edma3`, 31 ch, 0x10000 stride) serves sai1 +
  micfil; edma2 (`fsl,imx95-edma5`, 64 ch, 0x8000 stride) serves sai3.
- **SAI / MICFIL** (`hw/audio/imx95_sai.c`, `imx95_micfil.c`) — register-file
  front-ends carrying a correct VERID/PARAM so `fsl-sai`/`fsl-micfil` probe and
  register their DAIs.
- **WM8962** (`hw/audio/wm8962.c`) — the I2C codec: 16-bit regmap, device-id
  `SOFTWARE_RESET == 0x6243`. Its supply rail (`reg_audio_pwr`) is gated by a
  PCAL6408A expander at 0x21 on **lpi2c4** (a real master), so both are
  modelled or the codec stays in deferred probe.

## Out of scope

Playback / capture. No QEMU audio backend is attached; the SAI/MICFIL FIFOs and
the codec's FLL/DC-servo are not driven. A `STREAMON` binds the PCM but no
samples move. This is the registration bar — the cards enumerate and can be
opened/queried, not played.

## Running

```sh
SND_MODDIR=<bsp-rootfs>/usr/lib/modules/<kver>/kernel/sound \
KERNEL=... DTB=... SM_ELF=... ./run.sh
```

All the `snd-soc-*` drivers are modules (`=m`); the BSP module tree supplies
them. Artifacts are not redistributable.
