# qemu-imx95 — v0.1

A QEMU machine type for the NXP **i.MX 95 Application Processor, specifically the 19x19 EVK** (LPDDR5).

Goal: a non-cycle-accurate software-development emulator for the i.MX 95
so software developers don't have to wait for silicon. Long-term aim is
to be upstream-mergeable into QEMU mainline.

## What's in v0.1

End-to-end, stock NXP U-Boot SPL boots on this emulator and prints its
banner over LPUART1:

```
U-Boot SPL 2025.04-... (May 15 2026 - 12:37:01 -0500)
get_reset_reason:-1 for SYS
Normal Boot
```

To make that work, v0.1 added everything U-Boot SPL's `board_init_f`
touches from entry through `preloader_console_init`:

- **NXP MU (V2) device model** (`hw/misc/imx_mu.c`) — register-accurate
  Messaging Unit with TR/RR data registers, GCR.GIRn doorbells, GSR.GIPn
  interrupt-pending bits, externally-registerable hooks for doorbell and
  TR-write delivery.
- **SCMI server stub** (`hw/misc/imx95_scmi_server.c`) — stands in for
  the Cortex-M33 System Manager firmware. Watches MU2's doorbell hook,
  decodes SMT-format SCMI messages from `sram0`, dispatches to base
  (0x10), clock (0x14), and pinctrl (0x19) protocol handlers. Returns
  SUCCESS for clock CONFIG_SET / RATE_SET / PARENT_SET so U-Boot's
  clock subsystem and CCF integration both work. Advertises 80 clocks
  with unique names (required for CCF's `clk_register` not to reject
  duplicates).
- **ELE responder stub** (`hw/misc/imx95_ele_server.c`) — stands in for
  the EdgeLock Secure Enclave firmware. Watches elemu1's TR-write hook,
  decodes the ELE binary protocol, responds to `ELE_GET_INFO_REQ` with a
  plausible 256-byte info structure (SoC rev 0xA1, OEM-open lifecycle,
  zero-filled UID) written to the agent-provided buffer via DMA.
- **ULP watchdog stub** (`hw/misc/imx95_wdog.c`) — minimal model
  satisfying U-Boot's `disable_wdog()` (CS / CNT / TOVAL / WIN with the
  unlock-word handshake).
- **OCRAM extended** to `0x20480000 / 384 KiB` to cover the SPL load
  area + BSS + stack (was 96 KiB at 0x204C0000 in v0.0.2).
- **System Manager MU + SCMI shmem wired** — mu2 at 0x445B0000, sram0
  at 0x445B1000 as real RAM, response GIP via `imx_mu_assert_gip()`,
  IRQ to GIC SPI 226.
- **elemu1 promoted** from logging stub to a real MU instance at
  0x47530000 with the ELE responder attached.
- **Logging stubs added** for direct-MMIO peripherals SPL pokes
  pre-banner: system counter, CCM / ANATOP / IOMUXC / SRC / TRDC_AON /
  three BLK_CTRL aggregates (S_AONMIX, NS_ANOMIX, WAKEUPMIX), SMMU,
  WDG4 / WDG5, GPIO1–5, ELE_MU (elemu0). All addresses from
  `references/uboot-imx/arch/arm/include/asm/arch-imx9/imx-regs.h`.

Carry-over from v0.0.2:

- 6× Cortex-A55 cluster, GIC-600 (GICv3), 8 GiB DDR at `0x8000_0000`,
  `imx.lpuart` model with all 8 LPUART instances mapped, LPUART1 wired
  to `serial_hd(0)` as the 19x19 EVK console.

All memmap addresses and IRQ numbers come from:
- Linux DTS at `references/linux-imx/arch/arm64/boot/dts/freescale/imx95.dtsi`
  for peripherals the kernel sees directly
- U-Boot's `references/uboot-imx/arch/arm/include/asm/arch-imx9/imx-regs.h`
  for SCMI-routed peripherals SPL pokes before SCMI is up

## What's *not* in v0.1

- Post-banner SPL flow: SPL still resets after the banner because of
  - SCMI vendor protocol `0x84` (scmi_misc) not stubbed — needed for
    `rom_boot_info` query during SPL's container loader
  - No uSDHC / eMMC / QSPI flash model — SPL has nowhere to load the
    next stage from
  Both land in v0.2.
- SPL → U-Boot proper handoff, command line, EQOS — v0.2.
- Linux to login — v0.3.
- Accelerators (NPU/GPU/VPU/ISP), full ELE, real M33 SM firmware in
  place of the C-side stub — v0.4+.

