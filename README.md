# qemu-imx95

A QEMU machine type for the NXP **i.MX 95 Application Processor**, targeting
the **19x19 EVK** (LPDDR5) variant.

Goal: a non-cycle-accurate software-development emulator for the i.MX 95 so
software developers don't have to wait for silicon. Long-term aim is to be
upstream-mergeable into QEMU mainline. Structural and stylistic conventions
follow the existing upstream i.MX 8MP code
(`hw/arm/fsl-imx8mp.{c,h}`, `hw/arm/imx8mp-evk.c`).

## Status

**The emulator boots stock NXP Linux 6.12.49 to userspace.** All 6 Cortex-A55
cores come up via PSCI (kept active by `cpuidle.off=1`; see below), GICv3
initialises, the SCMI mailbox transport runs against the in-tree SCMI server
stub, all in-tree drivers reach their `.probe()` entry (some defer pending
providers — see Known limitation), and PID 1 runs in userspace (verified: an
init that mounts `/proc` and reports `aarch64`).

Earlier milestones — U-Boot SPL banner, SPL → U-Boot proper handoff over an
emulated SD boot chain, and the U-Boot interactive prompt — all still work.

v0.5 reached Linux userspace; current development is **v0.6** (interactive
console). See the roadmap and `TODO.md` for what's done and what's open.

## Booting Linux

Build a kernel `Image` + the `imx95-19x19-evk.dtb` from the NXP BSP
(`references/linux-imx`, imx defconfig), then:

```
./build/qemu-system-aarch64 -M imx95-19x19-evk -m 2G -display none \
    -serial mon:stdio \
    -kernel <Image> \
    -dtb <imx95-19x19-evk.dtb> \
    -append "earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1"
```

To boot all the way to userspace, add an initramfs and `rdinit`:

```
    -initrd <initramfs.cpio.gz> \
    -append "... cpuidle.off=1 rdinit=/init"
```

Two **required quirks** (both documented with root cause in `TODO.md`):

- **earlycon address `0x44380010`, not `0x44380000`.** The i.MX95 LPUART has
  VERID/PARAM/GLOBAL/PINCFG at offsets 0x00–0x0C and BAUD at 0x10. Linux's
  regular driver applies the `reg_off = 0x10` offset to the DT base
  automatically; earlycon does not, so the cmdline address must be
  pre-offset.
- **`cpuidle.off=1` is required.** Without it Linux hangs right after
  `SCMI Notifications - Core Enabled`. On entering its deepest idle state a
  CPU disables its GIC CPU interface (`ICC_IGRPEN1_EL1 = 0`) and quiesces its
  redistributor; the SCMI completion IRQ (mu2, SPI 226) is routed to that
  CPU, and the emulator has no power-controller wake-request to bring it
  back, so the response interrupt is never delivered. Disabling cpuidle keeps
  the CPU interfaces up. The proper fix (model i.MX95 CPU power management /
  the M33 SM firmware) is future roadmap.

Known limitation: there is no interactive shell yet — the full `fsl_lpuart`
serial driver defers probe (no `ttyLP0` tty), along with a cascade of
gpio/i2c/mmc/spi/pcie drivers. Userspace runs; an interactive console is the
next bring-up target.

## What each milestone added

- **v0.0.1** — scaffold: 6× A55 + GIC-600 (GICv3) + DDR/OCRAM, all
  peripherals as unimplemented stubs.
- **v0.0.2** — real memory map from the DTS, `imx.lpuart` device model,
  LPUART1 console, bare-metal hello binary.
- **v0.1** — U-Boot SPL prints its banner over LPUART1. Added the NXP MU (V2)
  model, the SCMI server stub (base / clock / pinctrl), the ELE responder
  stub (GET_INFO), a ULP watchdog stub, and ~18 direct-MMIO logging stubs.
- **v0.2** — SPL → U-Boot proper handoff over a full emulated SD boot chain.
  Added `TYPE_IMX_USDHC` (uSDHC1/2/3), the SCMI imx-misc vendor protocol
  (0x84, ROM passover), an on-demand clock model with a per-clock rate table,
  the i.MX uSDHC SDMA-boundary fix in `hw/sd/sdhci`, and AHAB-container SD
  boot so SPL loads and jumps to U-Boot proper, which prints its banner.
- **v0.3** — U-Boot interactive prompt reachable. Added LPI2C stubs,
  `psci-conduit=smc`, the SCMI POWER_DOMAIN protocol (0x11), USB PHY/DWC3
  stubs, and the LPUART WATER RX-count fix that lets `tstc()` see keystrokes.
- **v0.4** — Linux 6.12.49 boots via `-kernel` direct load: all 6 A55s up via
  PSCI (per-CPU MPIDR Aff1 fix), GICv3 up, SCMI mailbox transport active.
  Added the Linux-DTS MU stub set and the LPUART TX-not-gated-on-TE fix.
