# i.MX 95 ADC test

Exercises `hw/misc/imx93_adc.c` (`nxp,imx93-adc`, the `imx93_adc` iio driver).
`adc@44530000` was a UNIMP stub (MSR == 0), so the driver looped on *"ADC do
not in power down mode"*. The real model runs the small power / calibration /
conversion state machine the driver drives.

`run.sh` boots stock Linux and checks `iio:device0` is `imx93-adc` and the
channels convert (each `in_voltage<n>_raw` reads a synthetic 12-bit value;
distinct per channel) with the EOC interrupt (the driver uses interrupt index
2). PASS = device present + channels convert + no power-down loop.

```
bash tests/adc/run.sh
```

There is no analog input to sample in emulation, so readings are synthetic.
