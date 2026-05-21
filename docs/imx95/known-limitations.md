# qemu-imx95 — Known limitations

These are the things qemu-imx95 does **not** do, with the diagnosis for each.
The recurring theme: where a limitation exists, it is named precisely and
attributed to the correct layer (the i.MX 95 machine model, vs. QEMU core, vs.
a universal property of software SoC emulation).

---

## 1. Deep cpuidle requires `cpuidle.off=1`

**Symptom.** Booting Linux without `cpuidle.off=1` on the cmdline hangs the
machine shortly after `arm-scmi ... SCMI Notifications - Core Enabled`.

**Diagnosis (two layers, both characterized).** Linux's only CPU idle state,
`cpu-pd-wait`, has the `local-timer-stop` property, so entering it requires a
working **broadcast clockevent** and quiesces the per-CPU GIC interface:

1. **Broadcast timer (machine-model layer — fixed).** With `local-timer-stop`,
   an idling core shuts down its per-CPU arch timer and depends on a broadcast
   clockevent to be woken for the next tick. On i.MX 95 that is the system
   counter (`timer@44290000`). It is modelled for real in
   `hw/timer/imx95_sysctr.c`: a live 24 MHz up-counter, the CMPCV/CMPCR compare
   registers, and a compare-match interrupt on GIC SPI 72. (Before this model,
   the counter was a RAM stub with no live count and no IRQ, so idle cores
   never woke — the original reason `cpuidle.off=1` was needed.)

2. **GIC WakeRequest (QEMU-core layer — not fixed).** Once the broadcast timer
   lets Linux enter the deep state, the failure moves: on entry, Linux's GIC
   `cpu_pm` notifier disables the core's GIC CPU interface (group disable / PMR
   mask), then the core WFIs (QEMU's PSCI `CPU_SUSPEND` is a bare `helper_wfi`).
   A subsequent wake interrupt — a `stop_machine`/reschedule **SGI**, the arch
   **PPI**, or an **SPI** — is held pending in the redistributor but never
   delivered: `gicv3_cpuif_update()` only asserts `parent_irq` (the sole path
   that breaks a QEMU CPU out of WFI) when `icc_hppi_can_preempt()` is true,
   i.e. the interface is enabled. The redistributor stores
   `GICR_WAKER.ProcessorSleep`/`ChildrenAsleep` as register bits only — there
   is **no WakeRequest** that wakes a quiesced core on a pending interrupt.

   GIC trace at the hang (CPUs 1–5, each with a pending wake interrupt that is
   not asserted):

   ```
   CPU i/f 0x200  HPPI irq 1   group 2 prio 192 -> setting IRQ 0   (SGI 1, the stop_machine IPI)
   CPU i/f 0x400  HPPI irq 104 group 2 prio 192 -> setting IRQ 0
   CPU i/f 0x100  HPPI irq 26  group 2 prio 255 -> setting IRQ 0   (arch-timer PPI)
   ```

**Attribution.** The i.MX 95 machine model is correct — the broadcast timer
that this platform needs is modelled. The remaining gap is in QEMU core
(`hw/intc/arm_gicv3_cpuif.c` / `arm_gicv3_redist.c`) and affects *every* GICv3
machine whose guest quiesces the CPU interface before WFI; it is just rarely
exercised because most QEMU arm64 setups don't enable such idle states.

**Fix shape (deferred).** Model the architectural WakeRequest: wake a halted
CPU on a pending, distributor/redist-enabled interrupt even when the CPU
interface is quiesced; the guest's cpuidle-exit path re-enables the interface
and takes the interrupt. This wants upstream-grade care (avoid spurious-wake
loops) and is filed as a follow-up to qemu-devel
(`docs/reviews/deferred-community-writeups.md`).

**Workaround.** `cpuidle.off=1`. Linux then uses shallow `arch_cpu_idle` WFI on
the still-running per-CPU arch timer and boots cleanly to userspace.

---

## 2. `-icount` inflates the boot ~9× (do not use it for normal boots)

`-icount shift=auto` is a debug-determinism tool, not a normal-boot config. On
this heterogeneous A55+M33 machine, where both sides can sit in WFI between
SCMI/MU round-trips, icount delays interrupt/timer delivery to idle CPUs, so
Linux's `wait_for_completion_timeout()` waits run their full duration in
virtual time instead of completing on the IRQ. Measured boot-to-`/init`:

| config | time to /init |
| --- | --- |
| `-icount shift=auto` | 114.5 s |
| no icount | **13.2 s** |

Both produce the identical set of probe-timeout warnings — icount isn't adding
work, it inflates virtual time. The boot path therefore runs **without** icount
by default; it remains available behind `ICOUNT=1 tests/swap-boot/run.sh` for
when a race actually needs deterministic, non-perturbing tracing (see
methodology Pillar 3). This is also a general QEMU property, filed as a
community writeup.

---

## 3. Hardware accelerators (GPU / VPU / NPU) are probe-time stubs only

The Mali-G310 (GPU), Amphion (VPU), and Neutron (NPU) blocks are stubbed so
their Linux drivers complete registration with the DRM / V4L2 / NPU subsystems,
but **no actual rendering, codec, or inference occurs**. Userspace requiring
hardware acceleration (glmark2, `ffmpeg` hwaccel, NPU inference) will not work.

This is universal across SoC software emulators: no public software emulator of
any modern SoC's GPU/VPU/NPU exists (Snapdragon, Apple Silicon, Tegra, Exynos,
i.MX — none). The complexity is on the order of writing a full accelerator
driver in reverse, and the result would be orders of magnitude slower than
silicon while still not being functionally useful for accelerator development.
Accelerator work requires real silicon or cycle-accurate RTL simulation; this
emulator targets BSP / SM-firmware / peripheral-driver development and CI.

Similarly, the display controller (DPU) is stubbed only far enough for its
Linux probe to complete (`hw/misc/imx95_dpu.c` reports the command sequencer
idle with FIFO space); there is no scanout.

---

## 4. Some peripheral drivers complete probe with soft failures

A number of Linux drivers for peripherals not on the boot critical path (some
I2C/SPI/PCIe/USB devices, MMC card detection without an actual card, etc.) are
backed by logging stubs and either defer probe or fail soft with `-ENODEV`.
These produce warnings in the boot log but do not panic or hang Linux —
userspace runs cleanly (PID 1 via initramfs). A fully populated runtime
environment (e.g. mounting an actual SD card filesystem, exercising real
I2C-attached sensors) would require modelling more peripherals for real. The
serial console (LPUART1 for Linux, LPUART2 for the SM debug monitor) works.

---

## On "stock NXP DTB boots"

The project boots the **unmodified** NXP EVK device tree — limitations are
handled by modelling or stubbing devices in the QEMU machine, never by editing
the DTB to hide hardware. Where a limitation can't be closed from inside the
machine model (cpuidle's GIC WakeRequest, icount's idle behaviour), it is an
honest, documented gap at the QEMU-core layer, not a fidelity compromise in the
machine.
