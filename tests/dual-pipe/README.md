# i.MX 95 DPU second pixel pipeline (CRTC 1) — end-to-end harness

`run.sh` validates the DPU's **second pixel pipeline**. The Socionext display
controller has two display streams: stream 0 (FrameGen0, the boot-logo CRTC,
already modelled) and stream 1 (FrameGen1). `hw/misc/imx95_dpu.c` now models both
— two QEMU consoles, two independent LayerBlend→ExtDst→FrameGen compositing
chains (each rooted at its own ConstFrame), and the **two display interrupt
blocks**: stream 0 on disp_irq0 (→ irqsteer 64/70/73/74 → GIC SPI 215) and
stream 1 on disp_irq2 (→ irqsteer 192/198/201/202 → GIC SPI 217).

The stock EVK dtb ships both LVDS output channels disabled with no panels, so
dpu95 finds only the single stream-0 path. The harness enables **both** LDB
channels (ch0 → lvds0-phy, ch1 → lvds1-phy) and attaches a panel to each, so
dpu95 brings up **two CRTCs each with a connector** (LVDS-1 and LVDS-2). One
libdrm `modetest` then sets a mode + full-screen plane on each in a single atomic
commit.

## What it checks

- Both connectors enumerate: **LVDS-1** (CRTC 0) and **LVDS-2** (CRTC 1).
- `modetest` sets a mode on **CRTC 51** (the 2nd pipeline) with a plane.
- The atomic commit **completes** (modetest measures vsync, `freq:`), with no
  `Atomic Commit failed` and no flip/shadow-load timeout.

The commit on CRTC 51 only completes because the model drives stream 1's
vblank/shadow-load interrupts through **disp_irq2**; without them the commit
would fall through dpu95's ~10 s flip_done/SHDLD timeouts. So a clean commit is
the proof the second pipeline (and its interrupt path) works.

The model composites and scans out both pipelines (the frame tick re-composites
each active stream's console). The harness also QMP-screendumps head 0 and head 1
and reports their brightness **informationally only** — in the headless
(`-display none`) dual-console setup the screendump targets a different console
object than the one the model scans out, so the captured pixels are not part of
the gate.

## Running

```
KBUILD=<kernel build>  SM_ELF=<m33_image.elf>  \
BSP_ROOTFS=<imx-image-full rootfs>   ./tests/dual-pipe/run.sh
```

SKIPs if `modetest` is not in `BSP_ROOTFS`.
