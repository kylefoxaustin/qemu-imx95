# Full-stack consolidation test

One boot, every i.MX 95 subsystem that landed after the v2.0.0 NETC base up at
once, proving they coexist on a single machine instance:

- **NETC** — all three ENETC PFs (two 1G + the 10G port), link + RX datapath
- **Audio** — the three ASoC cards (bt-sco, wm8962, micfil) register
- **JPEG** — both HW codecs register (`/dev/video2` dec, `/dev/video3` enc)
- **USB** — `usb-kbd` enumerates as a HID input on the ChipIdea host
- **DPU** — the display-controller driver binds (drm); the render path is
  covered by the display work, here we assert coexistence
- **SM** — the real System Manager (M33) boots and serves SCMI

A headless VNC backend gives the DPU a surface without a GUI window.

PASS = every subsystem marker present **and** zero anomalies (panic / oops /
external abort / BUG / `hw csum failure`).

## Run

```
bash tests/full-stack/run.sh
```

Artifacts come from the same layout as `tests/netc/run-10g.sh`; override via
`QEMU`, `KBUILD`, `SM_ELF`. Audio and JPEG are loadable modules, so `MODROOT`
must point at a module tree whose vermagic matches the booted `Image`; it
defaults to `$KBUILD` (the in-tree `.ko` files of that kernel build).

## Notes

- The PF→`ethN` mapping is enumeration-order-dependent, so the NETC check
  asserts on the link events (one 10G + two 1G) rather than a fixed index.
- This boot carries the full device set, so audio/jpeg/the 10G link settle
  later than in the single-subsystem tests; `init` polls each subsystem ready
  before reporting rather than racing a fixed sleep.
