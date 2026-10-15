<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# camera → ISP chain: raw Bayer from the sensor, developed out of DRAM

```
host .raw (RGGB mosaic)
  → QEMU imx95.isi "frames" property
  → ov5640 / MIPI CSI-2 / ISI                 what a raw sensor would deliver
  → ISI DMA into a V4L2 capture buffer        the frame is now in DRAM
  → VIDIOC_EXPBUF → dmabuf
  → queued as neoisp-input0                   the ISP reads THAT buffer
  → NeoISP develop → neoisp-frame             developed colour out
```

## Why this test exists

Two harnesses each proved half of this and neither proved the join:

- `tests/isp-develop/` feeds the ISP from a **file**, so no captured frame ever
  reaches it.
- `tests/camera-to-display/` captures a frame but deliberately keeps the ISP
  **out** of the path — a smart camera emitting already-developed YUYV — so
  that a transport fault cannot hide behind a resample that still looks like a
  photograph.

Both halves were tagged. The clause joining them was not, which is exactly the
defect shape where every value is correct and the sentence built from them is
unsupported. A vision pipeline *is* that clause.

## There is no ISI → ISP wire, and there should not be

On i.MX 95 silicon the ISI writes DRAM and the NeoISP — a V4L2 **mem2mem**
device — reads DRAM. **The join is memory.** The two models in QEMU are
correspondingly unconnected; grepping for a data path between
`hw/display/imx95_isi.c` and `hw/display/imx95_neoisp.c` finds only `#include`
lines. That is fidelity, not a gap. The gap was that nothing ever travelled the
whole way.

## Why dmabuf and not a memcpy

A userspace `memcpy` from the capture buffer into the ISP's input buffer would
prove transport, but it would put our own hands on every byte — so it could not
distinguish *"the ISP read what the ISI wrote"* from *"we re-typed what the ISI
wrote into a buffer the ISP happened to read"*. Exporting the ISI's capture
buffer with `VIDIOC_EXPBUF` and queueing that exact dmabuf as `neoisp-input0`
makes the two devices share one physical buffer, which is the claim being made.

Both queues use `vb2_dma_contig_memops`, so the export is one physically
contiguous region and the ISP's single `IMG0_IN_ADDR` can address it. A
scatter-gather exporter would need model work; this one does not.

## The two gates

1. **Correlation.** Each developed frame is correlated per channel against the
   RGB its mosaic was sampled from, gating on the weakest channel
   (`tests/isp-develop/checkdev.py`). Per-channel matters: a wrong Bayer phase
   swaps red and blue, which a luma-only check barely notices.
2. **Provenance.** Each developed frame is matched to *which* staged frame it
   came from, by hashing the captured Bayer, and must correlate against **that
   frame's own** reference. At least two distinct sources must appear, so a
   pipeline that develops one frame and re-serves it cannot pass.

The ISI cycles its host-frame source on its own timer, so the guest cannot
choose which frame it receives — hence match-by-hash rather than assuming
frame *N* is file *N*.

## Things that cost time here

- **The Bayer phase does not come from the fourcc.** The `neoisp` driver's
  format table has no phase field — its `SRGGB8` and `SBGGR8` entries are
  byte-identical. `DEMOSAIC_CTRL.FMT` comes from the **params** buffer, so a
  zero-filled params buffer means `FMT=0`, which the model reads as RGGB. The
  staged mosaic is generated RGGB to match. The fourcc still governs stride and
  link validation, just not which colour sits at pixel (0,0).
- **The geometry is 1280×720 because the sensor says so, not by preference.**
  Raw Bayer is 8bpp, and `ov5640_try_fmt_internal` forces any 8bpp mode below
  1280 wide up to 720p:

  ```c
  /* - 8bpp modes work for resolution >= 1280x720 */
  if (bpp == 8 && mode->width < 1280)
          mode = &ov5640_mode_data[OV5640_MODE_720P_1280_720];
  ```

  So a 640×480 Bayer request returns a 1280×720 sensor format, every
  downstream sink is then set to a geometry nothing agrees with, and the only
  visible symptom is `-EPIPE` at `STREAMON`. The program checks the readback
  and names the rule instead.
- **Width must be a multiple of 32.** The driver sets `.align = 32` on the
  Bayer formats. A non-aligned width gets a padded `bytesperline`, and the ISI
  model derives bytes-per-pixel as `pitch / width` — so padding makes it
  misread the row stride, and the failure then looks like a debayer bug. The
  harness asserts `bytesperline == width` and says so by name.
- **Take the lowest-numbered ISI pipe.** There are eight
  `mxc_isi.<n>.capture` nodes and the crossbar routes the camera to pipe 0.
  Grabbing whichever one `readdir` yields first gets a node that opens,
  accepts the format and exports dmabufs perfectly — and then fails
  `STREAMON` with `Failed to enable pipe 7`, because nothing is routed to it.
