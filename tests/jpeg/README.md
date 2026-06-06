# HW JPEG codec registration (done-at-bar)

The i.MX 95 has two dedicated hardware JPEG codecs, separate from the Wave6 VPU:

| block   | address      | driver / node                       |
| ------- | ------------ | ----------------------------------- |
| jpegdec | `0x4c500000` | `mxc-jpeg` → `/dev/video2` (decode) |
| jpegenc | `0x4c550000` | `mxc-jpeg` → `/dev/video3` (encode) |

Both are `fsl,imx9-jpgdec`/`fsl,imx9-jpgenc`, driven by the Linux `mxc-jpeg`
driver as V4L2 mem2mem devices.

`run.sh` boots Linux on the machine, loads the codec modules, and confirms both
register their V4L2 node. **PASS** prints:

```
mxc-jpeg 4c500000.jpegdec: decoder device registered as /dev/video2
mxc-jpeg 4c550000.jpegenc: encoder device registered as /dev/video3
```

## What the machine models

`mxc_jpeg_probe()` touches **no codec registers** — it ioremaps the window,
requests its IRQs, pulls the SCMI `VPU`/`VPUJPEG` clocks and the `PD_VPU` power
domain, and registers the m2m device. So the nodes register against the
existing machine (the SCMI clocks already serve the Wave6 VPU); no JPEG device
model is needed for the registration bar.

The machine does back the two MMIO windows (`hw/arm/fsl-imx95.c`, read-0 stubs)
so that a `STREAMON` — which writes the codec's `GLB_CTRL`/slot/config
registers — does **not** take a synchronous external abort and wedge the guest
(the same failure class as an unbacked PCIe window).

## Out of scope

Functional encode/decode. That needs the codec compute engine (JPEG
parse/Huffman/IDCT/colour-convert) plus its per-slot frame-done interrupt
(`jpegdec` GIC SPI 295.., `jpegenc` 291..). With the read-0 stub a `STREAMON`
binds but the queued buffer never completes (no frame-done IRQ), so userspace
times out — it does not crash.

## Running

```sh
JPEG_MODDIR=<bsp-rootfs>/usr/lib/modules/<kver>/kernel/drivers/media \
KERNEL=... DTB=... SM_ELF=... ./run.sh
```

`v4l2-jpeg.ko` (`CONFIG_V4L2_JPEG_HELPER=m`) and `mxc-jpeg-encdec.ko`
(`CONFIG_VIDEO_IMX8_JPEG=m`) come from the BSP module tree; the rest of V4L2
core is built in. Artifacts are not redistributable.
