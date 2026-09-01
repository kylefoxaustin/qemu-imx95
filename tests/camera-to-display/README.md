# camera-to-display — a picture in over CSI-2, out on the LCD

The i.MX95 vision **transport** path, end to end, with no ISP in it:

```
smart camera (already-developed YUYV)   the ISI host frame source
  -> MIPI CSI-2 receiver                hw/display/imx95_isi.c
  -> ISI -> DRAM capture buffer         DMA, byte-for-byte
  -> V4L2 DQBUF  (v4l2_to_fb)
  -> /dev/fb0 -> DPU -> LVDS panel      what Holobench shows in its framebuffer pane
```

**"Smart camera"** means the sensor emits developed YUV rather than Bayer, so the
NeoISP is deliberately *not* in this path. That is the point: it isolates the
transport, so a failure here cannot hide behind image processing. Flowing raw
Bayer through a real debayer is the next step, not this one.

## Two independent proofs, because either alone is weak

1. **Bytes.** The guest hashes the captured frame (FNV-1a); the host hashes what
   it fed in, at the same stride. Equal hashes mean the image crossed the
   pipeline *intact*, not merely that something arrived.
2. **Pixels.** A `screendump` of the panel is correlated against the source
   image. A correct hash with a black screen is still a broken display path, and
   only the second check catches it.

Result: `r=1.0000`, luma MAE 0.5, hashes equal.

> **The hash does not certify the frame; it certifies the bytes you chose to
> feed it.** With a 3840-byte stride carrying 1280 bytes of pixel, a hash over
> the wrong extent comes out clean while the image is sheared. The check is
> sound and its *scope* is the thing that silently moves.

## Running it

```sh
QEMU=…/qemu-system-aarch64 ./run.sh          # uses scene.png
SRC_IMG=~/my-photo.jpg ./run.sh              # or any image you like
SHOT=/tmp/panel.ppm ./run.sh                 # keep the screendump
HOLD=1 ./run.sh                              # hold the frame up (board-farm pane)
KEEP_DTB=out.dtb ./run.sh                    # emit the camera+panel dtb
```

Needs the usual operator-supplied pieces (kernel `Image`, base dtb, `dtc`,
`fdtoverlay`, `ov5640.ko`, SM firmware) plus python3-Pillow; it SKIPs cleanly
without them.

## Notes worth keeping

- The capture node is **multiplanar** (`V4L2_CAP_VIDEO_CAPTURE_MPLANE`). The
  single-planar `G_FMT` returns `EINVAL` — the `_MPLANE` API is required.
- The pipeline negotiates **640×480 YUYV with a 3840-byte line stride** — six
  bytes of stride per two bytes of pixel. `mkframe.py` honours whatever the
  driver reports rather than assuming packed lines.
- The media graph's links must be enabled and the sensor format propagated onto
  every crossbar sink before `STREAMON` validates. `tests/camera/v4l2_cap.c`
  already does that in `cap` mode, so this test runs it first rather than
  duplicating the logic.
- The frame is centred on the panel (640×480 at +320+160 of 1280×800), and
  stays that way deliberately. Scaling to full panel would put a resample back
  between capture and scanout — the very class of step that can hide a transport
  fault by smearing it into something that still looks like a photograph — and
  it would also destroy the known-black surround that makes the "99.5%
  non-black" check meaningful rather than tautological.
- A board-farm pane that polls (Holobench screendumps on a ~1.5 s timer, and the
  guest cannot trigger it) will **miss** a blit that lands and exits inside that
  window — and miss it silently, since a black pane looks exactly like a dead
  camera path. `HOLD=1` holds the final frame for that case.