- **v0.5** (in progress) — Linux to userspace. sysctr region RAM-backed so
  the NXP sysctr driver's write-readback loop converges; SCMI interrupt-mode
  bring-up unblocked via `cpuidle.off=1` (root cause diagnosed); reaches PID 1
  in userspace with an initramfs.

All memory-map addresses and IRQ numbers come from the NXP BSP, never
guessed:

- Linux DTS `references/linux-imx/arch/arm64/boot/dts/freescale/imx95.dtsi`
  for peripherals the kernel sees directly.
- U-Boot `references/uboot-imx/arch/arm/include/asm/arch-imx9/imx-regs.h`
  for SCMI-routed peripherals SPL pokes before SCMI is up.

The i.MX 95 RM is authoritative for register behaviour; the DTS is
authoritative for addresses and IRQ numbers.

## Layout

This repo is a QEMU mainline fork with the i.MX 95 scaffolding committed in.
The interesting files:

| File | Purpose |
| --- | --- |
| `include/hw/arm/fsl-imx95.h`          | SoC aggregate state, memory map enum, IRQ IDs |
| `hw/arm/fsl-imx95.c`                  | SoC realization: CPU, GIC, all device wiring, logging stubs |
| `hw/arm/imx95-evk.c`                  | 19x19 EVK board file |
| `include/hw/char/imx_lpuart.h`        | LPUART register layout + state |
| `hw/char/imx_lpuart.c`                | LPUART device model |
| `include/hw/misc/imx_mu.h`            | NXP MU (V2) register layout + state |
| `hw/misc/imx_mu.c`                    | MU device model with doorbell + TR-write hooks |
| `include/hw/misc/imx95_scmi_server.h` | SCMI protocol IDs + server state |
| `hw/misc/imx95_scmi_server.c`         | SCMI server stub (base / clock / pinctrl / power-domain / imx-misc) |
| `include/hw/misc/imx95_ele_server.h`  | ELE protocol constants + server state |
| `hw/misc/imx95_ele_server.c`          | ELE responder stub (GET_INFO) |
| `hw/misc/imx95_wdog.c`                | ULP watchdog stub |
| `tests/hello-imx95/`                  | v0.0.2 bare-metal smoke test |
| `tests/spl-banner/`                   | U-Boot SPL banner test + SD-boot docs |

## Host packages

Verified on Ubuntu 22.04. Each line covers a specific dev flow:

    # QEMU build (this repo)
    sudo apt install -y meson ninja-build

    # Bare-metal hello-imx95 test + static initramfs init (assembler + cross gcc)
    sudo apt install -y binutils-aarch64-linux-gnu gcc-aarch64-linux-gnu

    # U-Boot SPL build for the spl-banner test
    sudo apt install -y bison flex libssl-dev libgnutls28-dev efitools

Standard build chain (make, gcc, libc-dev, pkg-config, python3,
libglib2.0-dev, libpixman-1-dev) is assumed already present.

## Building

    mkdir -p build && cd build
    ../configure --target-list=aarch64-softmmu
    ninja qemu-system-aarch64

## Smoke tests

Machine type registers:

    ./build/qemu-system-aarch64 -M help | grep imx95

Bare-metal hello (v0.0.2):

    cd tests/hello-imx95 && make && cd ../..
    ./build/qemu-system-aarch64 -M imx95-19x19-evk -nographic -m 2G \
        -kernel tests/hello-imx95/hello.bin
    # Expected: "Hello from i.MX 95!"

U-Boot SPL banner (v0.1) — see `tests/spl-banner/README.md` for build steps.

Linux boot — see "Booting Linux" above.

## Roadmap

- **v0.0.1** — scaffold ✅
- **v0.0.2** — real memmap, LPUART model, bare-metal hello ✅
- **v0.1** — U-Boot SPL banner ✅
- **v0.2** — SPL → U-Boot proper handoff, uSDHC, SD boot chain ✅
- **v0.3** — U-Boot interactive prompt ✅
- **v0.4** — Linux boots via `-kernel`: 6 CPUs, GICv3, SCMI mailbox ✅
- **v0.5** — Linux to userspace: SCMI interrupt bring-up unblocked, reaches
  PID 1 in userspace ✅
- **v0.6** — interactive Linux shell (in progress): bring up the LPUART tty
  and its probe-defer cascade (clock/dma/gpio deps) so a real console works.
- **v0.7+** — eventually U-Boot → kernel-from-MMC for a fully realistic boot;
  accelerator stubs (NPU/GPU/VPU/ISP); real M33 SM firmware (with CPU power
  management) in place of the C-side SCMI/ELE stubs.
