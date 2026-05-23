# qemu-imx95

A QEMU machine type for the NXP **i.MX 95** SoC, targeting the **19x19 EVK**
(LPDDR5) variant.

qemu-imx95 boots stock NXP BSP Linux to userspace with the **real NXP System
Manager firmware** running on the emulated Cortex-M33 serving the Cortex-A55
cluster's SCMI traffic. Intended use cases are BSP development, System Manager
firmware development, peripheral-driver development, and CI for the above. It
is not cycle-accurate, and hardware accelerators (GPU, VPU, NPU) are stubbed at
probe time only — this emulator does not perform GPU rendering, video codec, or
NPU inference (see [Known limitations](#known-limitations)).

Structural and stylistic conventions follow the upstream i.MX 8MP code
(`hw/arm/fsl-imx8mp.{c,h}`, `hw/arm/imx8mp-evk.c`); the long-term aim is to be
upstream-mergeable into QEMU mainline.

**Maintainer:** Kyle Fox &lt;13992031+kylefoxaustin@users.noreply.github.com&gt;
(see [`MAINTAINERS`](MAINTAINERS) for the canonical entry).

## Scope: what's modelled and what's deferred

The i.MX 95 SoC has a heterogeneous CPU topology of **6× Cortex-A55** (the
application cluster), **1× Cortex-M33** (the System Manager core), and
**1× Cortex-M7** (the real-time domain). This machine currently models the
**A55 cluster and the M33** — the latter runs the real NXP SM firmware and is
the load-bearing piece for Linux's SCMI bring-up. The **M7 is deliberately
deferred**: it hosts an independent FreeRTOS/MCUXpresso real-time workload
and is not on the path from `-kernel` to Linux userspace, so omitting it
doesn't block any of the artifact's stated use cases (BSP development, SM
firmware development, peripheral-driver development, CI). It is a known,
intentional scope choice rather than an oversight, and adding it is on the
post-v1 roadmap (its own `ARMv7MState`, ITCM/DTCM regions, separate MU
channel, and `imx-rproc` integration on the Linux side).

## What runs today

Stock **NXP Linux 6.12.49** boots to userspace (PID 1) on the 6-core A55
cluster, on top of the real NXP **System Manager** firmware running on the
emulated Cortex-M33. The SM boots through its full init, runs its
logical-machine bring-up, and answers Linux's SCMI over a cross-connected MU2:

```
arm-scmi: SCMI Protocol v2.1 'NXP:IMX' Firmware version 0x333
scmi-perf-domain: Initialized 13 performance domains
arm-scmi: NXP SM BBM / CPU (9 cpus) / MISC / LMM (3 logical machines)
...
Run /init as init process
```

The **interactive Linux serial console works** (via a BusyBox initramfs):
typed commands reach the shell over ttyLP0 and output reaches the host
terminal. The SM also boots standalone to its debug-monitor prompt
(`tests/sm-banner/run.sh`). Earlier bring-up milestones — U-Boot SPL banner,
SPL → U-Boot proper handoff over an emulated SD boot chain, and the U-Boot
interactive prompt — still work.

**Validated** (full evidence in
[`docs/imx95/validation-report.md`](docs/imx95/validation-report.md)):

- Reproducible from a clean clone — including a clean `ubuntu:24.04`
  container as a different user with only the README-documented dependencies.
- Boots **three distinct kernels** — NXP 6.12.49 vendor, mainline 6.12.0,
  NXP 6.18.2 vendor — all reaching `/init`, all binding SCMI to the real SM.
- Boots **two distinct userspaces** — static BusyBox and a glibc-dynamic
  Yocto trim (real `bash` 5.2.37, dynamic coreutils, file-I/O integrity
  verified under sustained load).
- 24 h stability run: stable memory, data-integrity md5 unchanged, no panics
  or SCMI timeouts.
- Tier-3 limitations (no networking, no Linux block storage, GPU/VPU/NPU
  stub-only) characterized with precise failure points — see
  [`docs/imx95/known-limitations.md`](docs/imx95/known-limitations.md).

The campaign found and fixed two real bugs in the artifact before any
external user would have hit them: an LPUART FIFO read-only-bit storm
(commit `741af2f5e3`) and an undocumented `file` host-package dependency in a
test script (commit `7def38532a`).

## Required artifacts

> **The System Manager firmware image is required.** The real SM on the M33 is
> the only SCMI provider — there is no built-in software SCMI server. Booting
> without `-device loader,file=<m33_image.elf>,cpu-num=6` leaves the M33 halted
> and **nothing answers SCMI**, so Linux hangs at `arm-scmi` probe. This is not
> a bug; it matches real silicon, where the SM is always present.

To boot Linux to userspace you need four artifacts, all built from the NXP BSP:

| Artifact | Where from |
| --- | --- |
| Kernel `Image`              | `references/linux-imx`, imx defconfig |
| `imx95-19x19-evk.dtb`       | same kernel build |
| initramfs (`*.cpio.gz`)     | any aarch64 rootfs with `/init` |
| SM firmware `m33_image.elf` | `references/imx-sm`, `make config=mx95evk` |

## Quick start

Build (see [Building](#building)), then boot Linux to userspace on the real SM.

**Easiest path:** `tests/swap-boot/run.sh`, with the artifact paths set via the
`QEMU`/`SM_ELF`/`KERNEL`/`DTB`/`INITRD` env vars. The script encapsulates the
canonical invocation.

**Equivalent explicit command** (for adapting to other harnesses):

```
./build/qemu-system-aarch64 -M imx95-19x19-evk -m 2G -display none \
    -kernel <Image> -dtb <imx95-19x19-evk.dtb> -initrd <initramfs.cpio.gz> \
    -append "earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -device loader,file=<m33_image.elf>,cpu-num=6 \
    -serial mon:stdio -serial null
```

Two cmdline details are load-bearing:

- **earlycon address `0x44380010`, not `0x44380000`.** The i.MX 95 LPUART has
  VERID/PARAM/GLOBAL/PINCFG at 0x00–0x0C and BAUD at 0x10. Linux's regular
  driver applies the `reg_off = 0x10` offset to the DT base automatically;
  earlycon does not, so the cmdline address must be pre-offset.
- **`cpuidle.off=1` is required** — see [Known limitations](#known-limitations).

## Known limitations

**Deep cpuidle requires `cpuidle.off=1`.** Without it, Linux hangs shortly
after `SCMI Notifications - Core Enabled`. The mechanism is fully
characterized, and the gap is in QEMU core, **not** the i.MX 95 machine model:

1. Linux's `cpu-pd-wait` idle state has `local-timer-stop`, so an idling core
   shuts down its per-CPU arch timer and relies on a broadcast clockevent.
   **The machine model provides this** — `hw/timer/imx95_sysctr.c` is a real
   system-counter timer (live counter + compare-match IRQ on GIC SPI 72).
2. On entering that state, Linux's GIC `cpu_pm` notifier disables the per-CPU
   GIC interface (matching real silicon, where a deeper power state is entered).
3. **QEMU's GICv3 model does not implement the architectural WakeRequest** — it
   never wakes a halted CPU on a pending interrupt once the CPU interface is
   quiesced. So a cross-CPU wake (e.g. a `stop_machine` IPI) can't reach the
   idle core, and the boot hangs. This affects all GICv3 QEMU machines.
4. With `cpuidle.off=1`, Linux uses shallow WFI on the still-running per-CPU
   arch timer and boots cleanly. The proper fix is QEMU-core work, filed as a
   follow-up to qemu-devel.

**Hardware accelerators (GPU/VPU/NPU) are probe-time stubs only** — their Linux
drivers register but no rendering/codec/inference occurs.

Full diagnoses, including the icount × multi-CPU finding, are in
[`docs/imx95/known-limitations.md`](docs/imx95/known-limitations.md).

## Architecture overview

- **6× Cortex-A55** (GICv3 / GIC-600), the application cores running Linux.
- **1× Cortex-M33** running the real NXP System Manager firmware. Released from
  reset only when SM firmware is present in its ITCM.
- **MU2 cross-connect — the SCMI transport.** The A55-side mailbox
  (MUA @0x445b0000) and the M33-side mailbox (MUB @0x445c0000) are peer-linked
  over a shared SMT SRAM. Linux's SCMI doorbells on MUA reach the SM via MUB;
  the SM's responses on MUB raise the A55's MU IRQ via MUA.
- Real device models for the pieces the boot exercises: LPUART, uSDHC, MU, GIC,
  the system-counter timer, ANATOP PLLs, SRC, GPC, ELE responder, LPI2C +
  PMICs, and a DPU command-sequencer stub. Everything else is a logging stub.

All memory-map addresses and IRQ numbers come from the NXP BSP, never guessed:
the Linux DTS
(`references/linux-imx/.../imx95.dtsi`) for peripherals the kernel sees, and
the U-Boot RM-derived header for the SCMI-routed peripherals SPL pokes before
SCMI is up. The RM is authoritative for register behaviour; the DTS for
addresses and IRQ numbers.

## Repository tour

| Path | Purpose |
| --- | --- |
| `hw/arm/fsl-imx95.c`, `include/hw/arm/fsl-imx95.h` | SoC realization: CPUs, GIC, M33, device wiring, memory map, logging stubs |
| `hw/arm/imx95-evk.c`        | 19x19 EVK board file |
| `hw/char/imx_lpuart.c`      | LPUART model (console) |
| `hw/misc/imx_mu.c`          | NXP MU (V2) model — doorbell + peer cross-connect |
| `hw/i2c/imx_lpi2c.c`        | LPI2C master (interrupt-driven) |
| `hw/timer/imx95_sysctr.c`   | system-counter clockevent timer |
| `hw/misc/imx95_{src,anatop,gpc,pmic,dpu,ele_server,wdog}.c` | SM/Linux bring-up device models |
| `tests/swap-boot/run.sh`    | full Linux-on-real-SM boot |
| `tests/sm-banner/run.sh`    | SM standalone to its debug monitor |
| `scripts/probe_stall.py`    | A55-hang frame-pointer debugger ([scripts/README.md](scripts/README.md)) |
| `docs/imx95/methodology.md`       | debugging methodology + the project's working mode |
| `docs/imx95/known-limitations.md` | full limitation diagnoses |

## Building

    mkdir -p build && cd build
    ../configure --target-list=aarch64-softmmu
    ninja qemu-system-aarch64

Host packages (verified on Ubuntu 22.04): `meson ninja-build` for the QEMU
build; `binutils-aarch64-linux-gnu gcc-aarch64-linux-gnu` for the bare-metal
test + a static initramfs init; `bison flex libssl-dev libgnutls28-dev
efitools` for the U-Boot SPL test. The standard build chain (gcc, libc-dev,
pkg-config, python3, libglib2.0-dev, libpixman-1-dev) is assumed present.

## Smoke tests

    # machine registers
    ./build/qemu-system-aarch64 -M help | grep imx95

    # bare-metal hello (no SM firmware needed - useful to verify the build
    # itself before assembling the boot artifacts)
    cd tests/hello-imx95 && make && cd ../..
    ./build/qemu-system-aarch64 -M imx95-19x19-evk -nographic -m 2G \
        -kernel tests/hello-imx95/hello.bin      # -> "Hello from i.MX 95!"

    # SM standalone to its monitor
    SM_ELF=<m33_image.elf> tests/sm-banner/run.sh

    # full Linux to userspace on the real SM
    tests/swap-boot/run.sh

## Methodology & contributing

The project is built measure-first: propose a hypothesis, verify it against
data, and re-plan when measurement disagrees. The debugging techniques that
recur (the "five pillars"), the register-class triage pattern, and the
working mode itself are written up in
[`docs/imx95/methodology.md`](docs/imx95/methodology.md). Per-milestone design and review
notes live in `docs/reviews/` (local working artifacts).

## Milestone history

- **v0.0.1–v0.0.2** — scaffold; real memory map from the DTS; `imx.lpuart`
  model + LPUART1 console; bare-metal hello.
- **v0.1** — U-Boot SPL banner. NXP MU model, SCMI server stub, ELE responder,
  watchdog stub.
- **v0.2** — SPL → U-Boot proper over an emulated SD boot chain. `TYPE_IMX_USDHC`,
  the uSDHC SDMA-boundary fix, AHAB-container SD boot.
- **v0.3** — U-Boot interactive prompt. LPI2C, `psci-conduit=smc`, USB stubs,
  the LPUART WATER RX-count fix.
- **v0.4** — Linux 6.12.49 boots via `-kernel`: 6 A55s up via PSCI, GICv3, SCMI
  mailbox transport.
- **v0.5** — Linux to userspace (on the C-stub SCMI server); the cpuidle hang
  root-caused and worked around with `cpuidle.off=1`.
- **v0.6** — Cortex-M33 SM core; loads + executes the real NXP SM firmware.
- **v0.7** — SM through early init + PMIC/IO-expander (BBNSM, WDOG2, XCACHE,
  HSIOMIX, LPI2C + PF09/PCAL6408A).
- **v0.8** — SM to its debug monitor (GPC modelled).
- **v0.9** — the real SM serves Linux's SCMI: dual-aperture MU2 cross-connect +
  the SM driven through full init (PF53, FSB, SRC, eMcem, ELE, ANATOP/ARM-PLL,
  config-ops) so `LMM_Init` enables the AP mailbox.
- **v0.10** — Linux boots to userspace **on the real SM**: walked the
  deferred-probe cascade (eDMA, the DPU command-sequencer, clock-provider
  syscons, netc, usb3) to `/init`.
- **v1.0** — polish: boot time cut ~9× (icount off the default boot path), the
  system counter modelled as a real clockevent timer, the vestigial C-stub SCMI
  server removed (real SM is now the only provider), and documentation.
