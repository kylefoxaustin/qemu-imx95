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

The Mali-G310 (GPU), Amphion (VPU), and Neutron (NPU) blocks are logging
stubs: their MMIO is mapped and accesses are absorbed, but **no rendering,
codec, or inference occurs**. The precise failure mode for each driver is
characterized below from a Tier-3.4 live test with a glibc Mesa userspace
(see `docs/imx95/validation-report.md` for the full evidence):

- **Mali GPU.** The driver fails probe with `-22` (EINVAL) at the product-ID
  check: it reads the stub's GPU-ID register, sees `Unknown GPU Product ID 0`,
  logs "Early device initialization failed" and unbinds. No `/dev/dri/cardN`
  is registered by Mali, and no Vulkan ICD is installed. `vulkaninfo` reports
  `Found no drivers! ERROR_INCOMPATIBLE_DRIVER`. `glmark2-es2` and other
  GL/Vulkan tools have nothing to attach to.
- **DPU (display controller, `4b400000`).** Probes cleanly
  (`hw/misc/imx95_dpu.c` reports the command sequencer idle with FIFO space)
  and registers `/dev/dri/card0` + `renderD128` — so a DRM card *does* appear
  in `/dev`, but it has no connectors and no scanout. `modetest` (which probes
  by hardcoded driver-name list — i915/amdgpu/radeon/nouveau/vmwgfx — and
  doesn't match the i.MX stub) fails to attach; `kmscube` errors with
  `no connected connector! failed to initialize legacy DRM`.
- **Amphion VPU / Neutron NPU.** Logging-stub MMIO, no V4L2 stream/inference.
- **Display interfaces (MIPI DSI `4acf0000`, LVDS/LDB `4b0c0000`) and camera
  (MIPI CSI0/1 `4ad30000`/`4ad40000`, ISI `4ad50000`, NEO ISP `4ae00000`).**
  Unimplemented stubs (read-as-0) — present so enabling the DT nodes degrades
  gracefully instead of faulting, but there is no panel/scanout output and no
  camera capture. The display-mix CSRs and the DPU command-sequencer stub let
  the `dpu95`/DRM stack probe, but nothing reaches a connector. A real display
  path (DPU scanout + a fixed connected output surfaced to a QEMU window) and
  camera capture (a V4L2 source) are **roadmap items**, after NETC networking.

Userspace requiring hardware acceleration (glmark2, `ffmpeg` hwaccel, NPU
inference, anything wanting a working Mali ICD) will not work; nor will
on-screen display output or live camera capture.

This is universal across SoC software emulators: no public software emulator of
any modern SoC's GPU/VPU/NPU exists (Snapdragon, Apple Silicon, Tegra, Exynos,
i.MX — none). The complexity is on the order of writing a full accelerator
driver in reverse, and the result would be orders of magnitude slower than
silicon while still not being functionally useful for accelerator development.
Accelerator work requires real silicon or cycle-accurate RTL simulation; this
emulator targets BSP / SM-firmware / peripheral-driver development and CI.

---

## 4. Some peripheral drivers complete probe with soft failures

A number of Linux drivers for peripherals not on the boot critical path (some
I2C/SPI/PCIe/USB devices, MMC card detection without an actual card, etc.) are
backed by logging stubs and either defer probe or fail soft with `-ENODEV`.
These produce warnings in the boot log but do not panic or hang Linux —
userspace runs cleanly (PID 1 via initramfs). A fully populated runtime
environment (e.g. mounting an actual SD card filesystem, exercising real
I2C-attached sensors) would require modelling more peripherals for real.

**The Linux serial console works in both directions** — LPUART1 for Linux's
stdin/stdout, LPUART2 for the SM debug monitor. The interactive shell from a
BusyBox initramfs accepts typed input and displays output on the host terminal.
(A bug in the LPUART model's FIFO status handling was fixed in commit
741af2f5e3, which also resolved a downstream SCMI desync caused by RX interrupt
storms — one root cause, two symptoms.)

