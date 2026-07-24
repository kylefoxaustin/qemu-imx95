# Neutron Phase 1 Ledger

Status: Phase 1 open; reviewer green-lit 2026-07-23 (see
`neutron-phase1-reviewer-response.md`).

## Binding constraint

- **Item 0 lands FIRST**, before any FSM implementation. Non-negotiable.

## Items

| # | Item | Owner | DoD |
|---|------|-------|-----|
| 0 | **STUB null-only CI job (binding, first)** | ACC | **DONE (uncommitted, pending CI verification).** Landed as: (a) removed hard `select IMX95_NEUTRON_RUNNER` from `config FSL_IMX95` in `hw/arm/Kconfig`; (b) added `default y if IMX95_NEUTRON` to `config IMX95_NEUTRON_RUNNER` in `hw/misc/Kconfig` so normal builds are unaffected; (c) `configs/devices/aarch64-softmmu/imx95-neutron-stub.mak` sets `CONFIG_IMX95_NEUTRON_RUNNER=n` (sole switch, no Kconfig hand-edit needed to reach stub build); (d) GitLab CI jobs `build-neutron-stub` + `check-neutron-stub` in `.gitlab-ci.d/buildtest.yml` run hygiene grep + `imx95-neutron-test` + `imx95-neutron-realize-test` (triggered on changes to `hw/misc/imx95_neutron*`, `hw/misc/Kconfig`, `hw/arm/Kconfig`, the stub .mak, `tests/neutron/**`, `tests/qtest/imx95-neutron*`); (e) local dry-run script `tests/neutron/run-stub-build-check.sh`. Local build unavailable on this host (no ninja); relying on CI for machine-enforced verification. |
| 1 | FSM implementation (spec section 4 + section 6 done-BH guard) | ACC | **DONE (code landed as `5870fb495c`, pending CI green).** Added `NeutronPhase` enum (IDLE/RUNNING/COMPLETE), monotonic `job_seq` + `active_seq`, heap `NeutronJob` type, `neutron_reject_busy()` helper (unifies 7.1/7.2/7.3), `neutron_snapshot_request()`, `neutron_submit_run()`, `neutron_done_run()` with stale-completion guard (`phase == RUNNING && job->seq == active_seq`), `neutron_soft_reset()` for MBOX3=RESET (disown via `active_seq=0`), device-reset disown, and `migrate_add_blocker`/`migrate_del_blocker` around RUNNING (spec section 9). Backend seam kept synchronous per the handoff option "keep req/res signature and pass job as opaque"; `NeutronBackendOps` and all backends untouched. `done_timer` (QEMU_CLOCK_VIRTUAL, `NEUTRON_RUN_NS=1000`) retained as the two-phase completion deferral so existing `imx95-neutron-test` / `imx95-neutron-realize-test` remain green. TODO(Item 3) markers left where tracepoints will land. Local build unavailable (no ninja); Item 2 gated on CI green for `5870fb495c`. |
| 2 | T1-T12 qtests (`imx95-neutron-fsm-test.c`) | ACC | Skeleton in `neutron-qtest-skeleton.md`; written against the spec, not the impl; all TODO(acc) resolved or explicitly SKIPped with reason. |
| 3 | Tracepoints (spec section 10 + `neutron-trace-events-contract.md`) | ACC | Track B set in `imx95_neutron.c`; Track A set in `imx95_neutron_backend_runner.c`; hygiene grep extended to forbid `trace_neutron_runner_*` in front end and require `trace_neutron_stale_done`. |
| 4 | VMState + migrate blocker (spec section 9) | ACC | Blocker registered on RUN accept, removed on DONE/RESET; also cleared by EV_DEVRESET (spec fold-in #3). Doubles as RFC's reset/migration story. |
| C | (Carried from 0.5) Track A directory decision | ACC | Confirm `downstream/imx95-neutron-runner/` (preferred over `nxp/`) BEFORE first `git mv`. |

## FSM spec fold-ins (non-blocking, apply as implementation proceeds)

1. Invariant #7: no guest-observable dwell in ACKED without a live worker; RUN_ACK + worker spawn in one BQL critical section.
2. Unify sections 7.1 and 7.3 through single `neutron_reject_busy()` helper.
3. §8/§9 interaction sentence: EV_DEVRESET removes migrate blocker as part of disowning worker.
4. `job_seq` post-load safety note: post-load has no live worker; §6 guard uses `active_seq` regardless.
5. Mark T8 both-outcomes-legal: MBOX1=TIMEOUT OR clean stale-drop, never a third.

Applied to `neutron-state-machine-spec.md` via an amendments section (or inline
edits) as item 1 lands.

## Durable design principle

> "When you convert compile-time-static state into a runtime-injected seam,
> you inherit a lifetime problem - every time."

Carry into the RFC cover letter's design-rationale section.

## Sequencing summary

```
item 0 (CI + stub build)   MUST land first
  |
  v
item 1 (FSM)  ->  item 2 (qtests)  ->  item 3 (traces)  ->  item 4 (VMState)
                     (items 2/3 can develop in parallel; 3 before 2's trace assertions land)
```
