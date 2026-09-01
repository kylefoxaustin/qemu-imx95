# NeoISP: from registration tier to a working debayer

## Where it stands

`isp@4ae00000` (`nxp,imx95-b0-neoisp`) currently has **no model**. Both MMIO
windows — `registers` @0x4ae00000 and `stats` @0x4afe0000 — are plain backing
store, which is why the driver binds completely (regmap, soft-reset handshake,
power domain, cameramix clocks, IRQ 222, eight node-groups, 48 `/dev/video`
nodes) without a single line of device model. It registers; it does not process.
A frame handed to it comes back untouched.

## What the driver actually needs, from reading it

`neoisp` is a **V4L2 mem2mem** device: buffers in DRAM, buffers out, no
streaming from the sensor. `neoisp_queue_job()` programs the context and then
does exactly one thing to start work:

| register | offset | meaning |
|---|---|---|
| `NEO_PIPE_CONF_TRIG_CAM0` | `0x20` | write BIT(0) → **go** |
| `NEO_PIPE_CONF_IMG0_IN_ADDR_CAM0` | `0x3c` | input buffer, `addr >> 4` |
| `NEO_PIPE_CONF_IMG1_IN_ADDR_CAM0` | `0x40` | second input (HDR pair) |
| `NEO_PIPE_CONF_OUTCH0_ADDR_CAM0` | `0x44` | output buffer, `addr >> 4` |
| `NEO_PIPE_CONF_OUTCH1_ADDR_CAM0` | `0x48` | second output channel |
| `NEO_PIPE_CONF_IMG_CONF_CAM0` | `0x30` | input bits-per-pixel, alignment |
| `NEO_DEMOSAIC_CTRL_CAM0` | — | `FMT` bits 5:4 = the Bayer phase |
| `NEO_PIPE_CONF_INT_EN0` | `0x24` (v2) | interrupt enables |
| `NEO_PIPE_CONF_INT_STAT0` | — | status; **`FD2` = BIT(3)** is the one that sets `done` |

⚠️ **Addresses are stored shifted right by 4** (`NEO_PIPE_CONF_ADDR_SET`), so a
model must shift back before touching guest memory. Getting this wrong reads a
plausible-looking address 16× too low, which will fault rather than corrupt —
the good failure mode, but worth knowing.

## The minimum viable model

1. Trap the write of BIT(0) to `0x20`.
2. Read `IMG0_IN_ADDR`, `OUTCH0_ADDR` (shift left 4), geometry, and the
   `DEMOSAIC_CTRL` Bayer phase from the register file.
3. DMA the input out of guest memory, debayer it, write the result back.
4. Set `INT_STAT0.FD2` and raise IRQ 222.

That alone turns 48 dead video nodes into a working mem2mem ISP. Everything
else the block does — black level, white balance, gamma, colour correction,
denoise, tone mapping — is a refinement on top of a path that already carries a
frame end to end.

## Fidelity: what "correct" means here, and what it cannot mean

The block's per-pixel arithmetic is proprietary, so a QEMU model cannot be
bit-exact against silicon and must not claim to be. What it *can* be is
**structurally correct and self-consistent**: a known Bayer input produces a
recognisable developed image, deterministically, with a golden hash so
regressions are caught.

State the tier plainly, the way the DPU blit and Neutron rows already do:
*the transport and control path are modelled; the per-pixel arithmetic is an
approximation of the real block, not a reproduction of it.* A reviewer who is
told that can use the model correctly. One who is told "ISP works" cannot.

## Test shape, reusing what exists

`tests/camera-to-display/` already proves the transport with the ISP *out* of
the path. The ISP test is that harness with one link inserted: feed **Bayer**
instead of developed YUYV, run it through the mem2mem device, and display the
result. Two proofs again, and the second matters more here than it did for
transport:

- **Deterministic**: golden hash of the developed output, so the pipeline is
  gated against regression.
- **Recognisable**: correlate the developed image against the original RGB the
  Bayer was made from. Debayer is lossy, so demand `r > 0.95`, not equality —
  and set the threshold from a measured clean run rather than by taste.

⭐ The honest failure this design guards against: an ISP that produces *a*
picture rather than *the* picture. Correlating against the source image is what
separates "the debayer ran" from "the debayer ran correctly", and a hash alone
cannot tell those apart.
