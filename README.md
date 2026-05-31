# qemu-imx95

A QEMU machine type for the NXP **i.MX 95** SoC, targeting the **19x19 EVK**
(LPDDR5) variant.

> **This is a fork of QEMU mainline.** The i.MX 95 work lives on the
> `imx95-scaffold` branch, from its tip back to the upstream branch point
> `edcc429e9e`; the vast majority of the ~129k-commit history is inherited
> from upstream QEMU. The upstream QEMU README is preserved at
> [`README.rst`](README.rst) — this file describes the i.MX 95-specific work.

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

**Maintainer:** Kyle Fox ([@kylefoxaustin](https://github.com/kylefoxaustin))
(see [`MAINTAINERS`](MAINTAINERS) for the canonical entry).

## Quickstart for newcomers

This fork **builds and runs as-is** — everything needed to *use* the i.MX 95
machine is already on the `imx95-scaffold` branch. Nothing needs patching.
(The `docs/reviews/` patch artifacts are for *contributing back* to upstream
QEMU; you never apply them to use this fork.)

**1. Clone and build** — a standard QEMU build (verified on Ubuntu 22.04+; see
[Building](#building) for host packages):

    git clone https://github.com/kylefoxaustin/qemu-imx95.git
    cd qemu-imx95
    mkdir build && cd build
    ../configure --target-list=aarch64-softmmu
    ninja qemu-system-aarch64
    ./qemu-system-aarch64 -M help | grep imx95     # -> imx95-19x19-evk

**2. First boot in seconds — no external artifacts.** The in-tree Cortex-M7
firmware boots the M7 standalone and writes a fingerprint the test checks. You
need a bare-metal `arm-none-eabi` toolchain (Ubuntu: `gcc-arm-none-eabi`). Run
from the repo root (`cd ..` if you're still in `build/`):

    make -C tests/cm7-hello TOOLCHAIN=arm-none-eabi-
    tests/m7-boot/run.sh        # -> "M7 fingerprint 0xC0FFEE07 detected"

That proves the emulator end-to-end without downloading anything from NXP.

**3. The full stack — Linux + System Manager + M7.** Booting Linux to userspace
needs four artifacts built from NXP sources (the SM firmware, a kernel `Image`,
a DTB, an initramfs). They are not redistributable, so they aren't in the repo —
see [Required artifacts](#required-artifacts) for the build recipes and
[Quick start](#quick-start) for the boot command. The repo ships the initramfs
builder (`tests/busybox-initramfs/build.sh`), so the real work is just building
the NXP SM firmware + a kernel. For the M7's SM-managed lifecycle (boot,
rpmsg, fault recovery), see
[`docs/system/arm/imx95-evk.rst`](docs/system/arm/imx95-evk.rst).

## Scope: what's modelled and what's deferred

The i.MX 95 SoC has a heterogeneous CPU topology of **6× Cortex-A55** (the
application cluster), **1× Cortex-M33** (the System Manager core), and
**1× Cortex-M7** (the real-time domain). **This machine models all three CPU
complements.** The M33 runs the real NXP SM firmware (Linux's sole SCMI
provider); the SM in turn **boots, manages, and fault-recovers the M7**
alongside the A-cluster — the full i.MX 95 CPU complement, not a partial SoC.

The M7 integration was the `imx95-v1.x` milestone, delivered in six steps —
**all done**: the M7 TCG instance + TCMs + soak (Step 2); the silicon-faithful
SM-driven release (Step 3); SM-orchestrated boot + Linux `imx_rproc` attach +
stock `imx_rpmsg_pingpong` round-trip (Step 4); SM fault recovery, i.e. the SM
cold-resetting the M7 logical machine on a fault (Step 5); and the methodology
+ validation docs (Step 6). See
[`docs/imx95/known-limitations.md`](docs/imx95/known-limitations.md) §5 for the
per-step detail, and
[`docs/system/arm/imx95-evk.rst`](docs/system/arm/imx95-evk.rst) for the
machine documentation.

Customer-specific real-time peripherals (FlexCAN, TSN, audio DSP, etc.)
remain further out, beyond the v1.x M7 work. See
[`docs/imx95/known-limitations.md`](docs/imx95/known-limitations.md) §5 for
the full statement.

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

The SM also **boots and manages the Cortex-M7** (running real FreeRTOS
firmware), alongside the A-cluster. Linux's `imx_rproc` driver **attaches** to
the SM-managed M7 over the modelled MU7 cross-connect (it cannot start it — the
SM owns the M7 lifecycle on this board), and the stock NXP `imx_rpmsg_pingpong`
kernel module runs a **100-message** bidirectional exchange with an M7
rpmsg-lite client. The SM **cold-recovers the M7 on a fault** (`SYSRESETREQ` →
`lm_reset`) while the A-cluster boot continues unaffected.

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
- 24 h stability run (v1.0 A55 + M33 path): stable memory, data-integrity md5
  unchanged, no panics or SCMI timeouts. (The M7-integrated path has its own
  36 h soaks — see the Step 2/3 bullets below.)
- v1.x Step-5 (SM-managed M7) soak: 21 h continuous, **0 anomalies** (no
  panics / RCU stalls / SCMI timeouts / BUGs / oopses), host RSS flat (+1.1%,
  no leak), and the SM M7-release latch (`SRC.SCR` bit 12) held throughout.
  The run ended at 21 h on an external process disturbance, not a guest fault;
  a full ≥24 h soak on this path is the remaining pre-merge validation gate.
- **v1.x Step 2 (Cortex-M7) validated**: M7 instance modelled, runs
  standalone firmware end-to-end (`tests/m7-boot/`), A55 + M33 + M7
  parallel boot test passes (`tests/parallel-boot/`), and a
  **36 h+ parallel-CPU stability soak completed with 0 panics, 0 SCMI
  timeouts, 0 RCU stalls, 0 BUGs, and no memory leak**.
- **v1.x Step 3 (SM-driven M7 release) done**: `SRC_GEN.SCR`
  M7MIX-release latch + machine wiring + `tests/m7-first/` integration
  test. The Step-3 36 h stability soak passed 2026-05-28 (4154
  heartbeats, 0 panics/SCMI-timeouts/RCU-stalls/BUGs, RSS stable
  299→329 MB, SRC.SCR bit12 sticky throughout).
- **v1.x Step 4 (SM-orchestrated M7 + rpmsg) done**: the SM boots the
  M7 running real FreeRTOS firmware before the A-cluster, and Linux's
  `imx_rproc` driver attaches to the SM-managed core (it does not start
  it — the SM owns the M7 lifecycle on this board). The stock NXP
  `imx_rpmsg_pingpong` kernel module performs a full **100-message
  ping/pong exchange** with the M7 over the modelled MU7 cross-connect
  (kicks) and shared vrings (payload) in a full A55 + M33 + M7 boot.
  This also surfaced a generic ARMv7-M PMSAv7 MPU fix in
  `target/arm/ptw.c` (misaligned region base now aligned down to match
  Cortex-M silicon, rather than dropped).
- **v1.x Step 5 (SM-driven M7 fault recovery) done**: the real System
  Manager boots, manages and **fault-recovers** the M7. When the M7
  faults (asserts `SYSRESETREQ`), the SM takes `CM7_SYSRESETREQ_IRQn`
  and cold-resets the M7 logical machine (`reaction=lm_reset`) —
  stopping then restarting the core — exactly as on silicon. This
  required modelling the M7 CPU-WAIT gate (AONMIX `M7_CFG`) and
  fabricating the boot-ROM image handover the SM reads to learn it owns
  the M7. `tests/m7-fault-recovery` proves the M7 faults and the SM
  restarts it. Step 6 (docs polish / validation report) remains ahead.
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
| `hw/misc/imx95_aonmix.c`    | AONMIX M7 CPU-WAIT gate (v1.x Step 5 — the SM's M7 run/hold control) |
| `tests/swap-boot/run.sh`    | full Linux-on-real-SM boot |
| `tests/sm-banner/run.sh`    | SM standalone to its debug monitor |
| `tests/cm7-hello/`, `tests/m7-boot/` | in-tree M7 firmware + standalone M7 boot test |
| `tests/m7-fault-recovery/`  | SM-driven M7 fault recovery test (v1.x Step 5) |
| `tests/functional/aarch64/test_imx95_evk.py` | env-gated functional test (Linux boot + M7 fault recovery) |
| `scripts/probe_stall.py`    | A55-hang frame-pointer debugger ([scripts/README.md](scripts/README.md)) |
| `docs/system/arm/imx95-evk.rst`   | QEMU-conventions machine documentation (upstream prep) |
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

    # M7 fingerprint (no SM firmware needed - verifies the M7 instance + TCMs)
    make -C tests/cm7-hello TOOLCHAIN=arm-none-eabi-
    tests/m7-boot/run.sh     # -> "M7 fingerprint 0xC0FFEE07 detected"

    # SM standalone to its monitor
    SM_ELF=<m33_image.elf> tests/sm-banner/run.sh

    # full Linux to userspace on the real SM
    tests/swap-boot/run.sh

## Methodology & contributing

The project is built measure-first: propose a hypothesis, verify it against
data, and re-plan when measurement disagrees. The debugging techniques that
recur (the "five pillars"), the register-class triage pattern, the v1.x
cross-layer patterns (multi-layer probe failures, generic bugs surfaced by
platform-specific symptoms, one device model serving multiple protocols, "a
wall is a hypothesis"), and the working mode itself are written up in
[`docs/imx95/methodology.md`](docs/imx95/methodology.md); a fresh-eyes review
discipline (the GitHub render-cache pitfall) is in
[`docs/imx95/reviewer-discipline.md`](docs/imx95/reviewer-discipline.md).
Per-milestone design and review notes live in `docs/reviews/` (local working
artifacts).

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
- **v1.0.1** — LPUART RX read-only-bit storm fix (commit `741af2f5e3`).
- **v1.x** — Cortex-M7 integration, completing the i.MX 95 CPU complement
  (6× A55 + M33 SM + M7). Six steps: M7 instance + parallel boot + 36 h soak
  (Step 2); silicon-faithful SM-driven release via `SRC_GEN.SCR.M7MIX_RELEASE`
  + 36 h soak (Step 3); SM-orchestrated boot + Linux `imx_rproc` attach + stock
  NXP `imx_rpmsg_pingpong` 100-message exchange + a generic ARMv7-M PMSAv7 fix
  in `target/arm/ptw.c` (Step 4); SM-driven M7 fault recovery via
  `CM7_SYSRESETREQ_IRQn` → `lm_reset` (Step 5); methodology + validation docs
  (Step 6). Tagged `imx95-v1.x` at commit `42d3e4ef7b`.
