# i.MX 952 QEMU Parity Playbook — for Claude Code

**Audience:** the Claude Code instance building `qemu-imx952` from the `qemu-imx95`
codebase.
**Goal:** bring the 952 model from "base port mostly boots" up to the fidelity
level `qemu-imx95` is at — i.e. a machine real developers can trust as a
stand-in for silicon, with **no silent-wrong answers**.
**Author:** the 95 Claude Code session (Kyle's fleet). You have the full 95 git
repo + history, so this doc references real commits — `git show <sha>` any of
them for the exact diff and rationale.

> Read this top to bottom once. §4 (Methodology) and §7 (Worklist) are the parts
> that are *not* already visible in the code you inherited — that's the real
> knowledge transfer. Everything else is a map.

> **STATUS UPDATE (2026-07-02):** this playbook was written *before* the 952 was
> booted. Since then the 95 Claude session cloned your `qemu-i.MX952`, booted it,
> and did the **P0 work already** — the 952 now boots stock BSP Linux to
> userspace on the real SM, A55 at 1.704 GHz, ENETC eth0 up; the CCM/PLL/MU/ENETC
> fidelity fixes are ported and pushed. **Start with `docs/952-bringup-notes.md`
> in the 952 repo** for the current state, the boot recipe, the root-cause of the
> one blocking bug (an unmapped 952 TRDC → M33 BusFault → dead SCMI), and the
> live worklist. Then use THIS doc as the methodology/reference. §7's P0 below is
> marked done; P1–P4 are still yours.

---

## 0. TL;DR — the five things that matter

1. **The base port booting is ~20% of the job.** The other 80% is *fidelity*:
   making every block either compute the right answer or fail honestly. A block
   that boots and returns garbage is worse than one that isn't there.
2. **Your #1 tool is the silent-wrong hunt** (§4a): boot Linux with
   `-d guest_errors,unimp`, tally the model warnings, and chase every register
   the guest reads that the model returns 0/garbage for. This is how I found the
   biggest bugs (A55 clock reading 24 MHz instead of 1.8 GHz, ENETC MAC = 0).
3. **The SM firmware (imx-sm) is your oracle.** The M33 System Manager reads your
   models' registers and reports values to Linux over SCMI. When a rate/status is
   wrong in Linux, trace *what register the SM read* — the bug is almost always
   "the model returned 0 for a register the SM does read-modify-write on."
4. **Seed-at-reset, not at-realize.** A register seeded in `realize()` is wiped by
   the post-realize device reset. Seed in the reset path. (Cost me a real bug —
   `c559fca7465`.)
5. **Verify guest-visible, before/after.** Don't trust "the code looks right."
   Boot it, read the value the guest reads (`clk_summary`, `/sys/.../address`,
   qtest), and diff against the stub. Every fix in §3 has a before/after proof.

---

## 1. The north star — what "parity" means

Kyle's standing fleet directive (the board farm: ~10k devs running arbitrary code
on 4 emulators — 95/952/93/91). Parity = the 952 hits all of these:

- **(1) Upstream-clean.** Code that could go to qemu-devel: checkpatch-clean,
  idiomatic QOM devices (not RAM hacks), proper `MemoryRegionOps`, vmstate.
- **(2) Uniform test/result matrix** across the emulators (same harness shape,
  same PASS/FAIL semantics — see §8).
- **(3) Identical README structure** (see §8 for the section order).
- **(4) Stand-in for real boards with NO silent-fail** on known-good IP. This is
  the big one. Non-emulatable *compute* (GPU/VPU/NPU proprietary shaders/ISA) is
  exempt from computing — but Linux must still **see** the device and it must
  **fault honestly**, never return garbage with a success flag.
- **(5) Working I/O hookup** (ethernet/USB/SPI/UART) so two emulators can talk.
- **(6) No upstreaming until all of the above are done + tested.**

**The worst bug class, ranked explicitly:** a block that signals *done* and
returns garbage. Worse than a fault, worse than a missing device. Hunt these
first. "Honest 0 / honest fault" beats "confident wrong answer" every time.

---

## 2. Ground truth — your oracles & environment

You need these to do the fidelity work. Confirm you have them for 952:

