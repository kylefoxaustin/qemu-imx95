# sm-banner — System Manager (M33) bring-up smoke test (v0.6)

Loads the real NXP System Manager firmware onto the emulated Cortex-M33
and runs it.

## Prerequisites

Build the SM firmware from the NXP imx-sm tree (arm-none-eabi toolchain):

```
cd <your imx-sm checkout>      # github.com/nxp-imx/imx-sm
make config=mx95evk all
# -> build/mx95evk/m33_image.elf  (run with SM_ELF=<that path>)
```

## Run

```
./tests/sm-banner/run.sh
```

Override paths with `QEMU=` / `SM_ELF=` env vars.

## Expected result (v0.6)

The M33 boots from its ITCM vector table, runs SM early init, and faults
on the first unmodelled peripheral. QEMU reports:

```
qemu: fatal: Lockup: can't escalate 3 to HardFault (current priority -1)
... R03=44440000 ... R15=1ffc34b6
```

`R15=0x1ffc34b6` is inside `BBNSM_GprSetValue` (drivers/bbnsm/fsl_bbnsm.c);
`R03=0x44440000` is the BBNSM peripheral base. That fault is the intended
v0.6 endpoint: it proves the M33 is wired correctly and executes Thumb SM
code. Modelling the SM-side peripherals (starting with BBNSM) so the SM
gets further is v0.7 work.

## Notes

- The M33 is CPU index 6, after the 6-core A55 cluster. The SoC releases
  it from reset only when SM firmware is present in its ITCM, so a normal
  A55 Linux boot (no `-device loader` for the M33) leaves it halted and is
  unaffected.
- This script loads no A55 kernel; it exercises the SM core in isolation.