## Layout

This repo is a QEMU mainline fork with the i.MX 95 scaffolding
committed in. The interesting files:

| File | Purpose |
| --- | --- |
| `include/hw/arm/fsl-imx95.h`        | SoC aggregate state, memory map enum, IRQ IDs |
| `hw/arm/fsl-imx95.c`                | SoC realization: CPU, GIC, all device wiring, logging stubs |
| `hw/arm/imx95-evk.c`                | 19x19 EVK board file |
| `include/hw/char/imx_lpuart.h`      | LPUART register layout + state |
| `hw/char/imx_lpuart.c`              | LPUART device model |
| `include/hw/misc/imx_mu.h`          | NXP MU (V2) register layout + state |
| `hw/misc/imx_mu.c`                  | MU device model with doorbell + TR-write hooks |
| `include/hw/misc/imx95_scmi_server.h` | SCMI protocol IDs + server state |
| `hw/misc/imx95_scmi_server.c`       | SCMI server stub (base / clock / pinctrl) |
| `include/hw/misc/imx95_ele_server.h` | ELE protocol constants + server state |
| `hw/misc/imx95_ele_server.c`        | ELE responder stub (GET_INFO) |
| `hw/misc/imx95_wdog.c`              | ULP watchdog stub |
| `tests/hello-imx95/`                | v0.0.2 bare-metal smoke test |
| `tests/spl-banner/`                 | v0.1 end-to-end SPL banner test |

## Host packages

Verified on Ubuntu 22.04. Each line covers a specific dev flow:

    # QEMU build (this repo)
    sudo apt install -y meson ninja-build

    # Bare-metal hello-imx95 test (assembly + linker, no compiler)
    sudo apt install -y binutils-aarch64-linux-gnu

    # U-Boot SPL build for v0.1 spl-banner test
    sudo apt install -y gcc-aarch64-linux-gnu \
                        bison flex \
                        libssl-dev libgnutls28-dev \
                        efitools

Standard build chain (make, gcc, libc-dev, pkg-config, python3,
libglib2.0-dev, libpixman-1-dev) is assumed already present.

## Building

Standard QEMU build:

    mkdir -p build && cd build
    ../configure --target-list=aarch64-softmmu
    ninja qemu-system-aarch64

## Smoke tests

Machine type registers:

    ./qemu-system-aarch64 -M help | grep imx95

End-to-end v0.0.2 test (bare-metal hello):

    cd ../tests/hello-imx95 && make
    cd ../../build
    ./qemu-system-aarch64 -M imx95-19x19-evk -nographic -m 2G \
        -kernel ../tests/hello-imx95/hello.bin
    # Expected: "Hello from i.MX 95!"

End-to-end v0.1 test (U-Boot SPL banner):

    See tests/spl-banner/README.md for build steps. After building
    u-boot-spl.bin:

    ./qemu-system-aarch64 -M imx95-19x19-evk -nographic -m 2G \
        -device loader,file=../tests/spl-banner/uboot-build/spl/u-boot-spl.bin,addr=0x20480000,cpu-num=0,force-raw=on
    # Expected (after a few seconds of CCF clock probe):
    #   U-Boot SPL 2025.04-... (May 15 2026 - ...)
    #   get_reset_reason:-1 for SYS
    #   Normal Boot
    # SPL then errors out trying to load the next stage - that's v0.2.

## Roadmap

- **v0.0.1** — scaffold: 6× A55 + GIC-600 + DDR/OCRAM, all
  peripherals as unimplemented stubs ✅
- **v0.0.2** — real memmap from DTS, LPUART device model, LPUART1
  console, bare-metal hello binary ✅
- **v0.1** — U-Boot SPL prints banner over LPUART1: MU + SCMI server +
  ELE responder + watchdog model + 18 direct-MMIO logging stubs ✅
- **v0.2** — Full SPL → U-Boot proper handoff (SCMI vendor protocol
  scmi_misc, container loader, uSDHC/eMMC model), U-Boot proper
  command line, EQOS.
- **v0.3** — Linux boots to login. SCMI stub needs perf, sensor,
  system-power, and imx-vendor protocols (lmm, bbm, cpu, misc).
- **v0.4+** — Accelerator stubs, ELE stub expansion, real M33 SM
  firmware running on an emulated M33 core in place of the C-side
  SCMI / ELE stubs.

TTA.
