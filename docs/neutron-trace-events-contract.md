# i.MX95 Neutron -- `trace-events` Contract

Status: Phase 1 artifact -- the tracepoint contract the state-machine spec and
review comments reference
Source: reviewer message 2026-07-23 (captured verbatim; see
`neutron-phase1-reviewer-response.md`)
Applies to: `hw/misc/imx95_neutron.c` (front end) + backends
Tracks: **B (front-end set -- upstreamable)** and **A (runner set -- downstream)**

---

## 0. The one rule that governs this

The state-machine spec names tracepoints as load-bearing evidence:
`trace_neutron_stale_done` is how qtest T4 proves a stale completion was
dropped (no MBOX write, no IRQ). Reviews repeatedly asked for tracepoints
instead of stray `fprintf` noise. This file pins the exact set, their
arguments, and which track each belongs to.

> **Load-bearing rule (same boundary as everywhere else):**
> Tracepoints defined in `hw/misc/trace-events` and emitted from
> `imx95_neutron.c` are **Track B** -- upstream, hardware/protocol facts only,
> zero Track A vocabulary (no "subprocess", "fixture", "scratch",
> "runner-path", etc.).
> Tracepoints emitted from `imx95_neutron_backend_runner.c` are **Track A** --
> live in the same `trace-events` file (QEMU has one per directory), named
> with `_runner_` infix, only *fire* in a runner build. In a null-only build
> the runner TU isn't linked, so they never emit. The hygiene grep must
> forbid Track A trace names from `imx95_neutron.c`.

Same discipline as Phase 0.5 QOM properties now applies to traces.

---

## 1. QEMU tracepoint mechanics

- Declared in per-directory `trace-events`; generates `trace_<name>()` into
  `trace.h`.
- Printf-style format args (QEMU tracetool syntax).
- Emit `trace_<name>(args...)` from C. Runtime: `-trace enable=neutron_*`
  glob, or `-trace events=<file>`, HMP `trace-event`. Zero cost when disabled.
- **Naming:** `neutron_` (front end) and `neutron_runner_` (runner backend).

---

## 2. Track B -- front-end tracepoints (`imx95_neutron.c`)

Guest-visible hardware/protocol events only. 1:1 with state-machine transitions
so a trace log reads as a transition trace.

### 2.1 Definitions (add to `hw/misc/trace-events`)

```
# i.MX95 Neutron front end (Track B, upstreamable)
# Guest-visible mailbox/protocol events only. No backend-internal vocabulary.

neutron_doorbell(uint32_t cmd) "cmd=0x%x"

neutron_run_accept(uint64_t seq, uint64_t base_ddr, uint32_t tensor_off, uint32_t microcode_off, uint32_t tensor_count) "seq=%" PRIu64 " base=0x%" PRIx64 " tensor_off=0x%x mc_off=0x%x count=%u"

neutron_run_busy(uint64_t active_seq, int state) "active_seq=%" PRIu64 " state=%d"

neutron_done(uint64_t seq, uint32_t retcode, uint32_t error_code) "seq=%" PRIu64 " retcode=0x%x error_code=0x%x"

# STALE completion DROPPED -- the case qtest T4 asserts on.
neutron_stale_done(uint64_t job_seq, uint64_t active_seq) "job_seq=%" PRIu64 " active_seq=%" PRIu64

neutron_reset(uint64_t prev_active_seq, int prev_state) "prev_active_seq=%" PRIu64 " prev_state=%d"

neutron_kernels_latch(uint64_t base_ddr, uint32_t kernel_off) "base=0x%" PRIx64 " kernel_off=0x%x"

neutron_irq(int level) "level=%d"

neutron_fw_up(uint32_t appctrl) "appctrl=0x%x"

neutron_dma_error(uint64_t addr, uint64_t len, int is_write) "addr=0x%" PRIx64 " len=%" PRIu64 " is_write=%d"

neutron_state(int from, int to, uint32_t cause_cmd) "from=%d to=%d cause=0x%x"
```

### 2.2 Where each fires

| Tracepoint | Transition(s) | Fires when |
|---|---|---|
| `neutron_doorbell` | (all dispatch) | any APPCTRL bit2 write, before decode |
| `neutron_run_accept` | T1 | RUN accepted in IDLE; RUN_ACK posted |
| `neutron_run_busy` | T7, T8, T11 | RUN/KERNELS arrives while `job_active` |
| `neutron_done` | T3 | live completion published |
| `neutron_stale_done` | **T4** | finished job dropped because seq stale |
| `neutron_reset` | T5, T6 | RESET handled from any state |
| `neutron_kernels_latch` | T9 | KERNELS latched in IDLE |
| `neutron_irq` | (T3, T9, T10) | SPI 318 raised/lowered |
| `neutron_fw_up` | (handshake) | ZENV_CLK_ON -> APPCTRL[31:16]=0xF807 |
| `neutron_dma_error` | (failure) | completion path hits dma_* fault |
| `neutron_state` | (all) | any state variable change |

### 2.3 Trace-log invariant (machine-checkable)

For any single job lifecycle, the trace stream is exactly one of:

