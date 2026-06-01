# qemu-imx95 — validation TODO (pre-submission shakeout)

**Audience:** project maintainer (Kyle), future contributors, anyone
preparing the project for broader release.

**Status:** v1.0 (`imx95-v1.0`) ships with the *minimum* validation required
for the artifact's stated scope (boot stock NXP BSP Linux 6.12.49 to userspace
on real System Manager firmware). This document lists the validation work that
would strengthen the artifact before broader release — particularly before
upstream submission to qemu-arm@ — and the failure modes that, even after
validation, are out of scope.

---

## Why this document exists

The v1.0 boot demonstration is real but narrow: one kernel image, one DTB, one
tiny initramfs, one specific user on one specific machine. That's sufficient
evidence the artifact *can* work. It's not sufficient evidence the artifact
will work for *anyone else trying anything else*.

Before posting the project to qemu-arm@ or otherwise inviting external users,
three classes of risk should be reduced:

1. **Reproducibility** — can someone else on a different machine reproduce the
   v1.0 boot result by following the README?
2. **Robustness** — does the artifact tolerate inputs beyond the specific ones
   tested (different kernel versions, larger workloads, longer runtimes)?
3. **Coverage** — which use cases the project's scope claims to support
   actually work in practice?

These are addressed in three tiers.

---

## Tier 1 — Pre-submission gates

Must pass before posting an RFC PATCH series to qemu-arm@. Maintainers
reproducing locally is the first review step; if reproduction fails, the
series stalls.

### 1.1 Fresh-clone reproducibility (same machine)
From `git clone` with no pre-existing project state, follow the README's
Building + Required-artifacts + Quick-start sections verbatim and boot to
`/init`. **Pass:** `USERSPACE OK` within ~30 s, no fatal aborts.
Catches missing build deps and undocumented environment assumptions. ~2-3 h.

**PASSED (2026-06-01).** Done two independent ways:
(a) an independent non-author validator (different machine, Ubuntu 22.04.5,
fresh clone) built and booted to userspace in ~13 s with zero workarounds; and
(b) `tests/docker-repro/run.sh` — a **pristine `ubuntu:22.04` container** with
nothing pre-installed: fresh clone → `configure`/`ninja` (build 76–84 s) →
both smoke tests → Linux to `/init` at ~13.5 s. The container run **found
three undocumented build/runtime deps** that every prior on-machine test
missed because they were already present — `python3-venv` (ensurepip),
`python3-tomli` (meson TOML), `netcat-openbsd` (`nc -U` for the M7 HMP checks).
All three are now in the README Building section; the container passes 4/4.

### 1.2 Fresh-clone reproducibility (different machine)
Same as 1.1 on a different Ubuntu 22.04/24.04 machine. **The silent killer:**
no tracked file may hardcode a personal path —
`git grep -nE '/home/[a-z]+|Documents/'` over `*.sh *.py *.md` should be clean
of developer-specific layout. ~3-4 h (incl. VM setup).

**Path check: PASSED (2026-06-01).** The hardcoded-layout half of this gate is
fixed: the test scripts' artifact defaults no longer encode the author's
personal home-directory tree. They now fall back to a neutral convention
directory `$IMX95_ARTIFACTS` (default `~/imx95-artifacts`), still overridable
per-var (`SM_ELF`/`KERNEL`/`DTB`/`INITRD`); layout documented in the README's
"Required artifacts" section. The gate grep over tracked `*.sh *.py` is now
clean.

**Boot run: PASSED (2026-06-01).** The different-machine half is also done: an
independent non-author validator reproduced the full build + boot to userspace
on a separate Ubuntu 22.04.5 box, and `tests/docker-repro/run.sh` does it from
a pristine container (see Tier 1.1 above). Both Tier 1.1 and 1.2 are met.

