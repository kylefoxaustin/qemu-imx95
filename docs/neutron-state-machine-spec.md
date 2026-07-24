# QEMU i.MX95 Neutron - Mailbox State Machine Specification

Author: Kyle Fox
Date: 2026-07-23
Status: Phase 1, item 1 - normative spec (the oracle)
Audience: ACC (implementer), concurrency-qtest author, future upstream reviewer
Applies to: `hw/misc/imx95_neutron.c` front end + `NeutronBackendOps` seam

---

## 0. Purpose and status

This document is normative. It defines the complete observable behavior of the
Neutron mailbox device as a finite state machine: the states, the events that
cause transitions, the guest-visible side effects of each transition, and the
handling of every edge case - most importantly the stale-completion race
`RUN -> RESET -> DONE(old job)`.

It exists so that:

1. The concurrency qtests have an oracle. Every test asserts a transition
   defined here. Without a written transition table, "expected behavior" becomes
   whatever the tests happened to check - the exact failure mode we agreed to
   avoid.
2. The upstream RFC has an answer for reset/migration. A maintainer will ask
   "what happens to an in-flight inference on reset / across migration?" This
   is that answer.
3. The stale-DONE case is settled in writing, not in vibes.

Rule of precedence: if code and this document disagree, one of them is a bug;
resolve it explicitly, do not let them drift. Changes to observable behavior
change this document in the same commit.

---

## 1. Scope - what is and isn't a state variable

The FSM is defined over the device's completion lifecycle, not over every
register. The state is deliberately small.

### 1.1 State variables (the authoritative tuple)

| Variable | Type | Meaning |
|---|---|---|
| `phase` | enum | The FSM state (Section 2) |
| `job_seq` | uint64 | Monotonic counter; incremented on every accepted RUN |
| `active_seq` | uint64 | The `job_seq` of the job whose completion is still valid (0 = none) |
| `pending_cmd` | uint32 | The command word latched at the doorbell |
| `MBOX0` | uint32 (reg) | The guest-visible ack/retcode register (also the primary FSM signal) |

`MBOX0` is both a hardware register and the FSM's most important observable.
Its legal values and their FSM meaning:

| `MBOX0` | Name | FSM meaning |
|---|---|---|
| `0x0` | `RESET_VAL` | idle / post-reset |
| `0xA3` | `RUN_ACK` | RUN received, compute in flight |
| `0xAD0` | `DONE` | terminal completion (success or clean engine failure - see Section 5) |

### 1.2 Explicitly NOT state variables

`MBOX1..7`, `BASEDDR*`, `BASEINOUT*`, `BASESPILL*`, `APPCTRL`, `APPSTATUS`,
`INTENA` - these are data/config registers. They are snapshotted into a job
(Section 3.2) but they do not define the FSM `phase`. The guest may legally
scribble them at any time; the FSM does not react to writes to them except
`APPCTRL` bit 2 (the doorbell) and `MBOX3` (the command word).

---

## 2. States

    IDLE -[RUN doorbell]-> ACKED (transient) -[submit]-> RUNNING
    RUNNING -[worker done-BH, valid]-> COMPLETE
    COMPLETE -[RESET doorbell]-> IDLE
    RUNNING -[RESET doorbell]-> IDLE  (worker disowned)

### 2.1 State definitions

| State | Invariant | `MBOX0` | Worker | `active_seq` |
|---|---|---|---|---|
| IDLE | No job in flight; ready to accept RUN | `0x0` | none | `0` |
| ACKED | RUN received, ack posted, job not yet handed to backend. Transient - collapses into RUNNING within the same doorbell handler. | `0xA3` | spawning | `= job_seq` |
| RUNNING | Backend/worker executing; completion pending | `0xA3` | live | `= job_seq` |
| COMPLETE | Terminal result posted (DONE + MBOX1 + IRQ); awaiting driver's RESET | `0xAD0` | done | `= job_seq` (the completed one) |

ACKED is transient by design. The synchronous `RUN_ACK` write and the job
hand-off to the backend happen in the same `neutron_doorbell()` call under the
BQL. ACKED exists in the model to name the instant between "ack posted" and
"worker spawned," because the two-phase completion contract (Section 4)
requires the ack to be observable before any slow work. In practice the guest
never observes a stable ACKED-that-isn't-RUNNING; treat them as one
externally-visible state (`MBOX0=0xA3`) with an internal ordering guarantee.

---

## 3. Events

