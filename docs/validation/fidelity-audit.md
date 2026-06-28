# i.MX95 QEMU — peripheral fidelity audit

The board-farm standard (fleet directive): a developer brings their own
correctly-built binary; **code that runs on real silicon should run on the model
and get the right answer, or fail honestly.** The worst bug class is a *silent
wrong answer* — a block that signals completion/success but didn't actually
compute (accelerators that don't compute; analog blocks returning constants).
Either compute correctly, or fault/flag honestly.

This file is the honest per-block record: what the farm can trust, what fails
honestly (acceptable), and what **silently lies** (do not trust for real data).
Detectors that reproduce these verdicts live in `tests/code-sweep/fidelity/`.

## Classes

- **COMPUTES** — submits work, returns the correct result. Trust it.
- **FAULTS / ABSENT (honest)** — errors, rejects probe, or isn't registered.
  The guest's code sees a failure and can handle it. Acceptable.
- **SILENT-WRONG** — signals done/success but returns garbage/constants with no
  signal to the guest. **Do not rely on it for real data.**

## Verdicts

| Block | Class | Evidence |
|-------|-------|----------|
| **Neutron NPU** | **FLAGGED (default) → can FAULT-honestly to the guest (operator opt-in)** | Compute is NXP-proprietary firmware → un-modellable. The model keeps acking `DONE` (completion gates only on MBOX0, so it must). Two honesty layers: (1) *operator*: per-inference `LOG_GUEST_ERROR` + QMP `qom-get /machine/soc/neutron compute-modelled` (= `false`) + `inferences-acked-uncomputed`. (2) *guest*: the driver reads an `error_code` from **MBOX1** *after* `DONE` and surfaces it to userspace via `NEUTRON_IOCTL_INFERENCE_STATE` — and that read does **not** gate completion. So `qom-set /machine/soc/neutron neutron-uncomputed-errcode <nonzero>` makes the model return that recognisable error_code on every uncomputed inference: a guest-visible "did not compute" fault, **without hanging** the driver. Default 0 = faithful to silicon (happy-path success). Output is still uncomputed — do not trust NPU results — but the guest can now be told. `hw/misc/imx95_neutron.c`. |
| **ADC** | **FIXED → COMPUTES (operator-driven)** | Was SILENT-WRONG (hidden constant `0x100+ch*0x111`). Now each channel is a read/write QOM property `adc-ch0..7` settable at runtime via `qom-set /machine/soc/adc adc-ch<N> <value>` — whatever the operator injects is what the guest reads (the model's analog of the board's pin voltage). Default = a documented distinct-per-channel test pattern, so an un-driven channel is deterministic, not a hidden lie. Verified: injected `adc-ch5=2748` → guest `in_voltage5_raw=2748` while undriven channels read their defaults. `hw/misc/imx93_adc.c`. |
| **DPU 2D blit** | COMPUTES | `hw/dpu/imx95_dpu.c:1093-1266` — copy/fill/blend(Porter-Duff)/scale/rotate/colour-convert all read source + write computed result to guest memory. (Display *scanout* to console is a stub, but the 2D engine is real.) |
| **eDMA v3/v5** | COMPUTES | `hw/dma/imx95_edma.c:195-210` — actually `address_space_read`→`address_space_write`s the data; DONE/IRQ after the move. |
| **JPEG codec** | COMPUTES / honest | `hw/*/imx95_jpeg.c:479-499` — decodes/encodes with libjpeg when `CONFIG_LIBJPEG`; otherwise sets `SLOT_STATUS_ENC_CONFIG_ERR` (honest error), not garbage. |
| **ELE / Sentinel** | honest | `hw/misc/imx95_ele_server.c` — specific handlers (GET_INFO/STATE/FW_VERSION) return plausible data; unhandled cmds return an explicit generic SUCCESS (logged), not garbage. `GET_RANDOM` is honest pseudo-random (not CAAM/TRNG-quality). |
| **Mali GPU** | FAULTS (honest) | rejects probe with EINVAL — GL code fails fast, not silently. |
| **Amphion VPU** | ABSENT (honest) | logging-stub MMIO, not probed — no false "decoded" frames. |

Datapaths exercised by the code-sweep peripheral tier (storage/eMMC, ENETC,
USB-BOT, GPIO, I²C, RTC) are all **COMPUTES** — verified by integrity oracles
(write→drop-caches→read-back, register round-trips), not just "done" flags.

## Silent-wrong blocks — status

1. **Neutron NPU** — **FLAGGED → can FAULT-honestly to the guest (opt-in)**
   (was silent). Can't compute (proprietary fw), so the model keeps acking
   `DONE`. Honesty: a loud per-inference log + QMP `compute-modelled=false` /
   `inferences-acked-uncomputed` (operator level), **and** an operator opt-in
   `neutron-uncomputed-errcode` property that returns a non-zero error_code in
   MBOX1 — surfaced to the guest app by the driver (see the corrected fleet
   finding below) — so a guest that checks inference state learns it didn't
   compute. Default 0 keeps the happy path faithful. Real fix (run the model)
   needs the proprietary firmware/ISA.
2. ~~**ADC**~~ — **FIXED** (operator-driven conversion values via `qom-set`;
   default is a documented test pattern). No longer a hidden constant.

### Finding for the fleet: Neutron's error channel is MBOX1, not the completion path (corrected)

An earlier version of this audit claimed the NXP Neutron driver "has no error
path." That was **too strong** — corrected here after reading
`drivers/staging/neutron`:

- **Completion** is gated *only* on MBOX0 == `DONE` (0xAD0):
  `poll_result_callback` re-arms its poll timer forever and only completes on
  `DONE`, and the IRQ path checks only `retcode == DONE`. A non-DONE *retcode*
  in MBOX0 **does** hang the guest — so the model must keep acking DONE. (This
  is the part the old finding got right.)
- **But there is a separate guest-visible error channel:** after completion the
  driver reads an `error_code` from **MBOX1** and copies it to userspace in
  `struct neutron_uapi_result_status.error_code` via the
  `NEUTRON_IOCTL_INFERENCE_STATE` ioctl. This read does **not** gate completion,
  so a non-zero MBOX1 alongside MBOX0 == `DONE` reaches the app **without
  hanging** it.

So the precise rule for modelling these NXP remote-accelerators: you cannot fault
via the *completion* retcode (it hangs), **but you can fault via a separate,
non-gating error/status field** if the driver plumbs one to userspace. The
i.MX 95 model uses exactly this (`neutron-uncomputed-errcode`).

**Confirmed two-way taxonomy** (i.MX 93 checked its Ethos-U after this finding —
2026-06-28): NXP remote-accelerator drivers split by *what gates completion*, and
that decides where the honest fault goes:

- **Neutron-style — completion gates on a polled retcode** (MBOX0 == DONE).
  A non-DONE retcode hangs, so you MUST fault via a *side-band* field
  (Neutron: MBOX1 `error_code`, read after DONE, surfaced via the ioctl).
- **Ethos-style — completion gates on response *arrival*** (the `INFERENCE_RSP`
  rpmsg). `ethosu_inference_rsp()` maps `rsp->status` for every value then
  *unconditionally* sets done+wake, so the response's own `status` is **in-band
  but non-gating** — a `STATUS_ERROR` completes exactly like success and
  userspace reads it via the `INFERENCE_STATUS` ioctl. Here you fault via that
  in-band status field directly; no side-band needed.

Either way the honest fault is a *non-gating* status channel, never the
completion signal. (The i.MX 93 model is adding opt-in honest-fault on the same
default-faithful pattern, for unsupported opcodes/elementwise sub-ops it had been
silently no-opping.)

## Fixing vs flagging

Per the standard, each silent-wrong block should either be made to **compute**
(NPU: run the model; ADC: return a real/parameterisable conversion) or be made
to **fault honestly** so the guest knows. Until then this record + the detectors
are the honest disclosure for the farm.

## Running the detectors

```sh
tests/code-sweep/fidelity/run.sh        # boots the model, classifies each block
```
A detector "passes" by correctly *classifying* its block — a SILENT-WRONG verdict
is the detector working, not the model passing.
