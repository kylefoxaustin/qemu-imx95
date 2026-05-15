# qemu-imx95 — v0.0.2

A QEMU machine type for the NXP **i.MX 95 Application Processor, specifically the 19x19 EVK** (LPDDR5).

Goal: a non-cycle-accurate software-development emulator for the i.MX 95
so software developers don't have to wait for silicon. Long-term aim is
to be upstream-mergeable into QEMU mainline.

## What's in v0.0.2

- 6× Cortex-A55 cluster (default 6, configurable down)
- GIC-600 (GICv3-compatible) with full timer PPI + IRQ/FIQ/VIRQ/VFIQ
  wiring
- DDR window at `0x8000_0000`, default 8 GiB (matches the 19x19 EVK)
- 96 KiB OCRAM (sram1) at `0x204C_0000`
- **Real LPUART model** (`imx.lpuart`, `hw/char/imx_lpuart.c`):
  BAUD/STAT/CTRL/DATA/MATCH/MODIR/FIFO/WATER + the four global regs
  VERID/PARAM/GLOBAL/PINCFG, with software reset via `GLOBAL_RST`.
  Handles the Linux fsl_lpuart probe sequence and U-Boot's polling
  earlycon path.
- **All 8 LPUARTs instantiated** at their DTS-sourced bases; LPUART1
  is wired to `serial_hd(0)` as the 19x19 EVK console
  (stdout-path = &lpuart1 in the BSP DTS).
- Logging stubs for: BLK_CTRL_NETCMIX, the SM mailbox (mu2 SCMI
  channel) + shared-memory buffer, ELE_MU, WDOG3.
- Bare-metal "Hello from i.MX 95!" smoke test in
  `tests/hello-imx95/` — pokes characters at LPUART1 via polling,
  proves the LPUART model end-to-end.

All memmap addresses and IRQ numbers are extracted from the NXP BSP
device tree
(`arch/arm64/boot/dts/freescale/imx95.dtsi`), not guessed from prior
knowledge.

## What's *not* in v0.0.2

- U-Boot bring-up (v0.1)
- CCM / ANATOP / IOMUXC / SRC / GPC / AONMIX/WAKEUPMIX BLK_CTRL / TRDC
  — Linux reaches these only through SCMI → M33 SM firmware, so they
  have no `reg` in the Linux DTS. They return in v0.1 with RM-sourced
  bases when U-Boot SPL needs them.
- System Manager SCMI server (v0.3 — stubbed in C, real M33 firmware
  much later)
- WDOG1 / WDOG2 (M33-domain, not modeled)
- DDR controller registers (DDR is RAM; we sideload via `-kernel`)
- Accelerator complex — NPU, GPU, VPU, ISP (much later)

## Layout

This repo is a QEMU mainline fork with the i.MX 95 scaffolding
committed in. The interesting files:

| File | Purpose |
| --- | --- |
| `include/hw/arm/fsl-imx95.h` | SoC aggregate state, memory map enum, IRQ IDs |
| `hw/arm/fsl-imx95.c`         | SoC realization (CPU, GIC, LPUART wiring, unimplemented stubs) |
| `hw/arm/imx95-evk.c`         | 19x19 EVK board file |
| `include/hw/char/imx_lpuart.h` | LPUART register layout + state |
| `hw/char/imx_lpuart.c`       | LPUART device model |
| `tests/hello-imx95/`         | Bare-metal end-to-end smoke test |

## Building

Standard QEMU build. Requires `meson`, `ninja`, and the usual QEMU
dependencies (libglib2.0-dev, libpixman-1-dev, etc.).

    mkdir -p build && cd build
    ../configure --target-list=aarch64-softmmu
    ninja qemu-system-aarch64

## Smoke tests

Machine type registers:

    ./qemu-system-aarch64 -M help | grep imx95
    # Expected:
    #   imx95-19x19-evk      NXP i.MX 95 19x19 EVK (LPDDR5)

Memory map is sane:

    printf 'info mtree\nquit\n' | \
      ./qemu-system-aarch64 -M imx95-19x19-evk -display none \
        -monitor stdio -m 2G -S -serial null | \
      grep imx.lpuart

You should see eight `imx.lpuart` regions at the LPUART1–8 addresses
(`0x44380000`, `0x44390000`, `0x42570000`–`0x425A0000`,
`0x42690000`, `0x426A0000`).

End-to-end (the v0.0.2 milestone test):

    cd ../tests/hello-imx95 && make
    cd ../../build
    ./qemu-system-aarch64 -M imx95-19x19-evk -nographic -m 2G \
        -kernel ../tests/hello-imx95/hello.bin
    # Expected:
    #   Hello from i.MX 95!

The CPU idles in WFI after the message prints. Quit with `Ctrl-A x`.

Requires `binutils-aarch64-linux-gnu` for the hello binary build —
no compiler needed, just `as` and `ld`. See
`tests/hello-imx95/README.md` for details.

## Roadmap

- **v0.0.1** — scaffold: 6× A55 + GIC-600 + DDR/OCRAM, all
  peripherals as unimplemented stubs ✅
- **v0.0.2** — real memmap from DTS, LPUART device model, LPUART1
  console, bare-metal hello binary ✅
- **v0.1** — U-Boot SPL boots, prints banner, hands off to U-Boot
  proper (will need CCM/ANATOP/IOMUXC/SRC/GPC modelled, sourced from
  the i.MX 95 RM)
- **v0.2** — U-Boot command line, uSDHC, EQOS
- **v0.3** — Linux boots to login (SCMI server stubbed in C)
- **v0.4+** — Accelerator stubs, ELE stub

TTA.