### 3.1 Event list

| Event | Trigger | Thread |
|---|---|---|
| `EV_RUN` | Guest writes `RUN (0x269)` to `MBOX3`, then `APPCTRL |= bit2` | vCPU/BQL |
| `EV_KERNELS` | Guest writes `KERNELS (0x272)` + doorbell | vCPU/BQL |
| `EV_RESET` | Guest writes `RESET (0x23637)` to `MBOX3` (not doorbelled - handled in `neutron_dev_write`) | vCPU/BQL |
| `EV_OTHER` | `CLEAR_FW_LOG` / `GET_FW_LOGLEVEL` etc. + doorbell | vCPU/BQL |
| `EV_DONE` | Worker finished; done-BH runs on main loop | main loop/BQL |
| `EV_DEVRESET` | QEMU device reset (`DeviceReset` / machine reset) | main loop/BQL |
| `EV_MIGRATE_PRE` | Pre-migration save | main loop/BQL |

### 3.2 The job snapshot (taken on `EV_RUN`, before spawning)

On an accepted RUN the front end snapshots - under the BQL, before any worker
exists - a heap `NeutronJob`:

    job->seq              = ++s->job_seq;   /* and s->active_seq = job->seq */
    job->req.as           = &s->dma_as;
    job->req.base_ddr     = (H<<32)|L from BASEDDRH/L;
    job->req.tensor_offset    = MBOX4;
    job->req.microcode_offset = MBOX5;
    job->req.tensor_count     = MBOX6;
    job->req.kernel_offset    = s->kernel_offset;   /* latched by prior KERNELS */

Everything the worker needs is in the job. The worker never re-reads a
register. This is what makes register scribbles during RUNNING harmless.

---

## 4. Transition table (the normative core)

Notation: `state x event -> new state [side effects]`. All register writes and
IRQ raises happen on the BQL/main loop; the worker touches only `dma_*`.

| Current | Event | Guard | Next | Side effects |
|---|---|---|---|---|
| IDLE | `EV_RUN` | `MBOX0 == RESET_VAL` | RUNNING | `job_seq++`; `active_seq=job_seq`; snapshot job; `MBOX0=RUN_ACK`; spawn worker |
| IDLE | `EV_KERNELS` | - | IDLE | latch `kernel_offset=MBOX4`; `MBOX0=DONE`; APPSTATUS(INFDONE\|MBOX); IRQ |
| IDLE | `EV_OTHER` | - | IDLE | `MBOX0=DONE`; APPSTATUS; IRQ |
| IDLE | `EV_RESET` | - | IDLE | `MBOX0=RESET_VAL` (idempotent) |
| RUNNING | `EV_DONE` | `job.seq == active_seq` AND phase==RUNNING | COMPLETE | `MBOX0=res.retcode(DONE)`; `MBOX1=res.error_code`; APPSTATUS(INFDONE\|MBOX); IRQ; free job |
| RUNNING | `EV_DONE` | `job.seq != active_seq` OR phase!=RUNNING | (unchanged) | DROP (stale): free job, no reg write, no IRQ (Section 6) |
| RUNNING | `EV_RUN` | - | RUNNING | BUSY: `MBOX0=DONE`, `MBOX1=NEUTRON_ERR_BUSY(0x95E6)`, APPSTATUS, IRQ; do not spawn; do not touch active_seq (Section 7) |
| RUNNING | `EV_RESET` | - | IDLE | `active_seq=0`; cancel/disown worker (Section 6.2); `MBOX0=RESET_VAL` |
| RUNNING | `EV_KERNELS`/`EV_OTHER` | - | RUNNING | see Section 7.2 - treated as BUSY (reject) while a job is live |
| COMPLETE | `EV_RESET` | - | IDLE | `active_seq=0`; `MBOX0=RESET_VAL`; ready for next RUN |
| COMPLETE | `EV_RUN` | `MBOX0 != RESET_VAL` | COMPLETE | stuck-guard reject: guest is expected to RESET first; treat as BUSY (Section 7.3) |
| any | `EV_DEVRESET` | - | IDLE | full reset (Section 8): `active_seq=0`; disown worker; all MBOX->0; drop IRQ |
| any | `EV_MIGRATE_PRE` | - | (same) | migration is blocked unless IDLE/COMPLETE (Section 9) |

The two `RUNNING x EV_DONE` rows are the heart of this document. The guard
`job.seq == active_seq` is the stale-completion filter. Everything else is
bookkeeping around it.

