# tests/cm7-hello

A minimal Cortex-M7 firmware for the i.MX 95 M7 real-time core, used to
prove the v1.x bring-up path end-to-end. The firmware writes a known
machine-readable fingerprint into a known DDR address and halts in WFI;
a later v1.x patch wires the M7 into the machine model and adds a
functional test that boots Linux + reads the fingerprint from the A55
side to confirm the M7 actually executed.

## What it does

1. ARMv7-M vector table at `0x00000000` (M7-view ITCM): initial SP =
   top of DTCM, reset vector → `Reset_Handler`.
2. `Reset_Handler` (in `startup.S`) calls `main()` and then spins in
   WFI.
3. `main()` (in `main.c`) writes:
   - the 32-bit magic word `0xC0FFEE07` at M7-view address
     `0x20000000` (start of the M7's own DTCM), and
   - the ASCII string `"CM7 RUNNING\n"` immediately after.

   An A55-side observer reads the same RAM via the architectural
   system-view alias of M7 DTCM at `0x20400000` (per the upstream Linux
   `imx_rproc_att_imx95_m7` attribute table). The fingerprint lives in
   the M7's private memory and is exposed to the A55 through the
   designed-in cross-view alias - no shared DDR is touched, so the
   Step-2 fixture cannot accidentally collide with Linux's initramfs,
   kernel image, or other DRAM allocations.

## Memory map

Matches the upstream Linux `imx_rproc_att_imx95_m7` attribute table:

| Region | M7 view | A55/system view | Size | Use |
| --- | --- | --- | --- | --- |
| ITCM | `0x00000000` | `0x203C0000` | 256 KiB | vector table + code |
| DTCM | `0x20000000` | `0x20400000` | 256 KiB | stack + data + bss |
| DDR  | `0x80000000` | `0x80000000` | 1.25 GiB | shared with A55 |

## Build

Needs `arm-none-eabi-gcc`. The included Makefile defaults to
`$HOME/tools/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi/bin/`;
override `TOOLCHAIN` if the toolchain is installed elsewhere:

    make
    # or, with a different toolchain path:
    make TOOLCHAIN=/opt/.../bin/arm-none-eabi-

Produces `cm7_image.elf` (the artifact QEMU loads onto the M7) and
`cm7_image.bin` (raw binary, for reference). `make info` prints the
ELF header (machine = ARM, expected `Cortex-M7` entry at the start of
ITCM) and the section layout.

## Status

Standalone-buildable as of v1.x scaffold; not yet integrated with the
machine model. Integration (Cortex-M7 instance in `hw/arm/fsl-imx95.c`,
TCM/DDR regions, NVIC, `cpu-num=7` for `-device loader`) lands in a
follow-up v1.x patch.
