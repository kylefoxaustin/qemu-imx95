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

## Porting this to another SoC

Ported to the i.MX 93 by the 93emulator session (PASS, r=0.9986,
`tests/camera-to-display/` in that tree). Three things diverged, and they are
the three to check first on any new part — none of them is a code change, all
of them are assumptions this README made without saying so.

- **Do not assume the panel needs a dtb splice.** On the 95 the camera dtb and
  the panel come from different places, so this test splices them. On the 93 a
  single prebuilt dtb carries both ends — `imx93-11x11-evk-mt9m114.dtb` gives
  `/dev/video0` *and* `/dev/fb0`, because the base EVK dtb already drives the
  adv7535 HDMI bridge and the camera variant is "base + camera". No overlay, no
  splice. Check for both nodes on a stock dtb before building any splice step.

- **Do not assume the geometry.** This README says 640×480 YUYV / bpl 3840; on
  the 93 the same sensor family negotiates **1280×720 YUYV, bpl 3840,
  sizeimage 2,764,800**. Staging frames at the wrong geometry is not a
  cosmetic error — it silently routes into the fallback path below. Read the
  geometry from `G_FMT` and stage to that, never to a number in a document.

- **⚠️ Do not assume the ISI model is loud about falling back.** This one is
  load-bearing. `hw/display/imx95_isi.c:164` prints *FALLING BACK TO THE
  SYNTHETIC GRADIENT* when the host frame source is unusable, so on the 95 an
  operator mistake announces itself. The 93's `hw/display/imx93_isi.c` serves
  its moving gradient **silently** on a short per-frame read — the capture then
  looks healthy *and varies between frames*, which defeats "is it moving?" as a
  sanity check.

  The host-staged-hash gate is what makes that failure honest: a gradient
  substitution hashes to no staged frame, so the test fails with *"hash matches
  NO staged frame"* instead of passing on synthetic pixels. On the 95 that gate
  is belt-and-braces. On the 93 it is the only thing standing between a wrong
  geometry and a green tick — and it did exactly that, catching a 640×480
  staging against a 720p pipeline.

  If you port to a part whose ISI is silent, keep the hash gate even if the
  pixel correlation looks convincing. Better still, make the model loud:
  `warn_report_once()` on the fallback, so the emulator never answers a
  question it wasn't asked without saying so.

  *Since fixed on the 93* (`ac5e35a4ebe`), verified by a mutation-style A/B —
  silent on the correct-geometry path, and on a deliberately-wrong 640×480 run
  it names the fault: *"a capture will look healthy but is NOT your frame"*.
  The hazard class outlives the instance, so the guidance stands: check whether
  the model announces its fallbacks before deciding how much the hash gate is
  carrying.

- Cross-check the FNV-1a constants **at runtime**, C against host Python, on the
  same bytes — not by eye. The 64-bit offset basis and prime appear at four
  sites across two languages, and two halves agreeing on the *same wrong*
  constant produces a confident PASS. The 93 port did this and both sides
  reported `a64eb7d880fe85c0`.

## ⚠️ Hashes from tags v2.5.0 and v2.6.0 cannot be reproduced

`imx95-v2.5.0` and `imx95-v2.6.0` shipped a **non-standard FNV-1a offset
basis** — `1469598103934665603` instead of `14695981039346656037`
(`0xcbf29ce484222325`). One digit short. Fixed on `main` and in
`imx95-v2.6.1`; the affected tags are published and are not being rewritten.

| tag | affected files |
|---|---|
| `imx95-v2.5.0` | `tests/camera-to-display/v4l2_to_fb.c` |
| `imx95-v2.6.0` | + `tests/isp-develop/neoisp_m2m.c` |
| `imx95-v2.6.1` and later | none |

**Why it survived so long:** a dropped digit still hashes, still distinguishes
inputs, and still varies frame to frame — so every *internal* check passed. Both
halves of a self-contained test agree with each other while agreeing with
nobody outside it. The defect is invisible to any test that only compares its
own outputs, which is most of them.

So: a hash recorded by those tags is internally consistent and externally
meaningless. If you are verifying a receipt from that era, recompute from the
frames with a stock FNV-1a rather than trusting the recorded value — and do
the C-against-host-Python cross-check on the same bytes at runtime, which is
the check that catches this class. Comparing constants by eye does not; two
halves can agree on the same wrong value.

Found by the 91emulator session while porting this test, by using the
canonical constant and noticing ours disagreed.