---

## 5. Completion semantics - DONE always, error in MBOX1

Per the earlier review decision, all terminal completions post
`MBOX0 = DONE (0xAD0)`; success vs. failure is communicated in `MBOX1`:

| Outcome | `MBOX0` | `MBOX1` |
|---|---|---|
| Success | `DONE` | `0` |
| Engine failure (unsupported op, size mismatch, DMA fault, etc.) | `DONE` | non-zero errcode |
| Not computed (null backend) | `DONE` | `uncomputed_errcode` (default `0x95E0`) |
| Busy (RUN while RUNNING/COMPLETE) | `DONE` | `NEUTRON_ERR_BUSY (0x95E6)` |

Rationale: the guest driver's completion wait keys on `MBOX0 == DONE`. Using a
non-DONE retcode for engine-level failures risks a driver timeout that looks
like an infrastructure bug when the real cause is "unsupported op." Reserve
non-DONE retcodes for mailbox/protocol faults the driver is explicitly known
to handle. The error-code family:

| Code | Name | Meaning |
|---|---|---|
| `0x95E0` | `UNCOMPUTED` | null backend; nothing computed |
| `0x95E1` | `FIXTURE_MISS` | (Track A) no fixture matched |
| `0x95E2` | `EXEC_FAILED` | (Track A) runner launched but failed |
| `0x95E3` | `TIMEOUT` | backend exceeded deadline |
| `0x95E4` | `SIZE_MISMATCH` | output size disagreed with descriptor |
| `0x95E5` | `DMA_FAILED` | `dma_memory_read/write` fault |
| `0x95E6` | `BUSY` | RUN arrived while a job was active |

---

## 6. The stale-completion contract (RUN -> RESET -> DONE(old job))

This is the case the whole document exists to pin down.

### 6.1 The race

1. `EV_RUN` accepted: `job_seq=1`, `active_seq=1`, phase=RUNNING, worker A
   live.
2. Guest issues `EV_RESET` (e.g. driver decided the NPU is wedged, or is
   tearing down). Front end sets `active_seq=0`, phase=IDLE,
   `MBOX0=RESET_VAL`.
3. Worker A - which was mid-compute and cannot be instantly killed - finishes
   and schedules its done-BH.
4. `EV_DONE` for job A runs on the main loop.

### 6.2 The contract

