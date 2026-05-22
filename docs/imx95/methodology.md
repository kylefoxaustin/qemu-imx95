# qemu-imx95 — Methodology

How this project is built and debugged. The techniques below recur across the
milestones; they are written down so future contributors inherit them rather
than rediscover them.

---

## The working mode: measure-first, hypothesis-test

Development runs as a **hypothesis-test loop, not order-execute.** A direction
is proposed (often by a reviewer), then **verified against data before any code
is committed.** When measurement contradicts the hypothesis, work pauses and
the contradiction is surfaced, and the plan is rebuilt from measured ground
truth.

This is not ceremony — it repeatedly caught wrong assumptions before they
became wrong code. Across the v0.x → v1.0 arc it reframed the next step at
least six times (e.g. "the slow boot is I2C timeouts" → measured: it's icount;
"model the GIC for cpuidle" → measured: first the broadcast timer, then a
distinct GIC-core gap). The estimated saving is tens of hours of speculative
work. A proposed direction is a starting hypothesis, not an instruction; the
ground-truth check is mandatory.

In practice this looks like: a hypothesis is proposed (often by a reviewer
reading documentation or pattern-matching against prior milestones); the
implementer verifies it against measurement (code reading, traces, register
dumps, reproducible repros); when measurement contradicts the hypothesis, the
implementer pauses and surfaces the contradiction rather than committing
speculative work; the reviewer then refines the hypothesis from measured ground
truth. Future contributors should expect to operate inside this loop —
proposals are starting points to test, not orders to execute.

A corollary for hierarchical bugs: **name the layer you've verified, don't
claim the absolute root.** A fix at layer N can reveal layer N+1 (the cpuidle
work: broadcast-timer layer, then GIC-WakeRequest layer — both real, both
necessary). State "at layer N the cause is X; a deeper layer may exist."

The discipline applies beyond code. Tag names, build configurations,
documentation conventions, and other operational decisions in a fork-based
project all carry implicit hypotheses inherited from the parent — verify before
acting. (Bare-name tagging worked through v0.10 by luck; the v1.0 milestone
surfaced a collision with QEMU's own `v1.0` release tag, hence the project's
`imx95-vX.Y` tag namespace.)

**Symptoms can share roots.** When multiple failures surface in different
subsystems, check for a shared upstream cause before treating them as
independent — the "name the layer" discipline applies to symptoms too. The
LPUART FIFO bug (`741af2f5e3`) presented as both a dead interactive console and
an SCMI desync (`Message ... not expected`); they looked unrelated, but a
single latched read-only status bit caused a 75 kHz RX interrupt storm that
both blocked RX delivery and starved the MU2 SCMI interrupt. One fix retired
both.

---

## The five debugging pillars

**1. `-d nochain` + `hbreak` when TCG breakpoints misbehave (v0.2).**
If a gdb software breakpoint fires at some addresses but not others in the same
session, stop investigating the mystery — switch to `qemu ... -d nochain` (no
TB chaining) and convert breakpoints to `hbreak`. QEMU chains translated
blocks; software BPs depend on the right TB being invalidated, and chained
execution can bypass them. A documented gotcha, not a reasoning error.

**2. Fault-dump-first + DTS-grep + `-d unimp` (v0.3).**
For an unmapped-access fault, read the fault dump (PC, address), `addr2line`
the PC against the relevant ELF, and map the faulting address to a DT node by
its `reg`. Don't reason about the peripheral before you know which one it is.
For firmware, `info symbol` in gdb (when attached) beats grepping the BSP — the
NXP tree has parallel variants of many functions, and grep finds the wrong one.

**3. `-icount shift=auto` for races/heisenbugs — with its inverse caveat (v0.5 / v1.0).**
*Use when:* debugging a race, a timing-sensitive heisenbug, or an
interrupt-delivery flake. Under icount, virtual time is driven by instruction
count, not wall-clock, so adding `--trace`/`fprintf` doesn't perturb the
schedule. This made the v0.5 SCMI/cpuidle hang a deterministic, traceable repro.
*Do NOT use when:* steady-state boot tests or performance measurement on a
heterogeneous multi-CPU machine. When both the A55s and the M33 can be in WFI
between SCMI/MU round-trips, icount delays interrupt/timer delivery to idle
CPUs, so `wait_for_completion_timeout()` waits run their full virtual-time
duration — inflating the boot ~9× (114 s vs 13 s). **Same tool, opposite
effect, depending on the architecture underneath.** Enable icount to debug a
race; remove it when done; never carry it in a test script reused for
non-race purposes.
The lesson generalizes beyond icount: **tools have contexts.** A technique that
resolved a problem at one architectural layer can introduce a different problem
at another. When inheriting a tool from a prior milestone ("we used X before"),
verify it still serves the current problem — don't carry it forward by default.

