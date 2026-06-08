# i.MX 95 DPU 2D blit engine — real-app end-to-end test

`run.sh` validates the DPU 2D blit engine (`hw/misc/imx95_dpu.c`) through the
whole NXP G2D stack, not just the qtest:

```
g2d_basic_test  ->  libg2d-dpu  ->  /dev/dri/renderD* (dpu95 blit render node)
                ->  Command Sequencer cmdlist  ->  our imx95.dpu blit model
                ->  destination buffer + ComCtrl completion IRQ
```

`g2d_basic_test` self-verifies its `g2d_copy` and `g2d_cache_op` results, so a
clean run proves the copy datapath end to end (the kernel-free
`tests/qtest/imx95-blit-test.c` only drives a synthetic cmdlist).

## What it checks

- `g2d_copy` self-check passes (`copy_fail=0`) — a same-format RGBA copy round
  trips correctly through the model.
- `g2d_cache_op` self-check passes (`cache_fail=0`).
- at least one RGBA blit ran against the model.
- the Porter-Duff **clear-mode blend** zeroes every pixel (`blend_clear_warn=0`)
  — g2d verifies this, so it validates the alpha-blend (BlitBlend9) math.

## Colour conversion — `run-convert.sh`

`run-convert.sh` validates the blit **colour-space converter** with the
`g2d_convert.c` exerciser: it drives an `RGBA8888 -> YUYV` blit and back
(`YUYV -> RGBA8888`) and checks the round trip reproduces the four quadrant
colours (4:2:2 chroma subsampling is lossless in the flat interiors). A
conversion routes the source through FetchDecode9 with the Store9 source-select
reading the colour-space-converter output (`0xe100c == 0x06`); the model does the
RGBA<->YUYV BT.601 conversion. `g2d_basic_test`'s own `YUY2->NV12` self-check is
`#if G2D_OPENCL` and so is compiled out of the DPU build, which is why this needs
its own exerciser.

`run-nv12.sh` + `g2d_nv12.c` extend the same idea to **NV12** (2-plane 4:2:0):
RGBA->NV12 writes Y to Store9 plane 0 (BASEADDRESS0, 8bpp) and interleaved Cb,Cr
to Store9 plane 1 (BASEADDRESS1, half-res); NV12->RGBA reads Y from FetchDecode9
and the chroma plane from FetchEco9 (0xa0000). The RGBA->NV12->RGBA round trip
reproduces the quadrant colours. The CSC dispatch picks the conversion by the
src/dst bit-depths (32<->16 = YUYV, 32<->8 = NV12) and falls through to a plain
copy when the formats match (so a stale CSC select can't swallow a copy). Other
YUV formats (I420/YV12/YUYV variants) are not modelled.

## Scaling — `run-xform.sh`

`run-xform.sh` validates the blit **scaler**. It cross-compiles a small g2d
exerciser (`g2d_scale_rot.c`, self-contained — inlined libg2d ABI, links
straight against the rootfs `libg2d.so.2`) that drives a **2× upscale**
(64×64 → 128×128, four solid colour quadrants) plus a 90° rotation, then reads
the destination back and self-checks. The model detects an HScaler9/VScaler9 in
the pipeline (pixengcfg CLKEN) and nearest-neighbour scales FetchDecode9's source
rect to the Store9 destination rect; the harness asserts `SCALE: PASS`.

Rotation is **not modelled yet** (reported informationally): libg2d implements a
G2D rotate as a sequence of tiled FetchRot9 strips (each with its own source base
and destination offset, no single rotate flag), which the blit model does not
replay. Needs an aarch64 cross compiler (`CC=`, default `aarch64-linux-gnu-gcc`);
SKIPs if absent or no BSP `libg2d`.

```
BSP_ROOTFS=<imx-image-full rootfs>  CC=aarch64-linux-gnu-gcc \
KBUILD=<kernel build>  SM_ELF=<m33_image.elf>   ./tests/blit/run-xform.sh
```

## Running

`dpu95` is built into the BSP kernel, so no modules are staged. The only
external input is the G2D userspace, harvested (readelf closure) from an
`imx-image-full` BSP rootfs. If the rootfs is absent the test SKIPs.

```
BSP_ROOTFS=<imx-image-full rootfs>   # has /opt/g2d_samples/g2d_basic_test
KBUILD=<kernel build>  SM_ELF=<m33_image.elf>   ./tests/blit/run.sh
```

Set `BLIT_TRACE=1` to capture the blit command-stream trace (verbose) to
`$QERR` for debugging the cmdlist / register decode.