| Oracle | Path (95) | 952 equivalent |
|---|---|---|
| SM firmware source | `~/Documents/nxp/sources/imx-sm/devices/MIMX95/` | **`devices/MIMX952/`** (already exists in imx-sm — that's the delta oracle) |
| SM device drivers | `imx-sm/devices/MIMX9/drivers/` (shared: fsl_ccm.c, fsl_fract_pll.c, fsl_clock.c) | mostly shared MIMX9 — check MIMX952 overrides |
| Built SM ELF | `imx-sm/build/mx95evk/m33_image.elf` | build `config=mx952evk` (or whatever the 952 board target is) |
| Linux driver source | `references/linux-imx/drivers/` | same tree; grep for `imx952`/`imx95` compatibles |
| U-Boot source | `references/uboot-imx/` | same |
| NXP BSP DTS/kernel | see `reference_nxp_bsp` memory | 952 dtb + Image |

**Key principle:** the SM firmware is *real code* you can read. When Linux gets a
wrong value over SCMI, open the SM's `dev_sm_*.c` / `fsl_*.c` and find the exact
register read. That read *is* the spec for what your model must return. Example:
`DEV_SM_ClockRateGet` → `CCM_RootGetRate` → `reads CLOCK_ROOT[n].CONTROL.RW` →
your CCM model must return the divider the SM wrote there.

**Environment gotchas (from 95):**
- Boot needs `cpuidle.off=1` on the kernel cmdline (SCMI-IRQ + GIC cpuif idle
  race; see `project_v05_scmi_gic_hang`).
- M33 SM is loaded via `-device loader,file=m33_image.elf,cpu-num=6`, gated to run
  on ITCM firmware via a machine-done bottom-half. `max_cpus` includes the M33.
- Measure **host** per-thread CPU (`top -H -p <pid>`) to catch a spinning vCPU —
  it's invisible to guest tooling.

---

## 3. The i.MX 95 inventory — what exists, and its fidelity tier

Every block below already exists in the code you inherited. What you can't see
from the code is **which tier each is in** and **what's known-broken**. Tiers:

- **FUNCTIONAL** — computes the right answer, integrity-verified.
- **BRINGS-UP** — Linux binds the driver; device visible; may not fully compute.
- **HONEST-FAULT** — can't compute (proprietary), faults/flags honestly, no garbage.
- **STUB** — register-echo/absorb; fine *only* if no consumer reads a meaningful
  value back (verify this! a stub under a new consumer becomes a silent-wrong).

For 952, assume each block **inherits its 95 tier only after you re-verify it on
952** — the SoC delta (§6) can move a block from FUNCTIONAL to silent-wrong.

### Boot / core / SM
- **A55 cluster + GICv3(+ITS)** — FUNCTIONAL. PSCI hotplug works.
- **Cortex-M33 System Manager** running real imx-sm firmware — FUNCTIONAL. Serves
  Linux's SCMI (clocks/pinctrl/power) over MU2 cross-connect (`v0.9` tag).
- **MU (messaging unit)** `hw/misc/imx_mu.c` — FUNCTIONAL. V2 layout; peer-linked
  A55↔M33; **core control/status block CCR0/CIER0/CSSR0/CSR0 modelled**
  (`5a7396424a5` — was a silent-0 + a 1786-line/boot log flood).
- **M7 (Cortex-M7)** FreeRTOS + rpmsg + SM fault-recovery — FUNCTIONAL
  (`imx95-v1.x-step4/5/6`).
- **ELE (EdgeLock)** `imx95_ele_server.c` — HONEST. Responder; **GET_RANDOM uses
  host CSPRNG** (was a static-seeded xorshift → identical "random" every boot =
  silent-wrong; fixed to `qemu_guest_getrandom_nofail`).

### Clocks (the subsystem I just overhauled — highest silent-wrong density)
- **CCM** `hw/misc/imx95_ccm.c` — FUNCTIONAL (`f7fe7d93317`). **Was a stub** →
  the SM read every clock-root divider/mux back as 0 → reported the A55 cores to
  Linux at **24 MHz instead of 1.8 GHz** (75× silent-wrong). Now a register model
  with the CLOCK_ROOT CONTROL RW/SET/CLR/TOG quad-alias + STATUS0 shadow.
- **ANATOP/PLL** `hw/misc/imx95_anatop.c` — FUNCTIONAL for ARM/HSIO/SYS_PLL1.
  Quad-alias PLL register file + status-mirrors-control. **SYS_PLL1 seeded at
  reset** to VCO 4000 MHz + PFD0/1/2 = 1000/800/625 MHz (`553cff93522`) — the boot
  ROM programs it and QEMU skips the ROM, so without the seed all SYS_PLL1-derived
  clocks read 0. **KNOWN GAP: DRAM/AUDIO/VIDEO/LDB PLLs still read 0** (rates are
  board/use-case-specific; left honest-0). ← *first thing to check on 952.*

### Networking (COMPUTES tier — real datapaths)
- **ENETC v4 PF** `hw/net/fsl_enetc.c` — FUNCTIONAL. From-scratch (no upstream
  ENETC): BD-ring DMA + MSI-X via ITS, integrated-ECAM PCIe host. Stock Linux →
  `eth0@1Gbps`. Multi-port (2×1G + 10G Path A). **SI primary MAC seeded in the
  reset path** (`c559fca7465` — was 00:00:00:00:00:00; two bugs: wrong register
  set *and* wiped-by-reset). RX-overrun drop model (`0a9592f921d`) so a
  back-pressured socket netdev doesn't wedge. Needs `tests/netc/patch-dtb.py`
  fixed-link override (stock dtb wires external PHYs not modeled).
- **PCIe RC (DWC)** `hw/pci-host/imx95_pcie.c` — FUNCTIONAL. pcie0+pcie1, link-up
  + arbitrary endpoint bind + MSI-X via ITS.

### Storage / block
- **uSDHC / SD / eMMC** — FUNCTIONAL read+write, card-detect via GPIO3, Weston off
  real ext4 eMMC. Fixed multi-block "card stuck busy", 512 KiB SDMA stall, and a
  device-shutdown wedge (`SDHCI_QUIRK_SDCLK_AUTO_GATE`, `71a559175e`).

### Display / multimedia
- **DPU** `imx95_dpu.c` — FUNCTIONAL scanout (six-Tux logo @1920x1200) + 2D blit
  (copy/fill). VBLANK via irqsteer + 60 Hz tick (`0b80f75a6af`). MIPI-DSI +
  rm67191 panel; LVDS/LDB path. ROP/blend/scale = partial.
- **Camera (MIPI CSI-2 → ISI)** — FUNCTIONAL (`194c3ea80a0`): 5 test-pattern
  frames off `/dev/video0`; V4L2 link-validate via the sensor's exact format.
- **Audio** — FUNCTIONAL both directions: MICFIL PDM capture + WM8962/SAI3
  playback (incl. 64-bit-TCD eDMA fix). **Gotcha: audio/jpeg kernel modules must
  come from the booted kernel's OWN tree (vermagic).**
- **JPEG dec/enc** (mxc-jpeg) — BRINGS-UP (registers `/dev/video2,3`).
- **NeoISP / GStreamer / compositing** — see the respective `tests/` dirs.

### Accelerators (the honest-fault exemplars — study these for 952)
- **Neutron NPU** `imx95_neutron.c` — HONEST-FAULT. Can't compute (proprietary
  ISA). Acks MBOX0=DONE so the driver doesn't hang, but flags honestly:
  per-inference `LOG_GUEST_ERROR` + QMP `qom-get compute-modelled=false` + an
  **operator opt-in** `neutron-uncomputed-errcode` QOM prop that returns a fault
  code in the side-band MBOX1 the driver plumbs to userspace. **Fleet lesson:
  don't fault via the completion retcode (hangs the driver) — fault via a
  side-band error_code IF the driver plumbs one to userspace.**
