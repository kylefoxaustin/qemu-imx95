# QEMU i.MX95 Neutron - Two-Track Roadmap: Internal Prototype vs. Upstreamable Model

Author: Kyle Fox
Date: 2026-07-23
Status: Planning / architecture direction
Audience: ACC (internal builder), the runner-backend owner, and future upstream author

---

## 0. Why this document exists

The Neutron work has quietly become **two different projects wearing one repo**:

1. **Track A - the internal prototype.** Get *real inference results* flowing
   through QEMU i.MX95 as fast as possible so BSP/CI has something to assert on.
   This is the `runner` backend: spawn NXP's proprietary `neutron-runner`,
   feed it the carveout, DMA the answer back. Pragmatic, fast, proprietary.

2. **Track B - the upstreamable model.** A clean QEMU device that could
   eventually be posted to `qemu-devel` and accepted into mainline: mailbox
   front end + honest `null` backend + (eventually) a self-contained in-tree
   compute engine. No proprietary binaries, no subprocess exec, no fixtures.

**The dead-end risk** is building Track A in a way that quietly poisons Track B -
e.g. baking `g_subprocess`/fixtures assumptions into the front end, or letting
the backend seam leak proprietary concepts into guest-visible behavior.

**The insurance policy** is the `NeutronBackendOps` seam that already exists.
Used deliberately, it lets Track A and Track B coexist forever without one
blocking the other. This document defines the discipline that keeps that true.

> One-line summary: **Ship the runner to unblock CI now, but treat the front end
> as if a QEMU maintainer will read it, and keep every proprietary/subprocess/
> fixture concept strictly below the backend seam.**

---

## 1. The load-bearing rule (read this even if you read nothing else)

> **The backend seam is the upstream boundary.**
> Everything *above* `NeutronBackendOps` must be written to mainline QEMU
> standards from day one. Everything *below* it (runner, fixtures, subprocess,
> scratch files, proprietary anything) is explicitly downstream and may be as
> pragmatic as you like.

If you obey exactly one rule, obey that one. It converts "we'll clean it up
later" (which never happens) into "the clean part and the dirty part are
physically separated by a typedef."

Concretely:

- `hw/misc/imx95_neutron.c` (front end): **Track B quality, always.**
- `include/hw/misc/imx95_neutron_compute.h` (the seam): **Track B quality.**
- `hw/misc/imx95_neutron_backend_null.c`: **Track B quality** (this is the
  in-tree backend that ships upstream).
- `hw/misc/imx95_neutron_backend_runner.c`: **Track A only.** Never upstreamed.
- `hw/misc/imx95_neutron_backend_native.c` (future pure-C engine): **Track B**,
  the eventual real upstream compute path.

---

## 2. Track assignment - what belongs where

| Component | Track | Upstreamable? | Notes |
|---|---|---|---|
| Mailbox MMIO front end, registers, doorbell, IRQ | B | Yes | Legit SoC peripheral model |
| Two-phase RUN_ACK / DONE, timer/worker completion | B | Yes | Standard QEMU async device pattern |
| TCM RAM regions, remoteproc handshake | B | Yes | Hardware-accurate |
| QOM property surface (front-end props only) | B | Yes | Keep runner props namespaced separately |
| `NeutronBackendOps` seam | B | Conditional | Upstreamable *iff* the in-tree backend is real (null/native), not a hook whose only impl is proprietary |
| `null` backend (honest errcode, no compute) | B | Yes | Common `hw/misc` shape; this is what ships upstream first |
| Pure-C `native` compute engine (future) | B | Yes | The i.MX93 Ethos-U precedent; the real upstream compute story |
| `runner` subprocess backend | A | No | External proprietary ELF + subprocess exec |
| `fixtures.json` (offset->canned output) | A | No | Test scaffolding, not hardware behavior |
| Scratch-dir / `/tmp` / `outputs.bin` orchestration | A | No | Host orchestration, out of scope for a device model |
| `run-bitexact.sh`, EXTRA_QEMU_ARGS wiring | A | No | Downstream CI glue |
| qtests for front end + null backend | B | Yes | In-tree, no proprietary deps |
| qtests gated on `QEMU_TEST_NEUTRON_RUNNER=1` | A | No | Skip cleanly upstream; fine downstream |

**Reading of the table:** the *only* things that ever go to `qemu-devel` are the
front end, the seam, the null backend, the native engine (later), and their
in-tree qtests. Everything Track A is a permanent, deliberate downstream fork.

---

## 3. Phase plan

### Phase 0 - Separation hygiene (do this now, ~1 day)

Before more features land, make the split physical so it can't rot:

- Split backends into their own files: `imx95_neutron_backend_null.c` and
  `imx95_neutron_backend_runner.c`. Front end must not `#include` or reference
  runner concepts.
- Namespace QOM props: front-end props (`compute-backend`,
  `neutron-uncomputed-errcode`) vs. runner-only props
  (`neutron-runner-*`). The runner props should only be *registered* when the
  runner backend is compiled/selected, so a null-only build has zero runner
  surface.
