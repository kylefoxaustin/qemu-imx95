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

## Status — connector up; multi-plane composite is the next step

The display-output prerequisite is **solved**. The stock EVK dtb ships the
display output disabled (LDB / LVDS-PHY / pixel-link / pixel-interleaver all
`status="disabled"`, no panel), so the dpu95 component aggregation found no
CRTC/connector. The harness decompiles the dtb, enables that whole output chain
and attaches a fixed 1280x800 LVDS panel, and recompiles — purely a dtb change,
no model change (the 0-stub LVDS CSR is enough for the LDB/PHY drivers to bind).
With it the connector `LVDS-1` registers, `modetest` sets the 1280x800 mode, the
DPU scans the primary plane out (the boot logo renders at 1280x800), and the
dpu95 LayerBlend pipeline (ConstFrame0 -> LayerBlend1 -> ExtDst0 -> FrameGen0)
is programmed and captured by the trace. The harness PASSES on that.

**Next:** drive a real multi-plane commit (a primary + an overlay plane) and
teach `hw/misc/imx95_dpu.c` to walk the LayerBlend chain and composite the
planes (it currently scans out only the primary FetchLayer). The routing is
mapped from the trace: each LayerBlend's PIXENGCFG dynamic gives PRIM_SEL[5:0] /
SEC_SEL[13:8] (link IDs), BLENDCONTROL the alpha, POSITION the offset.

## Running

```
KBUILD=<kernel build>  SM_ELF=<m33_image.elf>  \
BSP_ROOTFS=<imx-image-full rootfs>   ./tests/compositing/run.sh
```

SKIPs if `modetest` is not in `BSP_ROOTFS`, or if no CRTC/connector is present.
