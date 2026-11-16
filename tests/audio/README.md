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

## Out of scope for `run.sh` (the registration bar)

`run.sh` attaches no QEMU audio backend and does not drive the SAI/MICFIL
FIFOs or the codec's FLL/DC-servo: a `STREAMON` binds the PCM but no samples
move. That is what this harness asserts — the cards enumerate and can be
opened and queried.

Moving samples is the job of the two harnesses beside it, which DO attach a
backend: `run-playback.sh` (square wave -> eDMA -> SAI FIFO -> byte-checked
wav) and `run-capture.sh` (MICFIL PDM to userspace). Both are Tier A in
`docs/validation/test-matrix.yaml`; see the known-broken note below for the
one thing playback does not do.

## Running

```sh
SND_MODDIR=<bsp-rootfs>/usr/lib/modules/<kver>/kernel/sound \
KERNEL=... DTB=... SM_ELF=... ./run.sh
```

All the `snd-soc-*` drivers are modules (`=m`); the BSP module tree supplies
them. Artifacts are not redistributable.

## A non-default rate never reaches the SAI

**Known broken (measured 2026-11-15, pre-existing).** `run-playback.sh` drives
two rates. The 48 kHz leg passes; the 16 kHz leg fails, and it fails in the way
that matters:

```
FAIL(rate=16000): the SAI enabled TX on a rate NO CODEC EVER ANNOUNCED.
      what the SAI actually saw:
         wm8962_rate ADDITIONAL_CONTROL_3=0x0010 -> 48000 Hz
FAIL(rate=16000): 2s of audio drained in ?s (want >= 1.5s)
```

A non-default rate does not reach the SAI. The guest opens the PCM at an exact
16000 — ALSA's `snd_pcm_set_params` rejects an inexact rate, and the frame
count is delivered — and the codec still announces 48000, so TX is enabled on a
rate no codec ever announced. At 48 kHz the SAI's hardcoded fallback **equals**
the right answer, so every timing check in the passing leg is satisfied without
proving the rate was honoured; that is why the 16 kHz leg exists.

Two things this does **not** say, because the harness does not show them: each
rate runs in its own boot (`play_rate=` on the kernel command line), so nothing
here exercises a rate change at *runtime*; and the drain-time check prints `?s`
rather than an elapsed figure, so "the stream drained early" is read from the
check failing, not from a measured duration.

This is `owner: model` in `docs/validation/test-matrix.yaml` — silicon honours
the rate — and it is recorded, not fixed. The playback row stays Tier A because
the 48 kHz data path *is* byte-verified; what is broken is reconfiguration.
