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

## Status — blocked on display-output bringup

Currently **SKIPs**: the dpu95 DRM device binds but its component aggregation
reports `[drm] Cannot find any crtc` — with the stub LDB / pixel-link / panel
output chain there is no CRTC/connector for `modetest` to set a mode on, so no
multi-plane commit happens and there is nothing to composite. Getting a
connector up (modelling enough of the LDB → LVDS-panel output chain to register
one) is a display-output prerequisite, separate from the LayerBlend composite
logic. The QMP-screendump capture and the pipeline trace are in place and work;
they are gated on that prerequisite.

## Running

```
KBUILD=<kernel build>  SM_ELF=<m33_image.elf>  \
BSP_ROOTFS=<imx-image-full rootfs>   ./tests/compositing/run.sh
```

SKIPs if `modetest` is not in `BSP_ROOTFS`, or if no CRTC/connector is present.