---

## 5. Cortex-M7 — in-progress across v1.x

The i.MX 95 SoC has a heterogeneous CPU topology of 6× Cortex-A55,
1× Cortex-M33, and 1× Cortex-M7. The M33 has been modelled since v0.6
(it runs the real NXP System Manager firmware and is Linux's sole SCMI
provider). The Cortex-M7 real-time domain is being added across the v1.x
milestone; **completion of all v1.x M7 work is the gating milestone for
upstream submission to qemu-arm@**, so the artifact submitted to the
QEMU community represents the full i.MX 95 CPU complement honestly,
not a partial SoC.

The v1.x execution plan has six steps; what's modelled today vs. still
ahead:

| Step | Scope | Status |
| --- | --- | --- |
| 2 | Cortex-M7 CPU instance + TCMs + NVIC + default parallel reset policy + standalone hello firmware + unit/integration tests | **done** (36 h+ parallel-CPU soak passed 2026-05-25, zero anomalies) |
| 3 | M7-first reset sequencing (Scenario 1: M7 released before the A-cluster) | **done** (SRC.SCR.M7MIX-release latch + machine wiring + `tests/m7-first` integration test; 36 h stability soak passed 2026-05-28, zero anomalies, RSS stable, SRC.SCR latch sticky) |
| 4 | SM boots the M7 (real FreeRTOS) before the A-cluster; Linux `imx-rproc` **attaches** to the SM-managed core; full M7↔Linux rpmsg round-trip (Scenario 2) | **done** — see "M7 lifecycle — SM-orchestrated, Linux-attached" below; verified end-to-end with the stock NXP `imx_rpmsg_pingpong` 100-message exchange |
| 5 | M7 lifecycle: the SM boots, manages and **fault-recovers** the M7 (cold-resets the M7 LM on a fault) | **done** — see "M7 fault recovery — SM cold-resets the M7 LM" below; `tests/m7-fault-recovery` verifies the M7 faults and the real SM restarts it |
| 6 | Docs polish + v1.x validation report | **done** — methodology cross-layer patterns in [`methodology.md`](methodology.md); v1.x Steps 3–5 validation in [`validation-report.md`](validation-report.md) |

The **FlexCAN** controllers are now modelled (all five, on QEMU's CAN bus
subsystem — `hw/net/can/flexcan.c`, validated by `tests/qtest/flexcan-test.c`).
Other customer-specific real-time peripherals (TSN networking, audio SAI/DSP,
etc.) remain future scope beyond initial M7 support — none of these are
required for the M7 to demonstrate execution.

### M7 lifecycle — SM-orchestrated, Linux-attached

Unlike a generic remoteproc model where Linux owns the M-core lifecycle,
**the i.MX 95 System Manager boots the M7 before the A-cluster** (per the
SM board configuration `imx-sm/configs/mx95evk.cfg`: `LM1 M7 boot=2`,
`LM2 AP/Linux boot=3`). The AP logical machine does not own the M7 LM, so
Linux cannot start the M7; it only **attaches** to the already-running,
SM-managed core.

Linux's `imx_rproc` driver attaches via the LMM protocol:

```
imx-rproc imx95-cm7: lmm(1) not under Linux Control
```

This is the normal Linux-side behaviour on this board, not an error. (An
earlier draft of this section attributed the Step 4 probe failure to a
`size=0` `rsc_table` reserved-memory node; that hypothesis was wrong. The
real blocker was initrd/DTB **placement**: `arm_load_kernel` landed the
initrd at loader+128 MiB = `0x88000000` and the DTB just above it —
exactly the M7 remoteproc carveout — so the no-map reserved-memory pass
returned `-EBUSY`, the page stayed linear-mapped, and `ioremap` refused
it with `-ENOMEM`. The fix presets the board's initrd start clear of the
carveout.)

