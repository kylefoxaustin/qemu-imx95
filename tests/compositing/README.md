# i.MX 95 DPU display compositing — end-to-end harness (WIP)

`run.sh` is the validation harness for display-side **compositing**: the DPU's
LayerBlend chain composites multiple DRM planes (primary + overlays) into one
scanned-out frame. It boots Linux, uses libdrm `modetest` to set a primary plane
plus an overlay plane at a known position, captures the emulated DPU display via
**QMP screendump**, and (eventually) asserts the overlay composited over the
primary at the right spot — the i.MX 93's proven screendump approach.

`hw/misc/imx95_dpu.c` currently scans out only the primary FetchLayer plane (the
boot logo). Compositing means walking the LayerBlend chain
(`ExtDst0 -> LB(plane) -> … -> ConstFrame`, each LayerBlend a 2-input Porter-Duff
blend with position + alpha) and compositing the planes. Set
`IMX95_DPU_TRACE_PIPE=1` (the harness does) to dump the display-pipeline register
writes for reverse-engineering that routing.

## Status — working

The stock EVK dtb ships the display output disabled (LDB / LVDS-PHY / pixel-link
/ pixel-interleaver all `status="disabled"`, no panel), so the dpu95 component
aggregation found no CRTC/connector. The harness decompiles the dtb, enables that
whole output chain and attaches a fixed 1280x800 LVDS panel, and recompiles —
purely a dtb change (the 0-stub LVDS CSR is enough for the LDB/PHY to bind). With
it the connector `LVDS-1` registers and `modetest` sets the 1280x800 mode.

`modetest -a` then commits a **primary (full-screen SMPTE) + a 320x240 overlay
at (200,150)** atomically, and `hw/misc/imx95_dpu.c` walks the LayerBlend chain
(`ConstFrame0 -> LB1[+primary] -> LB2[+overlay@200,150] -> ExtDst0 -> FrameGen0`)
to composite the planes. The QMP screendump shows the overlay's test pattern
over the SMPTE bars at the right position; the harness asserts that (a near-black
overlay pixel inside the 320x240 bbox the bright SMPTE bars never produce) and
PASSES. Opaque blend only for now — per-pixel alpha-blend is TODO.

## Running

```
KBUILD=<kernel build>  SM_ELF=<m33_image.elf>  \
BSP_ROOTFS=<imx-image-full rootfs>   ./tests/compositing/run.sh
```

SKIPs if `modetest` is not in `BSP_ROOTFS`, or if no CRTC/connector is present.