```
neutron_run_accept(seq=N ...) ... neutron_done(seq=N ...) neutron_irq(level=1)
```

or, if RESET intervened during RUNNING:

```
neutron_run_accept(seq=N ...) ... neutron_reset(...) ... neutron_stale_done(job_seq=N, active_seq=0)
```

**Never both a `neutron_done` and a `neutron_stale_done` for the same seq,
and never a `neutron_done` after a `neutron_reset` that invalidated that
seq.** That sentence is the machine-checkable form of the whole
stale-completion safety property.

---

## 3. Track A -- runner-backend tracepoints (`imx95_neutron_backend_runner.c`)

Same `hw/misc/trace-events` file, but `neutron_runner_*` names; emit only in
runner build. Purpose: collapse the five-layer `EXEC_FAILED (0x95E2)`
ambiguity.

### 3.1 Definitions

```
# i.MX95 Neutron runner backend (Track A, DOWNSTREAM ONLY)
# Only fire when imx95_neutron_backend_runner.c is linked. MUST NOT be
# emitted from imx95_neutron.c.

neutron_runner_manifest_lookup(const char *key, int hit) "key=%s hit=%d"

neutron_runner_spawn(uint64_t seq, const char *argv0, const char *scratch_dir) "seq=%" PRIu64 " argv0=%s scratch=%s"

neutron_runner_exit(uint64_t seq, int code, int signalled) "seq=%" PRIu64 " code=%d signalled=%d"

neutron_runner_timeout(uint64_t seq, uint32_t timeout_ms) "seq=%" PRIu64 " timeout_ms=%u"

neutron_runner_output(uint64_t seq, uint64_t bytes, uint64_t expected) "seq=%" PRIu64 " bytes=%" PRIu64 " expected=%" PRIu64

neutron_runner_result(uint64_t seq, uint32_t error_code) "seq=%" PRIu64 " error_code=0x%x"

neutron_runner_cancel(uint64_t seq, int had_child) "seq=%" PRIu64 " had_child=%d"

neutron_runner_stderr(uint64_t seq, const char *line) "seq=%" PRIu64 " %s"
```

### 3.2 EXEC_FAILED triage story

Before: `MBOX1=0x95E2` and five candidate causes. After
`-trace enable=neutron_runner_*`:

| Symptom in trace | Diagnosis |
|---|---|
| `manifest_lookup(hit=0)` | fixture miss (0x95E1) -- wrong key |
| `spawn` then no `exit` before `timeout` | runner hung -> 0x95E3 |
| `exit(code!=0, signalled=0)` + `stderr` lines | runner ran but failed -> 0x95E2; stderr line is the actual cause |
| `exit(signalled=1)` after `cancel` | we killed it on RESET -- expected |
| `output(bytes!=expected)` | size mismatch -> 0x95E4 |
| `spawn` never fires | argv/env construction failed before exec |

The `stderr` capture line is the single highest-value tracepoint for the
`EXEC_FAILED` debugging cycle.

---

## 4. Hygiene: extend the front-end grep

Phase 0.5's `check-frontend-hygiene.sh` forbids Track A tokens in
`imx95_neutron.c`. Extend it so runner trace *names* are also forbidden in
the front end:

```sh
# Track A trace names must not be EMITTED from the front end.
if grep -nE 'trace_neutron_runner_[a-z_]+' "$FRONTEND"; then
    echo "FAIL: front end emits a Track A runner tracepoint" >&2
    exit 1
fi
```

Positive check (optional but recommended): assert the front end *does* emit
the safety-critical tracepoint that qtest T4 depends on:

```sh
grep -q 'trace_neutron_stale_done(' "$FRONTEND" || {
    echo "FAIL: front end no longer emits neutron_stale_done (qtest T4 relies on it)" >&2
    exit 1
}
```

---

## 5. Wiring the trace assertion into qtest T4

Stronger than only checking MBOX0 didn't change:

- Run QEMU with `-trace enable=neutron_*` writing to a temp file, or use qtest
  trace hooks if available.
- After the sequence, assert the log contains
  `neutron_stale_done(job_seq=N, active_seq=0)` and does NOT contain
  `neutron_done(seq=N ...)`.
- Directly encodes the section 2.3 invariant.

Pure-MMIO fallback: after RESET, MBOX0 reads `RESET_VAL` and stays; zero
additional SPI 318 asserts occur.

---

## 6. TL;DR

- Two families: `neutron_*` (Track B, upstream-clean) and `neutron_runner_*`
  (Track A, downstream, never emitted from front end).
- Front-end set maps 1:1 to the state machine; `neutron_stale_done` is
  safety-critical; qtest T4 asserts on it.
- Log invariant (never both `done` and `stale_done` for one seq; never `done`
  after a reset that invalidated that seq) is the machine-checkable form of
  stale-completion safety.
- Runner set collapses the EXEC_FAILED five-way ambiguity; `stderr` capture
  is the single highest-value tracepoint.
- Hygiene grep now forbids `trace_neutron_runner_*` in the front end and
  requires `trace_neutron_stale_done` to remain present.
