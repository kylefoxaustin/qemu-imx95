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
| `adc_detect` | i.MX ADC (iio) | **FIXED → operator-driven.** Undriven channels read a documented default pattern; inject a real value with `qom-set /machine/soc/adc adc-ch<N> <v>` and the guest reads it back (verified ch5=2748). No longer a hidden constant. |

## Neutron NPU — flagged at the operator level (QMP)

The NPU can't compute (proprietary firmware) and **can't fault** either — the
NXP driver polls for `DONE` forever, so a non-DONE retcode hangs the guest. So
the model keeps acking but exposes the truth to the operator/control-plane via
QMP (no in-guest detector needed):

```
qom-get /machine/soc/neutron compute-modelled            # -> false
qom-get /machine/soc/neutron inferences-acked-uncomputed # -> count of RUN cmds
```

`compute-modelled=false` means "do not trust NPU outputs"; the counter rises
each time the guest "runs" an inference that wasn't actually computed. See
`docs/validation/fidelity-audit.md`.