The M7 firmware itself is an SCMI agent: the M7's first act after reset
is an SCMI handshake with the SM via MU9 (modelled as the MUI_A5
cross-connect in `hw/misc/imx_mu.c`). This is in addition to the A55↔SM
SCMI channel via MU2.

Once the M7 is running, Linux's `imx_rproc` and the M7's rpmsg-lite
client communicate over the modelled MU7 cross-connect for kick
notifications, with payload exchange via shared vrings in the M7 DRAM
carveout (`0x88000000`). This is the standard OpenAMP architecture:
mailbox notifies, shared memory carries data.

Verified end-to-end with the stock NXP `imx_rpmsg_pingpong` kernel module
performing a 100-message ping/pong exchange in a full A55+M33+M7 boot,
reproducible via the env-gated `m7_rpmsg_pingpong` functional test
(`tests/functional/aarch64/test_imx95_evk.py`; recipe in `tests/m7-rpmsg/`).

**Known unknown (deferred to a later milestone):** MU TR/RR
back-pressure — what real i.MX 95 hardware does when `TR` is written
while `RR` is already full. The current model overwrites silently. The
tested workload (rpmsg-pingpong) does not exercise this case because the
receiver drains `RR` before the sender can issue another kick; the
correct guard behaviour needs reference-manual review before it is
modelled.

### M7 fault recovery — SM cold-resets the M7 LM

The real System Manager owns the M7's whole lifecycle, including
**fault recovery**: the M7 LM is configured (`mx95evk.cfg`:
`FAULT_M7_LOCKUP/RESET/WDOG5 reaction=lm_reset`) so that on an M7
fault the SM cold-resets the M7 logical machine. The model wires this
end-to-end:

- The M7's NVIC `SYSRESETREQ` is routed to the SM as
  `CM7_SYSRESETREQ_IRQn` on the M33's NVIC, the way the SRC routes it
  on silicon. When the M7 asserts `SYSRESETREQ` (or locks up), the SM
  takes the fault.
- The SM's fault handler runs `LMM_SystemLmReset` → shutdown then boot
  of the M7 LM: `DEV_SM_CpuStop` holds the M7 (it asserts the M7-mix
  reset lines via `SRC SLICE_SW_CTRL.RST_RSTR` and sets CPUWAIT in
  AONMIX `M7_CFG`), then `DEV_SM_CpuStart` releases it. The model turns
  the CPUWAIT set into a halt and the clear into a `cpu_reset` + resume,
  so the M7 re-runs from its ITCM reset vector.

For the SM to **manage** the M7 at all, it must learn the M7 from the
boot-ROM handover table it reads at startup
(`LMM_CpuInit` → `DEV_SM_RomBootCpuGet`). On hardware the boot ROM
loads the M7 image and records it; this emulator loads the M7 image
directly (`-device loader`), bypassing the ROM, so the machine
fabricates a minimal one-entry handover for the M7 in reserved M33 DTCM
before the SM runs. That is what lets the SM boot the M7 via
`LMM_CpuStart` (releasing CPUWAIT and enabling the fault IRQ) rather
than the machine force-starting it. The handover is M7-only by design:
the A55 boots outside the SM's LMM path, so it is unaffected.

`tests/m7-fault-recovery` proves the chain: an M7 fixture that
deliberately asserts `SYSRESETREQ` on its first boot, with a DTCM boot
counter that survives the core reset. The counter reaching 2 confirms
the M7 faulted **and** the real SM restarted it — with no SM the M7
would fault once and never come back.

### A note on the M7 fingerprint address

