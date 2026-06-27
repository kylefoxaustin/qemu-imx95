# Fidelity detectors

The "hunt" side of the board-farm fidelity standard: detectors that boot the
model and **classify a peripheral block** as COMPUTES / FAULTS / **SILENT-WRONG**
(signals done but returns garbage). Silent-wrong answers are the worst bug class
for a virtual board farm — a developer's correctly-built binary gets a "success"
and garbage data with no error. These detectors surface them.

```sh
tests/code-sweep/fidelity/run.sh        # classify each block in-guest
```

Each `*_detect.c` in this dir is cross-built static, run in-guest, and prints a
`VERDICT:`. A detector **passes by classifying correctly** — a SILENT-WRONG
verdict means the detector is working, not that the model is healthy.

The authoritative per-block record is
[`docs/validation/fidelity-audit.md`](../../../docs/validation/fidelity-audit.md).

## Detectors

| detector | block | current verdict |
|----------|-------|-----------------|
| `adc_detect` | i.MX ADC (iio) | **SILENT-WRONG** — returns `0x100+ch*0x111` per channel, invariant (no real conversion) |

## Known silent-wrong, detector TODO

- **Neutron NPU** (`hw/misc/imx95_neutron.c`) — signals inference DONE without
  computing the output buffer. A detector needs to submit a job via the neutron
  UABI with a sentinel output buffer and check it's untouched; that needs the
  vendor submit path, so it's the next fidelity-hunt target.