- Add a build gate so the runner backend can be compiled **out** entirely.
  Target state: `configure`/meson can produce a QEMU where
  `imx95_neutron_backend_runner.c` is not even in the binary, and the device
  still works (null backend). **This is the single best upstream-readiness
  proof you can have.**
- One-line grep test: `git grep -l subprocess hw/misc/imx95_neutron.c` must
  return nothing. Same for `fixtures`, `g_subprocess`, `/tmp`, `scratch`.

### Phase 1 - Make it work (Track A, internal)

This is the roadmap ACC already agreed to; it's all Track A and that's fine:

- State-machine spec first (also feeds Track B - see below).
- `NEUTRON_ERR_BUSY = 0x95E6` + BUSY rejection.
- Async subprocess wait + tracepoints.
- Five concurrent-RUN qtests + cleanup audit.
- Fix `EXEC_FAILED`.
- Content-hash fixture keys.
- EXTRA_QEMU_ARGS wiring.

**Goal of Phase 1:** real MobileNet inference through QEMU, bit-exact, in
internal CI. Ship it. Don't gold-plate for upstream yet.

### Phase 2 - Harvest the upstreamable core (Track B)

Once Track A works and you *understand the device behavior for real*, extract
the clean subset and prepare it for `qemu-devel`:

- Front end + seam + null backend, checkpatch-clean, tracepoints (no stray
  logging), signed-off-by, in-tree qtest, docs under `docs/system/arm/`.
- RFC cover letter to `qemu-devel`: "i.MX95 Neutron mailbox peripheral +
  null backend," explicitly stating the compute engine is future work.
- **Gate:** get maintainer sentiment *before* writing the native engine.

### Phase 3 - The real upstream compute path (Track B, optional/aspirational)

If (and only if) upstreaming the *compute* is worth it, build the pure-C
in-tree engine from the original spec (`neutron_compute_run`) as
`imx95_neutron_backend_native.c`. This is the i.MX93 Ethos-U pattern and is the
only compute path that can go upstream. The runner stays downstream forever as
the "fast/opaque" alternative.

---

## 4. How the architecture must CHANGE to become upstreamable

This is the "explain in great detail" part. Assume Track A works and you now
want the same *observable behavior* through an upstreamable design. Here is
what has to change, and why.

### 4.1 Remove the subprocess entirely - compute must be in-process

**Today (Track A):** device submit path -> `g_subprocess_new(neutron-runner ...)`
-> poll -> read `outputs.bin` -> DMA back.

**Why upstream rejects it:** QEMU device models don't shell out to external
binaries to produce guest-visible DMA results. Back ends process data through
defined host mechanisms (chardev/blockdev/sockets/files), not arbitrary exec.
It's an architecture *and* a security objection, and it's a reliable rejection.

**Upstream shape:** the compute happens inside the QEMU process, in a worker
thread, touching guest memory only via `dma_memory_read/write` through an
`AddressSpace`. That's `imx95_neutron_backend_native.c`. The worker->done-BH
threading model is identical to what the runner backend already needs; only the
"how do I get the answer" step changes from *spawn a process* to *call a
function*.

**Migration:** the `NeutronBackendOps::submit` signature does not change. You
implement a second backend with the same seam. The front end doesn't notice.

### 4.2 Delete fixtures as a compute source

**Today:** `fixtures.json` maps `<microcode_offset>:<tensor_count>` (later a
content hash) to a pre-baked `.tflite` + canned output offsets.

**Why upstream rejects it:** a device whose "compute" is a lookup table of
canned outputs isn't modeling hardware. Mainline can't run it (proprietary
model artifacts), and it isn't reproducible in-tree.

**Upstream shape:** the native engine *actually interprets* the microcode +
tensor descriptors from the carveout and computes. Its tests use small,
self-contained golden vectors committed in-tree (tiny conv/depthwise), not
proprietary model drops. Fixtures survive only as a **downstream Track A**
convenience.

### 4.3 The golden-vector problem moves in-tree

**Today:** correctness = "matches whatever the runner produced."

**Upstream shape:** correctness = "matches an in-tree golden the CI can
recompute or that is small enough to commit." Because Neutron's converter/ISA
is proprietary, the upstream native engine can only be validated against goldens
you can *publish*. Practically this means the upstream engine's op coverage is
bounded by what you can golden publicly - which is fine; partial coverage with
clean "unsupported op" errors is an accepted upstream pattern (Ethos-U did
exactly this).

**Implication:** the ISA/descriptor details that are proprietary are a genuine
constraint on how much compute can ever be upstreamed. The *front end* has no
such constraint - it's just a peripheral. This is why Phase 2 (front end + null)
is the safe, guaranteed-upstreamable deliverable, and Phase 3 (native engine) is
aspirational and scope-limited by what NXP is willing to open.

### 4.4 Logging -> tracepoints, everywhere

**Today:** glib CRITICAL noise, printf-ish diagnostics from the runner path.