The Step 2 standalone M7 test firmware (`tests/cm7-hello`) writes a
fingerprint magic word + ASCII string to **M7-view `0x20000000` (the
start of the M7's own DTCM)**, observable from the A55/system side via
the architectural system-view alias of M7 DTCM at **`0x20400000`** per
the upstream Linux `imx_rproc_att_imx95_m7` attribute table.

This is **not** the same as the M-core DRAM carveout that Linux's
`imx-rproc` driver uses for remoteproc — that carveout lives in shared
DDR starting at `0x88000000` and includes the `rsc_table` at
`0x88220000` (the resource-table contract between Linux and the M-core
firmware). The Step 2 hello firmware deliberately stays out of shared
DDR entirely: a fingerprint there would have to compete with whatever
Linux placed in general DRAM (initramfs, page cache, etc.), and an
earlier draft of this firmware that wrote to a "looks-safe-enough" DDR
address (`0x88200000`, just below `rsc_table`) was found to corrupt the
initramfs and prevent Linux from booting. The right channel for a
cross-CPU fingerprint is the M7's own private memory, exposed through
its designed-in system-view alias — which is exactly the channel the
upstream remoteproc driver uses to deposit firmware *into* the M7.

The full Linux-remoteproc protocol — which uses `rsc_table` to
negotiate memory regions, virtio devices and trace buffers between
Linux and the M-core — is **not** implemented in the Step 2 hello
firmware. That belongs to the Step 4 scenario above, where the SM boots
the real FreeRTOS M7 firmware and Linux's `imx_rproc` driver **attaches**
to the already-running core (it does not start it) and reads the
resource table the firmware publishes at the DTS address.

Step 4 wires this end-to-end; Step 5 adds SM-driven M7 fault recovery
(above). The remaining v1.x work is docs polish + the validation report
(Step 6).

---

## 6. Networking — two 1G ENETC ports (v2.0.0 + second port)

The NETC block is modelled end to end: a functional GICv3 ITS + an
integrated-ECAM PCIe host + a from-scratch ENETC v4 Ethernet PF
(`hw/net/fsl_enetc.c`, PCI `1131:e101`) with a BD-ring DMA engine and MSI-X
via the ITS. The stock Linux `nxp_enetc4` driver binds it, the port links up
at 1Gbps, and traffic flows. RX BD-ring scatter and ring wraparound have
deterministic qtests (`tests/qtest/fsl-enetc-test.c`), and a 24-hour
single-port iperf load-soak over this datapath passed (4.76 B frames /
7.19 TB, ~661 Mbps avg, zero rx/tx errors or drops, flat memory).

**Two 1G ports** are now wired and tested — ENETC0 (`ethernet@0,0`, devfn
`0x0`) and ENETC1 (`ethernet@8,0`, devfn `0x40`), the 1G pair the boards pin
out. ENETC0 sits at devfn `0x0` because the real NETC ECAM is an
integrated-endpoint host with no bridge there; gpex's own root device is
relocated to the unused slot `31.0` to free it. Both ports are validated with
a **back-to-back traffic test** (`tests/netc/run-2port.sh`), which joins the
two PFs on one QEMU L2 hub and pings `eth0`↔`eth1` (with `eth1` moved into its
own netns so frames must cross the modelled wire, not loop back in the host
stack). This runs on **both** the 19×19 EVK and 15×15 FRDM device trees — the
same QEMU machine, only `-dtb` differs — exercising SoC-model generality that
single-board validation can't. The test DTB is patched
(`tests/netc/patch-dtb.py`) to enable both ports as `fixed-link`, drop the
absent efuse MAC, and rewrite the `msi-map` to identity (QEMU's ITS uses the
PCI requester-ID as the DeviceID). The PFs attach to `-nic` backends (not bare
`-netdev`).

**Third port (ENETC2 / 10G) — done, two ways (v2.1.0 fixed-link, v2.4.0 real
chain).** The EVK's second physical port is ENETC2 (`ethernet@10,0`, devfn
`0x80`), a **10G `10gbase-r`** interface with in-band-status link semantics. It
comes up two ways: (a) as a third fixed-link PF (`tests/netc/run-10g.sh`), and
(b) — the v2.4.0 "Path B" work — through the **fully real PHY chain**: a new
NETC EMDIO controller (`hw/net/imx95_netc_emdio.c`, a PCI endpoint on a second
GPEX host for the bus-1 ECAM `pcie@4cb00000`) fronting an Aquantia AQR113C c45
PHY, plus the port's internal DesignWare xPCS. The stock `ethernet@10,0` node
(real `phy-handle`, `managed = "in-band-status"`) links at 10 Gbps with the
`Aquantia AQR113C` driver bound and no `pcs_config` timeout
(`tests/netc/run-real-10g.sh`). The key fix was a model-fidelity one: the ENETC
PF advertised PCI Revision ID 1, and the driver routes a rev1 in-band PCS
through a Lynx PCS but a rev4 one through the DW xPCS — so a stray revision byte
silently forked the driver down the wrong subsystem. On the older
**`imx95-scaffold`** reference branch (pure v1) networking is not present — NETC
is the additive v2.0.0 work, now part of the default `imx95-netc` branch.