**4. Batched-fault enumeration (M33_ENUM) — M33-only (v0.7).**
When a CPU grinds through many unmapped accesses during a new bring-up phase,
don't fix-one-rebuild-rerun. Temporarily patch
`arm_cpu_do_transaction_failed` (gated by an env var) to log the faulting
access and return without delivering the fault, so one run enumerates the whole
unmapped footprint. **Apply this on the M33/SM only.** The SM firmware was
designed to degrade gracefully against missing hardware; Linux was not — on the
A55, returning 0 for real reads makes Linux go haywire (cleared flags, empty
FIFOs, ready bits are all meaningful values). For A55 faults, walk individually
(Pillar 2). Always remove the probe before committing — debug machinery in core
rots the tree.

**5. HMP-monitor frame-pointer unwinding for A55 hangs (v0.10).**
When a Linux probe/initcall *hangs* (no fault to dump) and the stalled kworker's
kernel stack is at a non-deterministic address across runs, drive the QEMU HMP
monitor over a unix socket: read the live frame pointer (`info registers`),
dump that exact stack in the same run (`x /512gx <FP & ~0xfff>`), walk the
AArch64 frame chain offline (`[fp]` = caller fp, `[fp+8]` = saved LR), and
`addr2line` each return address. Sample a few times (hangs can be racy). This
is automated as `scripts/probe_stall.py`; it pinned the v0.10 DPU hang
(`dpu95_be_read`) and the cpuidle wedge (the PSCI-suspend chain).

---

## Register-class triage (modelling a peripheral "for real")

A peripheral register has up to three bit classes; a plain RAM stub or a
zero-return stub each gets some wrong. Triage each field from the *driver
source*:

- **Config bits** — write persists, read returns last write. RAM is correct.
- **Command bits** — write triggers an action, hardware self-clears when done.
  Must read back 0, or a "wait for done" poll spins forever. RAM is *wrong*.
- **Status bits** — read reflects modelled state. For a poll-for-ready path,
  hardcode "ready" (instant) or mirror the paired control register.

Companion rules: scope a model by what its consumer *does* with a register
(write-then-read-back → model it; write-only → a logging stub suffices;
read-only → return the right canned value); start PLLs at instant-lock; default
revision/ID registers to the value that makes the driver take the simplest
path. The pattern scaled from XCACHE (v0.7) through the SRC/ANATOP/GPC models
(v0.8–v0.9) to the system-counter clockevent timer (v1.0). NXP IP blocks
commonly expose each register as a RW/SET/CLR/TOG quad of aliased addresses;
model the logical value at the base slot and apply `|=`/`&=~`/`^=` for the
aliases (see `hw/misc/imx95_anatop.c`).

---

## The Path-C taxonomy (handling a peripheral a consumer touches)

When a driver probe fails on an unmodelled peripheral, pick per-driver:

1. **Stub** it (logging device that absorbs accesses) — enough for blind
   writes.
2. **Promote a stub to a real model** — when a consumer reads back what it
   wrote or polls a status.
3. **Trim the DT** — only for hardware we deliberately don't populate; never to
   hide a device that the board has.
4. **Disable in config** — when the driver shouldn't run at all.
5. **Accept the soft failure** — a bounded probe timeout/`-ENODEV` is an honest
   "unmodelled device" outcome, not a bug.
6. **Extend an existing model** — add or fix the specific field a new consumer
   path reads.

A device model only fails for register fields its consumers *actually* read, so
an existing model stays suspect for a new consumer path until checked.

---

## Authority: BSP over guesswork, generated config over prose

Addresses and IRQ numbers come from the NXP BSP — the Linux DTS for peripherals
the kernel sees, the U-Boot RM-derived header for SCMI-routed peripherals SPL
pokes before SCMI is up — never guessed. The RM is authoritative for register
behaviour. For config-driven SM behaviour (CRC on/off, MU pairing, LM
resources), the board's generated `configs/<board>/*.h` is authority over
prose docs, which describe the general case.
