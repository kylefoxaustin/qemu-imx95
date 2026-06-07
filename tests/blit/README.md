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

Format conversion is not modelled; `g2d_basic_test` exercises it too (as
performance runs g2d does not self-verify), so the model skips those rather than
corrupt the destination.

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