**Upstream shape:** everything guest-visible or debug-relevant goes through
QEMU `trace-events`. No stray `fprintf`, no glib warnings leaking from device
code. You're already adding tracepoints in Phase 1 - just make sure the
*front-end* ones are clean enough to ship, independent of the runner ones.

### 4.5 QOM property surface must be honest for a null-only build

**Today:** runner props (`neutron-runner-path`, `-fixtures`, `-scratch-dir`,
`-timeout-ms`, `-keep-scratch`) exist on the device.

**Upstream shape:** a mainline QEMU build has *none* of those, because the
runner backend isn't compiled in. Only `compute-backend` (with upstream values
like `null`, and later `native`) and the null errcode prop survive. The
`runner` value simply isn't a legal choice in an upstream build. Enforce this in
Phase 0 by registering runner props only when the runner backend is present.

### 4.6 Reset / migration correctness becomes mandatory, not P3

**Today:** reset teardown of the deferred DONE and stale-completion handling is
on the roadmap (P3-ish).

**Upstream shape:** device models are held to VMState/migration and reset
correctness as table stakes. The `RUN -> RESET -> DONE(old job)` case (my repeated
hobby-horse) *must* be provably handled, and any in-flight timer/worker state
must be correctly reset and either migrated or explicitly blocked during
migration. For upstream, "half-armed timer survives migration" is a hard bug.
Good news: the state-machine doc you're writing first is exactly the artifact a
maintainer will want to see for this.

### 4.7 The seam itself may need justification

**Today:** `NeutronBackendOps` is a vtable with `realize/submit/cleanup`.

**Upstream nuance:** maintainers are wary of plugin seams whose *only real
implementation* is proprietary and out-of-tree - it can look like a hook for a
binary blob. The seam is defensible **iff** there's a genuine in-tree backend
behind it (null now, native later). Framing for the mailing list: "the device
supports pluggable compute backends; the in-tree backends are `null` and
`native`; vendors may provide additional backends downstream." That's a normal,
acceptable story. "There's a hook and the real one is secret" is not.

---

## 5. Decision gates (so you don't sink cost into a dead-end)

- **Gate G0 (end of Phase 0):** Can you build QEMU with the runner backend
  compiled *out* and the device still works via null? If no, stop and fix the
  separation before adding features.
- **Gate G1 (end of Phase 1):** Real inference in internal CI. Track A done.
  Decision: is upstreaming still a real business goal? If no, you stop here with
  a clean internal fork and *no wasted upstream effort* - the separation
  discipline cost you almost nothing.
- **Gate G2 (mid Phase 2):** RFC to `qemu-devel` for front end + null. Decision
  point is a maintainer reply, not a guess. Cheap (one email), high-information.
- **Gate G3 (before Phase 3):** Is enough of the Neutron ISA/descriptor layout
  publishable to make an in-tree native engine worth building? If NXP won't open
  it, Phase 3 is capped at whatever ops you can golden publicly - decide scope
  with that constraint explicit.

---

## 6. Dead-end risk register

| Risk | Consequence | Mitigation |
|---|---|---|
| Runner concepts leak into front end | Front end becomes un-upstreamable; whole thing forks forever | Phase 0 grep gate; runner strictly below seam |
| Fixtures treated as the compute model | No path to real in-tree compute | Fixtures are Track A only; native engine computes for real |
| Upstream effort spent before maintainer buy-in | Wasted weeks on patches that get NAK'd | RFC at G2 before writing native engine |
| Migration/reset treated as polish | Upstream hard-blocks; subtle CI heisenbugs downstream too | State-machine doc first; stale-DONE qtest; VMState from the start on front end |
| Seam looks like a proprietary-blob hook | Maintainer distrust | Ship a real in-tree backend (null now, native later); frame accordingly |
| Assuming full compute is upstreamable | Over-promise; blocked by proprietary ISA | Scope Phase 3 to publishable-golden ops only |

---

## 7. TL;DR

- **Two tracks, one seam.** Above `NeutronBackendOps` = upstream-quality always.
  Below it = pragmatic/proprietary/downstream forever.
- **Phase 0 now:** physically separate backends; prove a runner-compiled-out
  build works. Cheap insurance against the dead-end.
- **Phase 1:** ship the runner, get real inference in CI. This is Track A and
  it's fine that it's not upstreamable.
- **Phase 2:** harvest front end + null backend, RFC to `qemu-devel`. This is
  the guaranteed-upstreamable deliverable.
- **Phase 3 (optional):** pure-C in-tree native engine (the i.MX93 Ethos-U
  pattern) is the *only* upstreamable compute path; runner stays downstream.
- **To go upstream the architecture must:** drop the subprocess, drop fixtures
  as a compute source, move goldens in-tree, convert logging to tracepoints,
  shrink the QOM surface for null-only builds, make reset/migration correct, and
  justify the seam with a real in-tree backend behind it.
- **You are not stuck.** Because the seam already exists, none of the Track A
  work is wasted, and the upstream path is additive, not a rewrite.
