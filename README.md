# qemu-imx95 — v0.0.1 scaffold

A QEMU machine type for the NXP **i.MX 95 19x19 EVK** (LPDDR5).

Goal: a non-cycle-accurate software-development emulator for the i.MX 95.
This is the v0.0.1 scaffold — the smallest possible thing that compiles
and registers as a real machine type. No peripherals are modeled yet;
they're all installed as `create_unimplemented_device()` stubs that log
accesses instead of faulting.

## What's in v0.0.1

- 6× Cortex-A55 cluster (default 6, configurable down)
- GIC-600 (GICv3-compatible) with full timer PPI + IRQ/FIQ/VIRQ/VFIQ wiring
- DDR window at `0x8000_0000`, default 8 GiB to match the 19x19 EVK
- 1 MiB OCRAM region (placeholder base)
- Logging stubs for: LPUART1/2/3, CCM, ANATOP, IOMUXC, SRC, GPC, the three
  BLK_CTRL aggregates (AONMIX, WAKEUPMIX, NETCMIX), the System Manager MU
  + shared memory, the ELE MU, TRDC, and watchdogs

Validated: compiles cleanly against QEMU mainline (commit `edcc429`,
2026-05-14) with `cortex-a55` as the default CPU type.

## What's *not* in v0.0.1

- LPUART model (v0.0.2 — first real peripheral)
- Boot ROM / boot flow (we sideload via `-kernel` for now)
- DDR controller registers (treated as no-ops — DDR is "always initialized")
- System Manager SCMI server (v0.3)
- Anything in the accelerator complex (NPU, GPU, VPU, ISP)

## Files

| File | Purpose |
| --- | --- |
| `include/hw/arm/fsl-imx95.h` | SoC aggregate state, memory map enum, IRQ IDs |
| `hw/arm/fsl-imx95.c`        | SoC realization: CPU, GIC, OCRAM, unimplemented stubs |
| `hw/arm/imx95-evk.c`        | 19x19 EVK board file |
| `0001-imx95-build-system.patch` | Kconfig + meson.build hooks |

## Building (on Skippy)

```bash
git clone https://gitlab.com/qemu-project/qemu.git
cd qemu
# Copy the three scaffold files into place:
cp /path/to/scaffold/include/hw/arm/fsl-imx95.h include/hw/arm/
cp /path/to/scaffold/hw/arm/fsl-imx95.c          hw/arm/
cp /path/to/scaffold/hw/arm/imx95-evk.c          hw/arm/
git apply /path/to/scaffold/0001-imx95-build-system.patch

mkdir build && cd build
../configure --target-list=aarch64-softmmu
ninja qemu-system-aarch64
```

Smoke test that the machine type registers:

```bash
./qemu-system-aarch64 -M help | grep imx95
# Expected:
#   imx95-19x19-evk      NXP i.MX 95 19x19 EVK (LPDDR5)
```

Smoke test that it instantiates (will exit quickly with no kernel; that's fine):

```bash
./qemu-system-aarch64 -M imx95-19x19-evk -nographic -d unimp -kernel /dev/null 2>&1 | head -20
```

You should see `-d unimp` log messages for any MMIO touched by the boot
stub. That's the proof the unimplemented-peripheral stubs are alive.

## Addresses marked TODO

Every peripheral base address in `fsl_imx95_memmap[]` is a **placeholder**.
The values don't overlap (so the machine instantiates), but they almost
certainly don't match the real RM. v0.0.2 priority work:

1. Pull the System Address Map from i.MX 95 RM
2. Replace placeholders with real bases for LPUART1–8, CCM, ANATOP,
   IOMUXC, SRC, GPC, BLK_CTRL_*, SM_MU, ELE_MU, TRDC
3. Cross-reference with the i.MX 95 device tree in the NXP BSP
   (`arch/arm64/boot/dts/freescale/imx95.dtsi`) for sanity

## Roadmap

- **v0.0.2** — LPUART device model + verified addresses → "hello world"
  binary prints over UART
- **v0.1** — U-Boot SPL boots, prints banner, hands off to U-Boot proper
- **v0.2** — U-Boot command line, uSDHC, EQOS
- **v0.3** — Linux boots to login (stubbed SCMI server in C)
- **v0.4+** — Accelerator stubs, ELE stub

TTA.