### 1.3 BusyBox initramfs interactive shell
Boot with a static aarch64 BusyBox initramfs; verify an interactive shell, and
`ls /`, `cat /proc/cpuinfo` (6 A55s), `ps`, `uname -a`, `dmesg`. Proves
userspace is genuinely alive (and exercises Linux-console LPUART RX). The
BusyBox initramfs + build script becomes a committed artifact
(`tests/busybox-initramfs/`). ~2-3 h.

**Status: PASSED (commit 741af2f5e3).** Tier 1.3 surfaced a latent bug in the
LPUART model's FIFO status handling (it latched read-only RXEMPT from a guest
read-modify-write; fix = store only writable bits on write, derive status fresh
on read). Both "no interactive console" and "SCMI desync under console input"
were resolved by the single fix — the campaign working as designed: found the
bug before strangers did.

### 1.4 Long-duration stability run
24 h run; check responsiveness, stable host RAM (±10%), no panics/RCU stalls/
WARN accumulation. Catches device-model leaks, virtual-time drift, GIC
pending-state accumulation. ~25 h wall, ~30 min active.

---

## Tier 2 — Strengthening

Before broad announcement (blog/talk/NXP presentation).

- **2.1 Different kernel version** — boot a kernel.org 6.6/6.1 LTS against the
  same DTB; document what differs. ~4-6 h.
- **2.2 Different DTB variant** — `imx95-15x15-evk.dtb` if NXP ships it; even
  partial boot (PSCI/GIC/SCMI) is informative. ~3-4 h.
- **2.3 Larger initramfs + file I/O** — ~50-100 MB; sustained `find`/`md5sum`/
  `dd`. ~2-3 h.
- **2.4 Multiple parallel boots** — two instances at once; check for shared
  socket/file state. ~1-2 h.
- **2.5 SM firmware build reproducibility** — rebuild `m33_image.elf` from the
  *current* NXP imx-sm tree per the README; boot it. ~2-3 h.

---

## Tier 3 — Coverage probing (informational, not gating)

Turn "we don't know if X works" into "X fails because Y, documented."

- **3.1 Networking** — `ifconfig`/`ping`/`wget`; expect no interfaces (NETC/MAC
  stubs). ~1 h.
- **3.2 Persistent storage** — `-drive ...`, attempt MMC mount; document where
  it breaks. ~1-2 h.
- **3.3 Android kernel boot** — boot the Android-for-i.MX-95 kernel; characterize
  what fails (display/storage/USB/GPU gaps). Full Android-with-UI is explicitly
  out of scope (v2.x at best). ~4-6 h.
- **3.4 GPU stress** — `glmark2`/`vkmark` with a Mesa initramfs; canonical
  probe-stub-GPU failure (opens `/dev/dri/card0`, hangs on command submit).
  Document the symptom. ~2-3 h.
- **3.5 NXP team workflow validation** — watch an NXP BSP dev use it for a real
  task (kernel peripheral-driver change, SM firmware mod). The single most
  valuable experiment for upstream positioning. ~half-day+.

---

## Suggested execution order

**Must do before any submission:** 1.1, 1.2, 1.3.
**Strongly recommended:** 1.4; 3.1/3.2/3.4 (failure-mode docs).
**Before broad announcement:** 2.1, 2.5, 3.5.
**Optional/informational:** 2.2/2.3/2.4; 3.3.

Recommended-path active time: ~25-35 h across multiple sessions; the 24 h
stability test runs in the background.

---

## Out of scope (deliberate, not validation gaps)

- Hardware accelerator emulation (GPU/VPU/NPU)
- Cycle-accurate behavior (this is functional emulation)
- Production-deployment validation
- Full Android-with-UI

---

## After the campaign

Update the README's "What runs today" with strengthened claims;
`docs/imx95/known-limitations.md` with characterized Tier-3 failures; and add
`docs/reviews/validation-report.md` summarizing tested/passed/failed/untested
— the evidence base for the eventual upstream cover letter.

---

The campaign isn't about adding capability — it's about strengthening the
evidence that the existing capability works for users beyond the author on
machines beyond the original. The risk being managed: an upstream submission
that fails to reproduce in maintainers' hands. Worth doing before posting to
qemu-arm@.
