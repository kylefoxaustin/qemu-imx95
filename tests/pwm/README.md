# i.MX 95 TPM PWM test

Exercises `hw/misc/imx_tpm_pwm.c` (the `fsl,imx7ulp-pwm` TPM, `pwm-imx-tpm`
driver). The `pwm@424e0000/42510000` controllers were UNIMP stubs, so the
driver read `PARAM == 0` and failed with *"failed to add PWM chip"* (`-EINVAL`).

`run.sh` boots stock Linux and checks the controllers register (`npwm = 6`) and
the data path is functional: set a period + duty via the PWM sysfs and read
them back (the driver computes them from the SCMI clock + the MOD/CnV registers
the model stores).

```
bash tests/pwm/run.sh
```

PASS = two pwmchips with `npwm=6` + the period/duty round-trip. There is no
electrical output to drive in emulation, so this is the register/control bar.