- **The developed frames come out over 9p, not base64 on the console.** At
  1280×720 an RGB32 frame is 3.7 MB; two of them as base64 is ~10 MB of
  byte-at-a-time MMIO through the UART model.
- **The ISI falls back to a synthetic gradient** if a staged frame is the wrong
  size for the negotiated geometry, and warns. Capture then looks perfectly
  healthy while the image is not yours, so the harness treats that warning as a
  hard failure rather than noise.
- **Set the sensor's format first, then propagate to every subdev SINK pad —
  the crossbar's included.** `v4l2_subdev_link_validate` compares width,
  height, code *and* field order across every enabled link and fails
  `STREAMON` with `-EPIPE` on any mismatch. The crossbar propagates sink →
  source along its **active route**: its routed sink is what moves its source
  pad, and its other sinks answer `EINVAL`, which is correct and not an error.
  Leave the crossbar out and its source pad stays at the 1920×1080 power-on
  default while the ISI sink sits at the negotiated geometry — `-EPIPE`, with
  every format *this test can print* in agreement. Writing its **source** pads
  is the thing that breaks routing; sinks are safe. The ISI pipe propagates
  sink → source by itself (`mxc_isi_pipe_set_fmt`), so no source pad anywhere
  needs writing.
- **`-EPIPE` is mute unless the kernel was built with
  `CONFIG_DYNAMIC_DEBUG=y`.** Every report on this path — `mc-entity`'s
  per-pad and per-link validation, and `v4l2_subdev_link_validate_default`,
  which is what actually returns `-EPIPE` and which names *which* field
  disagreed — is a `dev_dbg`. The NXP BSP defconfig has dynamic debug off, so
  those are compiled out and `STREAMON` fails with no explanation at all.
  `run.sh` turns the relevant sites on in the guest (`DYNDBG=1`, the default)
  and raises the console loglevel; with a BSP kernel it prints
  `DYNDBG-UNAVAILABLE` and the failure is a bare errno again. One line of
  kernel output ends what no amount of userspace printing can:

  ```
  v4l2_subdev_link_validate_default: width does not match (source 1920, sink 640)
  link was "crossbar":5 -> "mxc_isi.0":0
  Link 'crossbar':5 -> 'mxc_isi.0':0 failed validation: -32
  ```
- **The userspace link oracle cannot see this class of fault.**
  `tests/camera/v4l2_cap.c links` compares each link's two ends through
  `G_FMT` — and `G_FMT` on a crossbar source pad answers *from the route*, so
  the oracle reads agreement where the kernel's validator finds 1920 against
  640. It is still useful for the mismatches it can see; it is not an
  authority on `-EPIPE`.
- **Assemble the ISP job from ONE node group.** The driver registers eight
  groups of six identically-named nodes. Picking per-name across groups leaves
  no group with a complete set, `prepare_job` never becomes ready, and `DQBUF`
  blocks forever with no error anywhere.
- **Feed `params` *and* `stats`.** Both are `MEDIA_LNK_FL_ENABLED` by default
  and `prepare_job` refuses to schedule while any enabled node lacks a queued
  buffer — silently.
- **`STREAMON` before any `QBUF`.** vb2 hands pre-queued buffers to the driver
  only after `start_streaming` returns, but this driver schedules a job from
  inside `start_streaming`. A buffer queued first is invisible, the job runs
  with a NULL output buffer, and the hardware is programmed with output address
  0: one trigger, no frame.
- **Dequeue the ISP's own buffers each iteration.** `input0`, `params` and
  `stats` have one buffer each; without dequeuing them the second iteration has
  nothing to queue.

## Running it

```
QEMU=…/build/qemu-system-aarch64 tests/camera-isp-chain/run.sh
```

Needs `KBUILD` (kernel `Image`, base dtb, `dtc`, `fdtoverlay`, `neoisp.ko`,
`ov5640.ko`), `SM_ELF`, an `INITRD`
(`tests/busybox-initramfs/build.sh` makes one), a `CROSS` gcc and python3
Pillow. It **SKIPs** cleanly if any is missing.

Knobs: `W` `H` (width 32-aligned), `NFRAMES`, `PHASE`, `SRC1`, `SRC2`,
`KEEP_DTB`, `TMO`, `DYNDBG` (default 1).

A/B knobs, each restoring one thing this program used to do, so a regression
can be attributed instead of guessed: `CHAIN_SET_SOURCES`,
`CHAIN_SKIP_CROSSBAR`, `CHAIN_ONE_PIPE`, `CHAIN_NO_CAP_SFMT`,
`CHAIN_NO_EXPBUF`, `CHAIN_NO_SENSOR_FMT`, `CHAIN_CODE`, `CHAIN_FOURCC`,
`CHAIN_CAPTURE_ONLY`, and `USE_V4L2CAP=1`, which runs the known-good capture
program inside this harness — the control that should have been run first.
