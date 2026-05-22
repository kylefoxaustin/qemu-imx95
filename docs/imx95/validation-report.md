# qemu-imx95 — Validation Report

A pre-submission validation campaign run after the v1.0 tag (2026-05-21 → 2026-05-22),
before posting the project to qemu-arm@. It tests the artifact along three axes:

1. **Reproducibility** — can someone else reproduce the v1.0 boot result by
   following the README?
2. **Robustness** — does the artifact tolerate inputs beyond the specific ones
   originally tested (different kernel versions, larger workloads, longer
   runtimes)?
3. **Coverage** — which use cases the project's scope claims to support
   actually work, and where do the documented limitations bite?

The campaign earned its keep: it found and fixed **two real bugs** before any
stranger would have, and turned three "we don't know if X works" items into
"X fails because Y, documented."

---

## Summary

| # | Item | Result |
|---|---|---|
| 1.1 | Fresh-clone reproducibility (same machine) | PASS |
| 1.2 | Fresh-clone reproducibility (different env, clean container) | PASS — **bug found + fixed** |
| 1.3 | BusyBox initramfs interactive shell | PASS — **bug found + fixed** |
| 1.4 | 24 h stability run | IN PROGRESS (healthy in progress) |
| 2.1 | Different kernel (×3) | PASS |
| 2.2 | Different DTB variant (15×15 EVK) | PARTIAL (expected) |
| 2.3 | Larger initramfs + fuller (glibc-dynamic) userspace | PASS |
| 2.4 | Multiple parallel boots | PASS |
| 2.5 | SM firmware build reproducibility | PASS |
| 3.1 | Networking | characterized (no interfaces) |
| 3.2 | Persistent storage from Linux | characterized (driver defers probe) |
| 3.3 | Android boot | structurally blocked (no usable GL path; no Linux MMC) |
| 3.4 | GPU stress (glmark2 / vkmark / kmscube / vulkaninfo) | characterized (precise failure points) |
| 3.5 | NXP-team workflow validation | not run (needs an NXP BSP developer) |

---

## Bugs the campaign caught