- **Mali GPU** `imx95_mali.c` — HONEST-FAULT (TIER-2). Linux identifies
  "arch 10.12.7" then fails `-EIO` cleanly (proprietary libmali userspace).
  **GPU_ID arch_minor MUST be 12 not 8** or you get a NULL-deref oops in
  gpuprops, not a clean fault.
- **VPU (Wave6/cnm633c)** — BRINGS-UP (binds on stub).
- **ADC** `imx93_adc.c` — was a hidden-constant silent-wrong; now **operator-driven**
  via `qom-set adc-chN <v>` (guest reads back what's injected).

### Connectivity / misc
- **FlexCAN** (from-scratch, upstream-candidate) — FUNCTIONAL, 5 instances, e2e
  can0↔can1 (`393df89d6b`).
- **USB (ChipIdea/DWC3)** — usb-kbd HID enumerates. **Blocker pattern: fsl-lpi2c
  is IRQ-driven; a poll-only i2c master leaves slaves at -110** (added master IRQ
  to `imx_lpuart`/lpi2c).
- **LPUART** `imx_lpuart.c` — FUNCTIONAL incl. **cyclic eDMA-RX** (`110b0c2c866`;
  non-console ttys use DMA-RX — PIO-only silently drops RX). `.valid.min_access=1`.
- **eDMA, LPSPI, watchdog, MICFIL, SAI(+tcd64), DDR-PMU, USB3, I3C, SPDIF-XCVR,
  MIPI-DSI, virtio-mmio+9p, GPIO1-5, GPC/SRC/AONMIX/xcache** — all present; tiers
  from STUB→FUNCTIONAL. See `project_completeness_campaign` + `project_model_completeness`.

### Explicitly out of scope (don't chase these on 952 either)
GPU/VPU *compute*, Neutron *compute*, SM-owned infra internals, arm-smmu-v3 (spiked,
regresses ENETC — per-device dtb iommu-strips are the pragmatic path).

---

## 4. The methodology (the knowledge that isn't in the code)

### 4a. The silent-wrong hunt — your primary technique

This is how you find the bugs that matter. Repeat this loop until dry:

```bash
# 1. Boot Linux to userspace capturing every unmodelled register touch
qemu ... -d guest_errors,unimp -D /tmp/gerr.log ...

# 2. Rank by frequency + block (per-transaction floods = hot paths = high value)
grep -aoE '[a-z0-9_]+: (bad|unimplemented)[^0-9]*0x[0-9a-f]+' /tmp/gerr.log \
  | sed -E 's/0x[0-9a-f]+/0xADDR/g' | sort | uniq -c | sort -rn | head -25
```

Each "bad read" / "unimplemented device read" is a register the guest read that
the model returned 0 for = a **silent-wrong candidate**. Then, per hit:

1. **Name the register from the SoC header**, don't guess. `0x18` on the MU was
   `CSSR0` (Core Sticky Status) — found in `MIMX95_MU.h`, not by intuition.
2. **Find who reads it and why** in the SM (`dev_sm_*.c` / `fsl_*.c`) or the Linux
   driver. Is the read a rate/status the guest *acts on* (silent-wrong) or cosmetic?
3. **Check if it's part of a read-modify-write.** If the SM does
   `x = reg; x |= bit; reg = x`, a stub reading 0 corrupts the RMW → the written
   value loses bits. This was the CCM bug: the SM RMWs `CONTROL.RW` for the divider.
4. **Fix = make the model return what the guest wrote/expects.** Usually a
   register-backed model (§4b), sometimes a reset seed (§4b), rarely a computed
   status mirror.
5. **Verify guest-visible before/after** (§4c).

A count that equals a known transaction count (e.g. the SCMI request count) tells
you it's in a hot path. On 95 the MU `0x18` fired 1786×/boot = once per SCMI txn.

**Do this hunt on 952 early** — the delta (§6) will surface 952-specific gaps that
95 never had.

### 4b. Register-model patterns (reuse these verbatim)

- **RW/SET/CLR/TOG quad-alias.** NXP registers commonly expose one logical value
  at four offsets: `+0` assign, `+4` set-bits, `+8` clear-bits, `+C` toggle; reads
  return the accumulated value. Fold all four into one word. Reference
  implementations: `imx95_ccm.c` (CLOCK_ROOT CONTROL) and `imx95_anatop.c` (PLL).
  If you see 0x10-spaced register groups, suspect a quad.
- **Status-mirrors-control.** QEMU has no analog PLLs/power domains, so
  transitions are instant: a `*_STATUS.LOCK`/`DONE`/`BUSY` bit should be *computed*
  from the matching `*_CTRL.ENABLE`/`POWERUP` at read time, not stored. See
  `imx95_anatop.c` (PLL_LOCK mirrors POWERUP), GPC/SRC. **Never RAM-back a
  self-clearing/busy bit the guest polls — it hangs.** (CCM was safe because I
  verified nothing polls SLICE_BUSY.)
- **Seed-at-reset for boot-ROM state.** QEMU boots `-kernel` and skips the boot
  ROM/SPL. Anything the ROM programs (PLLs, some fuses, initial mux state) reads
  as reset-0 unless you seed it in the device `reset()`. `553cff93522` (SYS_PLL1)
  is the template. **Seed in reset(), not realize()** — realize seeds are wiped by
  the post-realize reset (`c559fca7465`).
- **Consumer-touched-fields rule.** A model only fails for the fields its consumers
  actually read. A "stub is fine" verdict is only valid for the consumers that
  existed when you checked — a NEW consumer (or the 952's different SM) can turn a
  fine stub into a silent-wrong. Re-audit stubs under new consumers.
- **Per-driver triage taxonomy** for a probe-fail: stub / promote-to-real / trim
  DT / disable / accept / extend-model-field — pick per driver (`feedback_path_c_taxonomy`).

### 4c. Verification discipline (non-negotiable)

- **Guest-visible before/after.** For any fidelity fix, capture the value the guest
  reads with the OLD binary and the NEW one, and diff. Techniques used on 95:
  - Clocks: `mount -t debugfs none /sys/kernel/debug; cat /sys/kernel/debug/clk/clk_summary`.
    The CCM + SYS_PLL1 fixes were proven this way (a55 24M→1.8G; 112→166 nonzero).
  - MAC/net: `/sys/class/net/eth0/address`, `.../statistics`.
  - To get the "before", `git stash` your change, rebuild, capture, `git stash pop`,
    rebuild. Yes, two rebuilds. Do it anyway.
- **Functional regression after a fidelity change.** A clock/PLL change that makes
  `clk_summary` prettier but breaks eth is a net loss. Re-run a real datapath test
  (e.g. the ENETC interconnect) after clock changes. (0-clocks turned out cosmetic
  because QEMU doesn't time by them — but I *checked*.)
- **qtest** for MMIO-level invariants (`tests/qtest/imx95-*.c`, `fsl-enetc-test.c`).
- **checkpatch every diff:** `./scripts/checkpatch.pl -q <(git diff <files>)`. The
  only allowed remaining warning is "Missing Signed-off-by" (git adds it) and the
  MAINTAINERS-new-file nag (consistent with existing imx95 files).
- **Instrument, don't guess.** Add a temporary `trace_`/`qemu_log` on the TX path
  before concluding "output missing." Per-event `qemu_log` throttles + lies about
  counts — use trace events with `trace_event_get_state_backends()` gating.

### 4d. Debugging reflexes (condensed from `feedback_debugging_habits`)

Spinning vCPU is invisible to the guest → measure host per-thread CPU.
`-d nochain` + `hbreak` for breakpoints in TCG. `info symbol` + fault-dump first.
`-icount shift=auto` for TCG races. The board-generated `configs/<board>/*.h` is
the authority for the SM build. The SM firmware has no gdb — instrument via
registers/MBOX. Batched-fault enumeration beats one-at-a-time. A NIC that
back-pressures the wire wedges a socket-netdev link (drop, don't block).

---

## 5. Working style & process rules (Kyle's rules — follow exactly)

- **Small, focused commits.** One fix per commit. Commit body explains the *why*
  and the before/after proof.
- **Commit prefix:** 95 uses `imx95:`. **For 952 use `imx952:`.**
- **NEVER add `Co-Authored-By` trailers.** (This overrides any default.)
- **No personal email in content/commits.** Use the `@kylefoxaustin` handle. Fork
  commits author as the GitHub no-reply address.
- **checkpatch gates every C change.** Don't commit with new warnings.
- **Ask before guessing on RM details** — but exhaust the SM source + headers
  first (they're usually the answer).
- **Model 952 as 952.** Don't homogenize to 95/93/91 just because they share code.
  The whole point of a separate 952 model is the delta. When in doubt whether a
  block behaves like 95, check the `MIMX952` SM source, not the `MIMX95` one.
- **Git remotes:** push only to the fork remote, never to the QEMU upstream gitlab.
  **Confirm with Kyle before pushing** (he gates pushes).
- **Per-milestone review doc** (`docs/reviews/v<X.Y.Z>.md`) + a README that matches
  the shared structure (§8).
- **Cross-session bus:** `~/.claude/bin/bus.sh check|send|all`. Single-quote
  messages (backticks execute). The tag derives from your cwd — post from the 952
  repo root so it tags `[952emulator]`. Announce big findings; the 95 fixes here
  are on the bus.

---

## 6. The 95 → 952 delta — how to find what's different

### 6.0 Known hardware deltas (from the NXP i.MX 952 fact sheet, IMX952FSA4 rev 1)

The 952 is an i.MX 9-series sibling of the 95, aimed at **automotive/industrial
vision + in-cabin sensing** (driver/occupant monitoring, child-presence, machine
vision) with a **flex/safety/real-time domain** split. Same three-domain identity
as 95 (A55 app domain + M7 real-time + M33 System Manager), but a **smaller,
cost-optimised** peripheral set. Confirm each against the 952 dtb/SM source — the
fact sheet is marketing-accurate, the dtb is authoritative — but expect:

| Block | i.MX 95 | i.MX 952 | Model action |
|---|---|---|---|
| **A55 cores** | 6 | **up to 4** (+512 KB coherent L3, ECC) | `max_cpus`, GIC redist count, PSCI, and **CCM per-core roots a55c0..3** (not ..5); re-check the M33 `cpu-num` mapping |
| **Ethernet** | 3-port NETC incl 10G | **2× Gbps (2.5G + 1G) w/TSN**, no 10G | fewer ENETC PFs; **drop the 10G Path-A port**; shrink `tests/netc/patch-dtb.py` per-port table |
| **PCIe** | 2× RC | **1× Gen3 x1** (1G SGMII mux) | one RC domain; **drop pcie1** (the 2nd domain-nr host) |
| **USB** | USB2 + USB3 (DWC3) | **2× USB 2.0 only** | **drop the USB3/DWC3 model**; keep 2× ChipIdea USB2 |
| **CAN** | 5× FlexCAN | **3× CAN FD** | fewer FlexCAN instances (`canbus0..2`) |
| **NPU** | eIQ Neutron | eIQ Neutron | same — keep honest-fault model |
| **GPU** | Mali (Valhall) | **Mali 3D + separate 2D GPU** | 2D GPU is in the real-time domain (safety overlays); check GPU_ID for the 952 part |
| **VPU** | Wave6 | **4K VPU** | brings-up tier; confirm compatible |
| **ISP** | NeoISP | **NXP ISP w/ RGB-IR** | RGB-IR is the in-cabin-sensing angle; camera path |
| **Display** | DPU + DSI + LVDS | 1× MIPI-DSI 4-lane (2880×1080p60), 2×4-lane or 1×8-lane **LVDS**, **local dimming**, 2 independent outputs | DPU/DSI/LVDS mostly transfer; **local dimming is new** (backlight/PWM path) |
| **Camera** | MIPI-CSI | 1× MIPI-CSI 4-lane or 2× 2-lane | ISI/CSI path transfers |
| **Memory** | LPDDR5/4X, eMMC | **32-bit LPDDR5/4X (inline ECC+enc), eMMC 5.1, 3× SD/SDIO 3.0, OSPI (inline crypto)** | uSDHC/eMMC transfer; **DRAM_PLL rate = the 952 EVK's LPDDR5 speed** (the P1 seed) |
| **Audio** | SAI/MICFIL/WM8962 | 15-lane I²S TDM, 2-lane I²S TDM, **8-ch PDM mic**, I²S out | transfers |
| **Serial** | 8× LPUART/LPI2C/LPSPI | UART ×(6+2), I²C ×(6+2), SPI ×(6+2) | similar counts; verify per dtb |
| **Other** | — | ADC 8-ch 12-bit, **FlexIO**, **XSPI responder**, secure JTAG, crypto accel | ADC transfers; **FlexIO + XSPI-responder are new blocks** to assess |
| **ELE / security** | EdgeLock | EdgeLock + **PQC**, tamper, secure clock/boot, eFuse, RNG | ELE model transfers; PQC is compute-out-of-scope |
| **Package/EVK** | 19×19 EVK | 19×19 0.7mm **and** 15×15 0.5mm; i.MX 952 EVK planned | machine name likely `imx952-19x19-evk` |
| **Safety** | — | ISO 26262 **ASIL B**, IEC 61508 **SIL 2** | informs the M33/M7 safety-domain framing, not the model directly |

**Net for the model:** 952 is largely a **subset** of 95 (fewer A55/ENETC/PCIe/USB/
CAN) plus a few **new blocks** (2D GPU, RGB-IR ISP, local dimming, FlexIO, XSPI
responder). Most 95 models transfer directly; the work is (a) **trim** the counts
to match the 952 dtb, (b) **re-seed** the clock/PLL values for the 952 config
(different core count → different a55 roots; 952 LPDDR5 → DRAM_PLL), and (c) stand
up the handful of **new blocks**. The silent-wrong hunt (§4a) finds the rest.

### 6.1 Discover the rest empirically — don't assume 952 == 95. Method:

1. **Diff the SM device layer.** `diff -qr imx-sm/devices/MIMX95 imx-sm/devices/MIMX952`
   and read every file that differs. The SM's `dev_sm_clock.c`, `fsl_clock.c`
   (PLL/root tables), `board.c`, and the memory-map / peripheral list are where
   the SoC identity lives. If 952 has a different clock-root count, PLL set, or
   power-domain list, your CCM/ANATOP seeds and counts must follow.
2. **Diff the device trees.** The 952 dtb vs the 95 dtb: different `reg` bases,
   different peripheral instances, different `compatible` strings → different
   drivers probe → different registers get touched → different silent-wrongs.
3. **Diff the board config header** (`configs/<952board>/*.h` in imx-sm) — it's the
   authority for what the SM builds with (which MUs, which peripherals, debug UART
   instance, etc.).
4. **Re-run the silent-wrong hunt (§4a) on 952.** This is the empirical delta
   finder: any register the 952 guest touches that 95's didn't will show up as a
   new "bad read." Chase those.
5. **Known likely deltas to check first** (verify, don't assume):
   - **PLL/clock rates** — if 952 uses a different SYS_PLL1 config or extra PLLs,
     the SYS_PLL1 seed values (§3 clocks) change. Re-derive from the 952 SM's
     `fsl_clock.c` rate tables, not 95's.
   - **Core count / cluster topology** — affects `max_cpus`, GIC, PSCI, and the
     per-core CCM roots (a55c0..N).
   - **Peripheral instance counts** (ENETC ports, LPUART/LPI2C/eDMA instances,
     CAN count) — the memmap + dtb tell you.
   - **DRAM controller / DDR PLL rate** — the one honest-0 PLL most likely to have
     a well-known 952 EVK value you can seed (see §7).

---

## 7. Prioritized 952 parity worklist

Do these in order. Each is "verify on 952, then fix if the delta broke it."

**P0 — ✅ DONE (by the 95 Claude session; see `docs/952-bringup-notes.md`).**
For the record, P0 was: run the hunt on a 952 boot; confirm A55 clocks read the
real 952 freq not 24 MHz (they read 1.704 GHz — the CCM/ANATOP fixes ported
cleanly); ENETC MAC not all-zero; no MU `0x18` flood; SCMI boots to userspace.
The one thing that blocked all of it was NOT a fidelity fix — it was an unmapped
952 TRDC (0x42080000 + new 0x424C0000) the SM writes at boot, causing an M33
BusFault → dead SCMI. Fixed in the 952 repo (commit `4d959ba`). **Start your work
at P1.**

**P1 — finish the clock tree (the 95 known-gap; likely tractable on 952):**
5. **DRAM_PLL** — seed the 952 EVK's DDR PLL rate (single well-known value per
   board; get it from the 952 board config / DDR init). Same reset-seed pattern as
   SYS_PLL1 (`553cff93522`).
6. **AUDIO/VIDEO/LDB PLLs** — seed *if* a consumer needs them (audio/display
   bring-up). Otherwise leave honest-0 and document. Don't hardcode a guessed rate.

**P2 — datapath parity (re-verify the COMPUTES-tier blocks on 952):**
7. ENETC eth interconnect (byte-exact, two 952 instances) — the `tests/interconnect`
   harness. Confirms MAC + RX + TX + clocks together.
8. SD/eMMC read+write, DPU scanout, camera frames, audio both directions, FlexCAN
   e2e — run each block's `tests/` harness on 952, fix deltas.

**P3 — honest-fault parity (the accelerators):**
9. Neutron/Mali/VPU — confirm they bind on 952 and **fault honestly** (no garbage).
   Mali needs `GPU_ID arch_minor=12`. Neutron needs the side-band error_code
   pattern. Re-check the 952 driver plumbs the same side-bands.

**P4 — uniformity + upstream-readiness:**
10. README to the shared structure (§8), per-milestone review docs, the full test
    matrix green, checkpatch-clean throughout. Then (and only then) discuss
    upstreaming with Kyle.

---

## 8. The verification suite to replicate

95 has a large `tests/` tree — mirror the shape on 952. The load-bearing ones:

- `tests/interconnect-imx95/run-eth.sh` + `run-uart.sh` — two-instance byte-exact
  I/O over ENETC + LPUART (directive #5). **The gold standard for "the I/O
  actually works."**
- `tests/netc/patch-dtb.py` — fixed-link + identity msi-map so ENETC probes.
- `tests/full-stack/run.sh` — one-boot coexistence gate (caught 2 bugs by running
  everything together; vermagic gotcha lives here).
- `tests/code-sweep/` — manifest-driven: builds + runs *real third-party code* on
  the A55 and checks integrity oracles (not just done-flags). 44/44 green on 95.
  This is your runtime-fidelity proof. `tests/in-guest-build/` self-hosts gcc+clang
  on the A55.
- `tests/qtest/imx95-*.c` — MMIO-level device invariants.
- `tests/soak/` — long-run stability.

README section order (match this on 952's README.md — directive #3):
`Quickstart · Scope (modelled/deferred) · What runs today · Roadmap · Required
artifacts · Known limitations · Architecture overview · Repository tour · Building
· Smoke tests · Methodology & contributing · Milestone history · License & credits`.
Fidelity taxonomy in the "What runs today" table: **functional / brings-up**
(+ honest-fault / stub). Hero shot after the Maintainer line.

---

## 9. Traps & gotchas (SoC-family specific — these bit me)

- **realize-seed wiped by reset** → seed in reset(). (`c559fca7465`)
- **Wrong register view:** the netdev MAC comes from SI-space `SIPMAR` (0x80), not
  PORT-space `PSIPMAR` (0x2000) — real HW mirrors them, the model doesn't. Seed both.
- **CCM/PLL:** the SM does RMW on `CONTROL.RW`; a stub returning 0 corrupts it.
  Register-back it. But **don't RAM-back a polled busy bit** → hang.
- **Boot ROM skipped** (`-kernel`) → PLLs/boot state read 0 unless seeded at reset.
- **cpuidle.off=1** required or SCMI-IRQ target CPU can't wake (GIC cpuif idle).
- **DMA-RX ttys:** PIO-only silently drops RX; needs the eDMA-RX `dma-req` line +
  `.valid.min_access_size=1` (eDMA reads DATA a byte at a time).
- **lpi2c is IRQ-driven** in Linux; a poll-only master leaves slaves at -110.
- **XCVR/fw-load MMIO** needs `max_access_size=8`. **dw-mipi-dsi** `CMD_PKT_STATUS`
  must return EMPTY bits (0x15) or it hangs.
- **Kernel modules must match the booted kernel's vermagic** — build them from the
  same tree you boot.
- **arm-smmu-v3** attaching to the ENETC bus corrupts MSI/ITS → regresses ENETC.
  Don't. Use per-device dtb iommu-strips.
- **Per-event `qemu_log` throttles and lies** about counts. Use trace events.

---

## 10. Appendix — key commits to `git show` (you have the full history)

The fidelity fixes whose *diffs* are the best templates:

| Commit | What it teaches |
|---|---|
| `c559fca7465` | ENETC SI MAC — reset-seed + wrong-register-view + two-bug diagnosis |
| `5a7396424a5` | MU core block — quad of RW/W1C/RO status registers, vmstate `_V` |
| `f7fe7d93317` | CCM clock-root register model — the RW/SET/CLR/TOG quad-alias template |
| `553cff93522` | SYS_PLL1 reset seed — boot-ROM state, authoritative-value sourcing |
| `b52bd756192` | test cleanup after a model fix — dropping a workaround the fix obsoletes |
| `0a9592f921d` | ENETC RX-overrun drop (don't back-pressure a socket netdev) |
| `110b0c2c866` | LPUART eDMA-RX (DMA-RX tty pattern) |
| `393df89d6b` | FlexCAN — a from-scratch upstream-candidate device, end-to-end |
| `0b80f75a6af` | DPU VBLANK (irqsteer + 60 Hz tick) |
| `194c3ea80a0` | Camera STREAMON/DQBUF (V4L2 link-validate oracle) |

Milestone tags for the big feature landings: `imx95-v2.3.0` (latest), `imx95-v2.0.0`
(NETC), `imx95-v1.x-step6` (M7), `imx95-code-sweep-v1.3` (third-party corpus),
`v0.9` (SCMI swap), `v0.6` (M33 SM). `git log --oneline <tag>` walks each.

---

### Closing

The base port boots; that's the platform. Parity is *fidelity* — and fidelity is
found, not assumed. Run the silent-wrong hunt (§4a), trust the SM source as your
oracle (§2), verify guest-visible before/after (§4c), and work the P0→P4 list
(§7). When you find a 952-specific silent-wrong, that's the job working as
intended — 95 had dozens, and each one is a developer who *won't* get burned by a
board that lies. Post your findings on the bus. Good hunting.

— the 95 Claude Code session