**Downstream note — the BSP rootfs `apt` setup.** Even with outbound
networking via `-nic user` (slirp), NXP's BSP rootfs
(`imx-image-full-imx95evk`) ships `/usr/bin/apt`
configured with a populated `/etc/apt/sources.list.d/`; without a
reachable mirror, package downloads can still fail. Three
working paths for downstream users who need to add packages to the
rootfs:

1. **Pre-bake via Yocto** — edit the image recipe (or a local
   `bbappend`) to add `IMAGE_INSTALL:append = " <pkg-dev> <pkg2-dev>"`
   and `bitbake imx-image-full`. The cleanest long-term posture for any
   workflow that wants a self-contained rootfs.
2. **Pre-populate the apt cache** — `apt download` the needed `.deb`
   files on the host (against NXP's repos), copy them into the rootfs's
   `var/cache/apt/archives/` before booting, then `apt install -y
   --no-download <pkg>` from inside the guest.
3. **Cross-compile from a Yocto SDK** — `bitbake imx-image-full -c
   populate_sdk` produces the standard NXP toolchain installer; install
   to `/opt/fsl-imx-xwayland/walnascar/`, source the
   `environment-setup-armv8a-poky-linux` script, cross-build, bind-mount
   the result into the rootfs.

This is a rootfs/mirror-reachability matter, not a separate model bug.
Surfaced during the first non-author install-target evaluation (see
`validation-report.md`'s "First-customer ecosystem readiness check"
section).

---

## 7. Intentionally-deferred model fidelity (with reasons)

A handful of features are modelled to the point Linux needs and then stop
short, by deliberate choice. None of these is a *silent-wrong* — the worst
class, where a model returns a plausible-but-incorrect result; each is either an
honest "not exercised", an honest "no data", or a clean workaround. They are
recorded here so the scope is explicit rather than surprising.

- **MICFIL capture rate + SPDIF/XCVR TX rate — hardcoded, not derived (the
  clock Hz lives in the SM, not the model).** `MICFIL_WORD_NS` (48 kHz) and
  `XCVR_TX_WORD_NS` (96 kHz) are hardcoded scaffold, the same class as the SAI
  fixed-48-kHz bug that *was* fixed (SAI's rate authority is the wm8962 codec,
  which announces it over a wired GPIO — MICFIL/XCVR have no such authority). On
  silicon the rate is `pdm_mclk / (CLKDIV·OSR·8)` (MICFIL) / `phy_clk / 64`
  (XCVR), where the driver keeps the divisors constant and puts the rate in the
  clock via `clk_set_rate`. On i.MX95 that goes over SCMI to the SM, which
  programs the AUDIO_PLL + root — **and the resulting Hz is computed by the SM
  firmware's clock-tree walk (`DEV_SM_ClockRateGet`), not stored in any register
  the model can read**: `hw/misc/imx95_ccm.c` holds the root CONTROL words but
  not the Hz, and `hw/misc/imx95_anatop.c` is a PLL lock-stub with no analog
  rate. So the rate is genuinely *not observable in the model* without
  replicating the SM's AUDIO_PLL fractional-N + PDM/SPDIF-root MUX/DIV tree in
  QEMU.

  This is confirmed, not assumed: an anti-fabrication boot (91emulator's guard —
  dump the guest's own `clk_summary` during an active capture) measured the SM
  really does feed a genuine `pdm_mclk = 49152000` (= 48000 × 1024) at capture
  time. So the rate is REAL and derivable *in principle* — the scaffold is not a
  silent-wrong (nothing consumes a wrong rate: the capture/spdif tests exercise
  only the one rate the hardcode matches). It is deferred because deriving it
  means modelling a clock tree that i.MX95 deliberately does *not* model (the
  whole "SM owns clocks" architecture), which is out of proportion to a latent
  bug. Recorded with the measured `49152000` for whoever models the PLL tree.

- **DPU 2D blit — ROP9 raster ops (deferred by consumer-touched-fields).** The
  blit engine models fill, copy, Porter-Duff alpha-blend, nearest-neighbour
  scale, rotate and CSC (qtested). It does **not** model ROP9 raster ops
  (AND/OR/XOR of source with destination): the mainline `dpu95-blit` driver
  leaves `ROP9_CONTROL` at its reset (passthrough), which a plain copy already
  represents correctly, and **no in-tree consumer programs a raster op**. The
  `ROP9_CONTROL` op-code encoding is RM-only (absent from the driver headers),
  so modelling it from a guessed layout would risk *breaking* the working
  passthrough/copy path for zero observable gain. Left at passthrough on
  purpose.

- **DPU 2D blit — `g2d` userspace end-to-end (proprietary ceiling).** The blit
  is exposed as a DRM render node and validated by qtest. A full
  userspace-`g2d`-library 2D op requires NXP's proprietary `libg2d` stack — the
  same ceiling as the Mali GPU and Amphion VPU (section 3): Linux sees the
  device, full userspace acceleration is not emulatable.

- **OCOTP fuse nvmem — MAC via DT workaround (ELE-gated).** The FSB fuse shadow
  is seeded with real NXP-OUI MACs, but the `fsl,imx95-ocotp` nvmem driver
  hard-defers on `imx_get_se_data_info()`, which needs the EdgeLock (`fsl-se`)
  secure-enclave driver to fully probe. That probe is a multi-step ELE exchange
  (SoC info + FW version + `imem_state` + `soc_device_register` + an SE-firmware
  authenticate path) against an ELE responder that is a deliberate v0.1 stub. So
  the ENETC MAC is supplied via a `local-mac-address` DT patch in the test
  harnesses instead of resolving from the efuse nvmem cell. Closing this means a
  real ELE bring-up to remove a clean, universal workaround — low ROI, deferred.

- **Audio SAI capture — no codec ADC route (shared with i.MX91).** Playback
  (WM8962/SAI3) and PDM capture (MICFIL) work end-to-end; **SAI RX capture** does
  not, because there is no codec-driven `RCSR.RE` ADC-DAPM route modelled (the
  same WM8962-ADC-DAPM gap the i.MX91 has). Capture returns silence, not wrong
  data.

- **Audio BT-SCO — registration-bar (dummy codec).** The BT-SCO ASoC card
  registers, but there is no SCO datapath because there is no real SCO codec
  behind it. Honest absence, not wrong data.

---

## On "stock NXP DTB boots"

The project boots the **unmodified** NXP EVK device tree — limitations are
handled by modelling or stubbing devices in the QEMU machine, never by editing
the DTB to hide hardware. Where a limitation can't be closed from inside the
machine model (cpuidle's GIC WakeRequest, icount's idle behaviour), it is an
honest, documented gap at the QEMU-core layer, not a fidelity compromise in the
machine.