**LPUART FIFO storm — fixed in commit `741af2f5e3` (during 1.3).** The model
latched the read-only `RXEMPT` status bit because the FIFO-register write
handler stored the whole written value into `s->fifo`. Linux's `fsl_lpuart`
read-modify-writes `UARTFIFO` at setup, so once `RXEMPT` was set by the model
it stayed set — the interrupt-RX drain loop (`while (!(UARTFIFO & RXEMPT))`)
then thought the FIFO was always empty, never read DATA, never cleared RDRF,
and the level RX IRQ stormed at ~75 kHz on a single typed byte. That storm
both blocked the byte from reaching the shell and starved the MU2 SCMI IRQ —
producing two seemingly-unrelated symptoms (dead console RX *and* "SCMI message
not expected" desyncs under input) from one root cause. Fix: derive
`TXEMPT`/`RXEMPT` fresh on read; store only writable bits on write.

**Undocumented `file` dependency — fixed in commit `7def38532a` (during 1.2).**
`tests/busybox-initramfs/build.sh` shelled out to `file(1)` to sanity-check
the arch of the BusyBox binary. `file` is absent on a minimal Ubuntu install
and is not in the README's dependency list — invisible on the author's host,
but the textbook silent killer for a new contributor. Fixed by reading the
ELF `e_machine` field with `od` (coreutils, always present); the external
dependency is gone, not added to the README.

---

## Tier 1 — Pre-submission gates

**1.1 Fresh-clone (same machine) — PASS.** A clean clone of the repo into a
fresh directory builds (`configure --target-list=aarch64-softmmu` + `ninja
qemu-system-aarch64`) and boots to `/init` in 13.1 s with the v1.0 artifacts
and `SCMI Protocol v2.1 'NXP:IMX' Firmware 0x333`. The README's build path
works from a clean clone.

**1.2 Fresh-clone (different environment) — PASS.** A clean `ubuntu:24.04`
Docker container — different user (`root`), different `$HOME` (`/root`), zero
pre-existing project state, fresh `apt` with **only the README-documented
packages** — runs the full flow: `git clone` (file://) → 604 MB working tree,
no untracked artifacts leaked; `configure` + `ninja` succeed; machine registers;
the committed `busybox-initramfs/build.sh` builds the initramfs; boot on the
real SM (with the three user-supplied artifacts mounted read-only, env vars set
per the README) reaches `Run /init` at 13.2 s, 0 SCMI/MU errors. Caveat: a
container shares the host kernel, so this is not different *silicon*; it closes
the reproducibility risk (clean build from documented deps, no leaked personal
paths) which is what 1.2 exists to catch. A true different-hardware run (cloud
VM) is optional follow-up.

**1.3 Interactive shell — PASS** (after the LPUART fix above). A static
aarch64 BusyBox initramfs boots, gives an interactive shell on `ttyLP0`,
runs `ls /`, `cat /proc/cpuinfo` (6 A55s reported), `ps`, `uname -a`, `dmesg`.
Deeper stress: 300 fork/execs, 10 rapid DVFS `scaling_setspeed` changes
(500/900/1404/1800 MHz = 10 runtime SCMI perf-set round-trips, all succeed,
0 timeouts), 128 MiB write+read with md5 integrity across two reads,
extensive `/proc` and `/sys` reads — all clean, 0 panics, 0 RCU stalls,
0 SCMI timeouts.

**1.4 24 h stability — in progress.** Detached run, heartbeat every ~30 s
recording `MemAvailable`, an md5 over a 4 MiB scratch file, and the current
A55 cpufreq. At the time of writing, ~7 h in: MemAvailable stable (no leak),
md5 stable across all heartbeats (data integrity), 0 panics/SCMI-timeouts/
RCU/BUGs. Analysis will be appended once it completes.

---

## Tier 2 — Strengthening

**2.1 Different kernel — PASS, three kernels.** The machine is not tied to
NXP's vendor 6.12.49:
- **Mainline Linux 6.12.0** (kernel.org LTS, arm64 defconfig, mainline
  `imx95-19x19-evk.dtb`) boots to `Run /init` and binds SCMI to the real SM
  unchanged.
- **NXP BSP 6.18.2** (the prebuilt `Image-imx95evk.bin` from
  `LF_v6.18.2-1.0.0`, a kernel six minor versions newer than 6.12.49) boots
  to `Run /init` at 13.1 s, also binds SCMI to our 6.12.49-era SM — and
  applies a *newer* SCMI quirk (`quirk_clock_rates_triplet_out_of_spec`)
  harmlessly. Strong forward-compat evidence.

**2.2 Different DTB variant (15×15 EVK) — PARTIAL, as expected.** Booting
the 6.18.2 kernel with the 6.18.2 `imx95-15x15-evk.dtb` (a different *board*
than the 19×19 EVK this machine models) brings up the board-agnostic core in
full — PSCI, GIC, **SCMI binds to the real SM** — then takes a synchronous
external abort (ESR `0x96000010`) at 8.6 s in `svc_i3c_master_probe`. The
15×15 EVK populates a Silvaco I3C master controller whose MMIO is unmapped in
the 19×19 model; the driver does an unguarded `readl` and SErrors. Correct
outcome: this machine models the 19×19 EVK, not the 15×15.

**2.3 Larger initramfs + fuller userspace — PASS.** A trimmed glibc-dynamic
initramfs (8.9 MB cpio.gz, 56 binaries + 27 shared libs) built from the NXP
Yocto rootfs boots cleanly: real `bash 5.2.37 (aarch64-poky-linux-gnu)`,
dynamically linked through `ld-linux-aarch64.so.1` → `libc.so.6` (exercises
the dynamic loader + glibc, which the static BusyBox did not). Sustained file
I/O: `dd` 100 MiB at 240 MB/s, `md5sum` twice with identical result
(integrity ok), recursive `find`, memory clean after `rm`. SCMI stayed stable
under the I/O load: 0 real errors.

**2.4 Multiple parallel boots — PASS.** Two instances of `swap-boot/run.sh`
launched within seconds of each other both reach userspace without
shared-state collisions.

**2.5 SM firmware build reproducibility — PASS.** The README recipe
(`rm -rf build/mx95evk; TOOLS=$HOME/tools make config=mx95evk all`) rebuilds
the SM firmware in 11 s, byte-differing from the prior `m33_image.elf` only by
the embedded timestamp/commit; the fresh ELF banners and reaches its debug
monitor.

---

## Tier 3 — Coverage probing (characterized failure modes)

**3.1 Networking — no usable interfaces.** Only `lo` appears. The NETC/enetc
blocks are logging stubs; `ping` reports `Network is unreachable`. The
integrated-PCIe ECAM probes but enumerates no devices (empty slots by design).
Matches the project's stated scope.

**3.2 Persistent storage (Linux view) — no block device.** With
`-drive ...,id=sd0 -device sd-card,drive=sd0` attached, Linux shows no
`/dev/mmcblk*` and an empty `/proc/partitions`: the `sdhci-esdhc-imx` driver
defers probe on an unmet dependency. The uSDHC *model* works (U-Boot SPL
booted from SD in v0.2), but Linux's MMC driver never completes probe, so the
card is invisible to userspace. Mounting an actual SD-card image from Linux
would require resolving that deferred-probe chain.

**3.3 Android boot — structurally blocked.** No Android-for-i.MX95 BSP image
was run (the NXP Android tree is a separate download not used in this
campaign). Structurally, an Android-with-UI boot needs at minimum:

- **Working Mali GL/Vulkan rendering** — for surfaceflinger / gralloc /
  hwcomposer. **Broken** (see 3.4 below: Mali driver fails probe).
- **Linux block storage** to mount `system.img`/`vendor.img`. **Broken**
  (see 3.2: Linux MMC defers probe).
- **gralloc / binder / surfaceflinger / input** all depend on the above.

So Android-with-UI is blocked by at least two already-documented gaps; a
bare-kernel boot with an Android cmdline could be attempted but would not
reach a usable init. Real Android characterization is a v2.x-scope follow-up.

**3.4 GPU stress — characterized precisely (revised from prior docs).** A
glibc Mesa userspace (built from the Yocto rootfs: `vulkaninfo`, `vkmark`,
`glmark2-es2`, `modetest`, `kmscube`, `weston`) booted on the real SM and
exercised the GPU/DRM path. Findings:

- **`/dev/dri/card0` and `/dev/dri/renderD128` DO exist** — registered by the
  **DPU stub** (`hw/misc/imx95_dpu.c`, `4b400000.display-controller`), not by
  Mali. (Prior docs said "the Mali driver completes registration with DRM";
  that was wrong — corrected in `known-limitations.md` §3.)
- **Mali probe fails entirely with `-22`** at the GPU-ID check: the stub
  returns `0`, the driver logs `Unknown GPU Product ID 0` →
  `Early device initialization failed error = -22`. No Mali Vulkan ICD is
  installed; no Mali GL operations are possible.
- **`vulkaninfo`** fails at instance creation: `Found no drivers! Cannot
  create Vulkan instance. ... ERROR_INCOMPATIBLE_DRIVER`.
- **`modetest`** doesn't attach: it probes a hardcoded driver-name list
  (`i915`, `amdgpu`, `radeon`, `nouveau`, `vmwgfx`) and our DPU stub doesn't
  match any of them.
- **`kmscube`** fails at two layers: `Unable to open libmali.so` (no Mali
  userspace ICD), then `no connected connector! failed to initialize legacy
  DRM` — the DPU stub has no connectors/scanout.

Throughout, the kernel didn't panic and SCMI stayed bound. This is the
canonical "stubbed-GPU SoC emulation" failure mode and matches the project's
stated scope; the precise failure points are now documented for future users.

**3.5 NXP-team workflow validation — not run.** This needs an NXP BSP
developer using the artifact for a real task (a kernel peripheral-driver
change, an SM firmware mod, etc.) and remains the single most valuable
follow-up before broad announcement.

---

## What this campaign proves

The artifact's stated scope — *boot stock NXP BSP Linux 6.12.49 to userspace
on the real System Manager firmware, on the NXP i.MX 95 19×19 EVK board* — is
reproducible from a clean clone with only documented build dependencies, on a
different environment than the author's, with three distinct kernel versions
(NXP 6.12.49 vendor, mainline 6.12.0, NXP 6.18.2 vendor), with two distinct
userspaces (static BusyBox and glibc-dynamic Yocto), and remains stable under
sustained workload. Tier-3 limitations have moved from "we don't know if X
works" to "X fails because Y, with this exact log line." The two bugs found
along the way were genuine — one in the LPUART model, one in a test
script — and would have been the kind of thing a first external user trips
over.

The artifact is not validated for: a literally different physical machine /
host kernel; an actual Android boot; hardware-accelerated GPU/VPU/NPU
workloads; or a stranger using it for a real BSP development task. Those
remain as honest follow-ups (1.2-physical, 3.3, 3.4-extended, 3.5).
