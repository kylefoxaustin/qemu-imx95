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
| **Neutron NPU** | **SILENT-WRONG** | `hw/misc/imx95_neutron.c:119-131` — sets MBOX0=DONE, APPSTATUS INFDONE, raises SPI 318; the output buffer is **not** computed (the source header says so). `/dev/neutron0` binds, so a TFLite/LiteRT inference "completes" with garbage outputs and no error. |
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

## Silent-wrong blocks — dev-impact ranking

1. **Neutron NPU** — ML inference is a named farm scenario; silent garbage
   outputs are the highest-impact lie. (95's analog of MCXN947's PowerQuad.)
   *Remaining — next fidelity target.*
2. ~~**ADC**~~ — **FIXED** (operator-driven conversion values via `qom-set`;
   default is a documented test pattern). No longer a hidden constant.

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