A worker's completion is valid iff, when its done-BH runs, `job.seq ==
active_seq` and the current phase is RUNNING. Otherwise it is stale and MUST
be dropped:

    static void neutron_done_bh(void *opaque) {
        NeutronJob *job = opaque;
        IMX95NeutronState *s = job->dev;

        if (s->phase != PHASE_RUNNING || job->seq != s->active_seq) {
            trace_imx95_neutron_stale_done(job->seq, s->active_seq);
            neutron_job_free(job);   /* silently drop: no MBOX write, no IRQ */
            return;
        }
        /* valid completion */
        ndev_w(s, N_MBOX0, job->res.retcode);
        ndev_w(s, N_MBOX1, job->res.error_code);
        ndev_w(s, N_APPSTATUS, ... | INFDONE | MBOX);
        s->phase = PHASE_COMPLETE;
        neutron_raise_irq(s);
        neutron_job_free(job);
    }

Key properties:

- Disown, don't kill. We do not attempt to cancel worker A's computation (you
  cannot safely interrupt a detached thread mid-`dma_*`). We let it finish and
  discard its result. `active_seq=0` (or a newer seq) is what invalidates it.
- A stale DONE writes nothing guest-visible. No `MBOX0`, no `MBOX1`, no
  `APPSTATUS`, no IRQ. The guest, having reset, sees only `MBOX0=RESET_VAL`.
- Monotonic `job_seq` also covers RUN->RESET->RUN. If a new RUN (job 2) was
  accepted after the reset, `active_seq=2`; worker A's done-BH (seq 1) still
  fails the guard and is dropped, and worker B completes normally.
- The trace point `imx95_neutron_stale_done` is mandatory, so a dropped
  completion is observable in a trace log rather than a silent mystery.

### 6.3 Worker-side rule

The worker MUST NOT touch any FSM/register state. It computes into the job's
own result struct and schedules the done-BH. All validity checking happens on
the main loop in the done-BH. This is why disown-and-drop is safe: the worker
has no way to post a result except through the guarded BH.

---

## 7. Concurrency / BUSY semantics

### 7.1 Chosen policy: queue depth 1, reject with BUSY

The device accepts one in-flight job. A RUN arriving while RUNNING (or
COMPLETE-awaiting-RESET) is rejected, not queued:

- Post `MBOX0=DONE`, `MBOX1=NEUTRON_ERR_BUSY (0x95E6)`, APPSTATUS, IRQ.
- Do not snapshot a job, do not spawn a worker, do not change `active_seq`
  (the live job keeps its claim).

Rationale (locked per earlier decision): simpler state machine, easier
validation, closer to apparent hardware behavior, easier failure analysis.
The driver owns serialization; the device just makes a violation visible
rather than silently corrupting.

### 7.2 KERNELS/OTHER while RUNNING

Also rejected as BUSY while a job is live. Latching a new `kernel_offset`
mid-inference would race the worker's snapshot semantics. The driver is not
expected to do this; if it does, it gets a visible BUSY.

### 7.3 RUN while COMPLETE (stuck-guard)

The driver is expected to issue RESET after seeing DONE before the next RUN.
A RUN while `MBOX0 != RESET_VAL` (i.e. still COMPLETE) is rejected as BUSY.
This mirrors the driver's own stuck-detection: it reads `MBOX0` at job start
and, if it is not `RESET_VAL`, concludes the NPU is wedged and does a HW
reset + firmware reload. So the honest device response to "RUN before RESET"
is BUSY, prompting exactly that recovery.

---

## 8. Device reset (EV_DEVRESET) - the hard reset

Distinct from the guest's mailbox `RESET` command: this is QEMU `device_reset`
(machine reset, `system_reset`, cold start). It is the strongest transition:

From any state -> IDLE, with:

- `active_seq = 0`, `job_seq` retained (monotonic across soft resets; reset
  to 0 only on cold `DeviceReset` is acceptable - see migration note
  Section 9).
- Any live worker is disowned (its future done-BH will fail the Section 6
  guard).
- All `MBOX0..7 = 0`; `APPSTATUS`, `INTENA` cleared; pending IRQ dropped
  (`qemu_set_irq(irq, 0)`).
- `kernel_offset` invalidated (first-implementation choice: safest; a cached
  kernel blob does not survive a hard reset).
- `pending_cmd = 0`; `phase = IDLE`.

The mailbox `RESET` command (`EV_RESET`) is the soft version: it returns the
FSM to IDLE and re-arms `MBOX0=0` but does not necessarily clear config
registers. Both must disown in-flight workers via `active_seq`.

---

## 9. Migration (VMState) - the maintainer's question

A converted-model worker can be mid-flight; migration must not capture or
resume a half-done inference incoherently. Contract:

### 9.1 Migratable state

The VMState description serializes the FSM tuple: `phase`, `job_seq`,
`active_seq`, `pending_cmd`, `kernel_offset`, and the register file. It does
NOT serialize a live worker thread or an in-flight `NeutronJob` (heap,
thread-owned).

### 9.2 The rule: no migration with a live worker

Migration is only coherent in IDLE or COMPLETE (no worker running). If a save
is attempted while RUNNING:

- Preferred: register a `migrate` blocker while `phase == RUNNING`
  (`migrate_add_blocker`) and remove it on completion/reset. This gives a
  clean "cannot migrate: Neutron inference in flight" error rather than a
  corrupt transfer.
- Rationale: an inference is short (ms). Blocking migration for its duration
  is acceptable and vastly simpler than trying to serialize/replay a
  detached worker's partial state.

### 9.3 Post-load

On the destination, `phase` loads as IDLE or COMPLETE only (by the blocker
above). If COMPLETE, `MBOX0=DONE` and the pending completion is already in
the register file - the guest's next RESET proceeds normally. No worker is
resurrected. `active_seq` loads consistent with `phase`.

This section is the direct answer to the RFC's Open Question on
reset/migration. Summary for the cover letter: "An in-flight inference blocks
migration (a short, ms-scale blocker); the device migrates only when IDLE or
COMPLETE; no worker thread or partial job crosses the wire."

---

## 10. Tracepoints (required)

Every transition that matters is observable:

    imx95_neutron_run(uint64_t base, uint32_t toff, uint32_t mcoff, uint32_t cnt, uint32_t koff)
    imx95_neutron_run_ack(uint64_t job_seq)
    imx95_neutron_kernels(uint32_t kernel_offset)
    imx95_neutron_done(uint64_t job_seq, uint32_t retcode, uint32_t error_code)
    imx95_neutron_stale_done(uint64_t job_seq, uint64_t active_seq)   /* Section 6 */
    imx95_neutron_busy(uint64_t active_seq, uint32_t cmd)             /* Section 7 */
    imx95_neutron_reset(const char *kind)                             /* "soft" | "device" */
    imx95_neutron_dma_error(uint64_t addr, uint64_t len, int is_write)
    imx95_neutron_migrate_blocked(uint64_t active_seq)                /* Section 9 */

---

## 11. Test matrix - every row is an assertion against Section 4 / 6 / 7

The concurrency qtests (Phase 1) map 1:1 onto these. Each is gated for the
null backend (no proprietary deps) unless noted.

| # | Sequence | Expected (per this spec) |
|---|---|---|
| T1 | RUN -> (wait) -> DONE | `MBOX0`: 0->A3->DONE; MBOX1=uncomputed; one IRQ; phase COMPLETE |
| T2 | RUN -> RUN | 1st->A3; 2nd->BUSY(0x95E6); only one worker; active_seq unchanged |
| T3 | RUN -> RUN -> RUN | 1st runs; 2nd+3rd BUSY; no corruption |
| T4 | RUN -> RESET -> (worker completes) | RESET->IDLE(MBOX0=0); stale DONE dropped (trace fires); no IRQ from job 1 |
| T5 | RUN -> RESET -> RUN -> DONE | job1 stale-dropped; job2 completes; active_seq=2 throughout job2 |
| T6 | RUN -> DONE -> RUN (no RESET) | 2nd RUN -> BUSY (stuck-guard, Section 7.3) |
| T7 | RUN -> DONE -> RESET -> RUN -> DONE | clean back-to-back; two IRQs; no leak |
| T8 | RUN -> backend TIMEOUT -> RESET -> RUN | timeout->MBOX1=0x95E3 (or dropped if reset first); recovery RUN works; no orphan worker |
| T9 | RUN -> device_reset (EV_DEVRESET) mid-flight | all MBOX->0; IRQ dropped; stale DONE dropped; phase IDLE |
| T10 | RUN -> attempt migrate | migrate blocked (trace `migrate_blocked`); after DONE+RESET, migrate succeeds |
| T11 | KERNELS -> RUN -> DONE | kernel_offset latched into job1 snapshot; normal completion |
| T12 | RUN -> KERNELS (mid-flight) | KERNELS -> BUSY (Section 7.2); worker unaffected |

Validation goals across the matrix: no stale DONE, no `MBOX0` corruption, no
leaked scratch/heap, no orphaned worker, no IRQ storm, no mailbox protocol
violation.

---

## 12. Invariants (assert these in code where cheap)

1. `phase == RUNNING` iff exactly one worker is live iff `active_seq ==
   job_seq` of that worker.
2. `phase == IDLE` implies `active_seq == 0` and `MBOX0 == RESET_VAL`.
3. `phase == COMPLETE` implies `MBOX0 == DONE` and no worker live.
4. A guest-visible completion (MBOX0=DONE + IRQ from a job) happens exactly
   once per accepted RUN, unless that job was invalidated by RESET/device
   reset, in which case it happens zero times.
5. `job_seq` is strictly monotonic within a power cycle; `active_seq` is in
   `{0, job_seq}` - never a stale intermediate.
6. The worker thread never writes a register, raises an IRQ, or reads the
   FSM; it only computes and schedules the done-BH.

---

## 13. TL;DR

- States: IDLE -> (RUN) -> RUNNING -> (worker) -> COMPLETE -> (RESET) ->
  IDLE. ACKED is a transient internal instant inside the RUN handler.
- Stale completion (`RUN->RESET->DONE(old)`): the worker is disowned, not
  killed; its done-BH fails the `job.seq == active_seq` guard and is dropped
  with a tracepoint and zero guest-visible effect.
- Concurrency: queue depth 1; a second RUN is rejected as BUSY (0x95E6), not
  queued.
- Completion: always `MBOX0=DONE`; success/failure in `MBOX1`.
- Device reset: from any state -> IDLE, disown worker, all regs cleared, IRQ
  dropped.
- Migration: blocked while RUNNING (short ms blocker); migrates only in
  IDLE/COMPLETE; no worker or partial job crosses the wire. <- the RFC
  answer.
- Test matrix (Section 11) maps 1:1 to the transition table - that is the
  whole point: the qtests assert this document, not their own assumptions.
