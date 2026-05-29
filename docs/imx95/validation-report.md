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
| 1.4 | 24 h stability run | PASS (ran to ~36 h, recorded below 2026-05-23) |
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

**1.4 24 h stability — PASS.** Detached run, heartbeat every ~30 s
recording `MemAvailable`, an md5 over a 4 MiB scratch file, and the current
A55 cpufreq. The run completed cleanly, going past its 24 h target to
~36 h — full final-state analysis is recorded below under
["v1.0 stability soak — PASSED (recorded 2026-05-23)"](#v10-stability-soak--passed-recorded-2026-05-23):
MemAvailable stable (no leak), md5 stable across all 4274 heartbeats
(data integrity), 0 panics/SCMI-timeouts/RCU/BUGs.

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

---

## Trajectory: upstream submission gated on v1.x (Cortex-M7 integration)

v1.0 represents the M33 + Linux integration story on the imx95-19x19-evk
machine, validated above against three kernels, two userspaces, and a 24h+
stability run. **Upstream submission to qemu-arm@ is planned for v1.x, after
the Cortex-M7 real-time core is integrated**, so the artifact submitted to
the QEMU community represents the full i.MX 95 CPU complement (6× A55 + 1×
M33 + 1× M7) honestly rather than a partial SoC. This is a deliberate scope
choice: shipping a 7-CPU model when the SoC has 8 would invite immediate
"why isn't the M7 modelled?" review feedback, and the right answer is to
model it before submitting.

The validation work documented in this report applies to v1.0. v1.x will
have its own validation pass focused on M7 integration before the
upstream-submission window opens. The v1.x M7 scope is listed in
[`docs/imx95/known-limitations.md`](known-limitations.md) §5.


---

## v1.0 stability soak — PASSED (recorded 2026-05-23)

The v1.0 24 h+ stability run completed cleanly past its 36 h target,
gating-stop scheduled by an unattended watcher that snapshotted the
final state and SIGTERMed the qemu instance. Recorded final state:

- **Heartbeats:** 4274 (one every ~31 s; 24 h ≈ 2800, 36 h ≈ 4200)
- **Guest uptime at stop:** ~36 h 4 min
- **MemAvailable trend:** 1795224 kB → 1860048 kB (no leak)
- **Data-integrity heartbeats with bad md5:** 0
- **Anomaly counts:** panics=0, SCMI timeouts=0, RCU stalls=0, BUGs=0
- **Host RSS over the run:** min=372632 kB / max=420244 kB / avg=412654 kB / samples=213 / spread=11.5%

No panics, no SCMI timeouts, no RCU stalls, no BUGs. Memory remained
stable across the entire run; data integrity held across every
heartbeat; host RSS stayed within the ±10 % campaign target. The v1.0
artifact has passed its long-duration stability gate.

The v1.x Step 2 soak (A55 + M33 + M7 all active, 36 h target) runs
next under a parallel watcher.

---

## First-customer ecosystem readiness check (post-v1.0)

Shortly after v1.0 was tagged, a parallel project codenamed *Ratchet*
(an independent drone-emulator effort) began evaluating qemu-imx95 v1.0
as an aarch64 Linux-userspace install target for an ORB-SLAM3-class
SLAM build. The exchange surfaced enough about how the artifact looks
to a non-author user that the findings are worth recording here as
evidence-of-fit alongside the campaign's own results.

### Methodology surfaced during the exchange

The most efficient way to characterize the guest environment turned
out to be a clean split:

- **System facts** (kernel banner, `/proc/cmdline`, `/proc/meminfo`,
  `/proc/cpuinfo`) — captured by a live guest boot. These depend on
  the machine model and the boot recipe, not the rootfs choice.
- **Userspace facts** (`/etc/os-release`, toolchain presence,
  `/usr/include` headers, `/usr/lib` runtime libraries) — captured by
  file-inspecting an extracted rootfs on the host. The rootfs is just
  files; "is gcc present" is a `stat()`, not an `exec()`. Faster than
  booting, contamination-free (no risk of accidental in-guest
  installs), and gives the same answer either way.

This is worth memorializing as a recurring shape: anyone characterizing
"what does qemu-imx95 look like to a downstream user" for a given
rootfs should use the same split.

### qemu-imx95 is a machine model, not a distribution

The exchange surfaced a misconception worth clearing up explicitly in
the report. **qemu-imx95 doesn't bundle a userspace.** What runs in the
guest is whatever rootfs / initramfs the user boots with. The project
ships pointers to two:

- The static-BusyBox initramfs (`tests/busybox-initramfs/build.sh`) —
  minimal; useful for proving Linux comes up.
- The NXP Yocto BSP `imx-image-full-imx95evk` rootfs from NXP's Linux
  Factory release zip (`LF_v6.12.49-2.2.0_images_IMX95.zip`) — the
  canonical "what an NXP-BSP-based downstream developer would have."

For sub-phase 0.1 of Ratchet's ORB-SLAM3 build attempt, the second is
the relevant target. Its `/etc/os-release` confirms it as
`ID=fsl-imx-xwayland`, `VERSION_ID=6.12-walnascar` — NXP's Yocto
walnascar release of the i.MX 95 reference distro.

### Native toolchain confirmed in the BSP rootfs

A real question for downstream users is whether the NXP `imx-image-full`
image is a *dev-class* image with a native toolchain or a *runtime-only*
image that assumes cross-compile from a Yocto SDK on the host. The
question matters because the answer changes the user's workflow
entirely.

The image **is dev-class with a native aarch64 toolchain**. Verified by
reading the ELF `e_machine` field of each binary (`0xB7` = EM_AARCH64):

| Binary | Resolved path in rootfs | Arch |
| --- | --- | --- |
| `gcc` | `/usr/bin/aarch64-poky-linux-gcc` | aarch64-ELF |
| `g++` | `/usr/bin/aarch64-poky-linux-g++` | aarch64-ELF |
| `cmake` | `/usr/bin/cmake` | aarch64-ELF |
| `make` | `/usr/bin/make` | aarch64-ELF |
| `pkg-config` | `/usr/bin/pkg-config` | aarch64-ELF |
| `ld` | `/usr/bin/aarch64-poky-linux-ld` | aarch64-ELF |

Plus the glibc dynamic loader and `libc.so.6`. The Poky-cross-target
binary name is just NXP's symlink convention; the binaries themselves
are aarch64 ELFs and run natively on the guest A55s. Conclusion:
**in-guest native build of large C++ projects (incl. ORB-SLAM3-class)
is on the table** without a cross-compile path.

### SLAM dep-chain coverage in the stock BSP rootfs

The same image was probed for the standard SLAM/CV dep chain. The
runtime / header split is the conventional Yocto one — runtime libs go
in the base image, dev headers go in `-dev` packages that the base
image doesn't pull in.

**Runtime libraries present** (in `/usr/lib`):

- **OpenCV 4.12.0** — full 104-`.so` stack including the SLAM-relevant
  modules: `features2d`, `xfeatures2d`, `flann`, `calib3d`, `stereo`,
  `rgbd`, `tracking`, `aruco`, `dnn`, `optflow`, `videoio`, `highgui`,
  `imgproc`, `imgcodecs`, `core`, plus another ~25 modules
- **Boost 1.87** — `libboost_system.so.1.87.0` and
  `libboost_filesystem.so.1.87.0` only (other Boost components absent)
- **GL stack** — `libGL.so.1.2.0`, `libGLESv2.so.2.1.0`,
  `libGLESv1_CM.so.1.1.0`, `libEGL.so.1.5.0`, `libvulkan.so.*`,
  `libdrm*` (full driver-hook set)

**Dev headers absent** from the base image:

- No `eigen3/`, no `opencv2/`/`opencv4/`, no `pangolin/`, no `sophus/`,
  no Boost include tree
- No `.pc` files for any of the above in `/usr/lib/pkgconfig/`
- `/usr/include/GL`, `/usr/include/EGL`, `/usr/include/drm`,
  `/usr/include/CL` (OpenCL), and `/usr/include/c++` (libstdc++) **are**
  present — so anything linking purely against the GL stack and the
  C++ stdlib will compile in-guest as-is

The dep-header gap is fixable without recipe authoring: the NXP
`imx-yocto-bsp` source tree on the development host has matching
recipes for everything in the runtime list —
`meta-imx/meta-imx-bsp/recipes-support/opencv/opencv_4.12.0.imx.bb`
(the exact runtime variant in the rootfs),
`meta-openembedded/meta-oe/recipes-support/libeigen/libeigen_3.4.0.bb`,
`poky/meta/recipes-support/boost/boost_1.87.0.bb`. So an
`IMAGE_INSTALL:append = " opencv-dev libeigen boost-dev"` to
`imx-image-full.bb` and a `bitbake` rebuild produces a rootfs in which
in-guest native build is fully self-contained. (Pangolin and Sophus
have no recipes locally and would need fresh `.bb` files — Sophus is
header-only, Pangolin is a CMake project.)

### Apt is present, but `apt install` doesn't work in v1.0

`/usr/bin/apt` is in the rootfs and `/etc/apt/sources.list.d/` is
populated. **But qemu-imx95 v1.0 has no functional networking** (NETC
is a logging stub — see [known-limitations](known-limitations.md) §6),
so `apt update` / `apt install` can't reach any mirror inline from
inside the guest. Three workarounds, all without machine-model
changes:

1. Pre-bake the needed packages into the rootfs via the Yocto image
   recipe (`IMAGE_INSTALL:append`), as above. The cleanest long-term
   posture for downstream users.
2. Populate the rootfs's apt cache on the host (`apt download` from an
   x86 box configured against NXP's repos, copy `.deb` files into
   `var/cache/apt/archives/`), then `apt install -y --no-download
   <pkg>` from inside the guest.
3. Cross-compile from a Yocto SDK on the host (`bitbake … -c
   populate_sdk` produces the standard NXP toolchain installer) and
   bind-mount the result into the rootfs at runtime.

This isn't a new limitation — it's a downstream-visible consequence of
the existing §3.1 networking gap. The known-limitations entry has been
extended to call it out so future downstream users don't bounce off it.

### Compile-time RAM headroom

`/proc/meminfo` from a live boot at the default `-m 2G`:

```
MemTotal:      2006068 kB
MemFree:       1812908 kB
MemAvailable:  1781104 kB
SwapTotal:           0 kB
```

~1.78 GiB available before any workload. The machine has no hard
`-m` cap; for an ORB-SLAM3-class C++ template build, `-m 8G` is the
recommended starting point. On a development host with enough RAM,
this is a one-flag change.

### What this contributes to the validation evidence base

Three things worth naming:

1. **First non-author user use case.** Until this exchange, all
   validation evidence in this report came from the author's own runs.
   Ratchet's recon is the first "the artifact is being looked at by
   someone who hasn't built it" data point. It confirms the v1.0
   machine is, in fact, usable as a Linux-userspace install target by
   a downstream party — the canonical NXP image boots on it, the
   native toolchain inside that image is real, and the SLAM dep chain
   is partially available with a documented path to completion.
2. **Methodology recurrence.** The host-side-rootfs-inspection vs.
   live-guest-boot split is the right shape for any future
   "characterize the guest environment for downstream X" exercise.
   Reusing it for follow-up integrations (ROS, MAVLink, Vulkan
   benchmarks, …) avoids the temptation to boot-and-poke around
   inside the guest, which is slower and risks accidental state
   contamination.
3. **New known-limitation surfaced.** The apt-but-no-network reality
   is a real downstream papercut and is now documented in
   [`known-limitations.md`](known-limitations.md) §6 rather than left
   as an in-band discovery for the next user.

The v1.x M7 work and the upstream-submission timing are unaffected by
this. This section records evidence that the v1.0 artifact has
real-world fit for at least one downstream use case, alongside the
campaign's own tier results.

---

## v1.x Step 2 stability soak — PASSED (recorded 2026-05-26)

The 36 h parallel-CPU stability soak — A55 cluster + M33 (real NXP SM
firmware) + M7 (cm7-hello fingerprint firmware) all active throughout —
completed cleanly at its auto-stop watcher, mirroring the v1.0 soak's
pattern. This is the explicit Step 2 completion gate per the v1.x
execution plan: "24 h soak run with all three CPU complements active
passes."

Recorded final state at the 36 h auto-stop (2026-05-25 07:26 CDT):

- **Heartbeats:** 4260 (36 h target ≈ 4200; ~31 min of margin)
- **Guest uptime at stop:** ~36 h 1 min (129666 s)
- **MemAvailable trend:** 1804988 kB → 1866824 kB (no leak; slight
  uptrend across the run, mirroring the v1.0 soak's pattern)
- **Data-integrity heartbeats with bad md5:** 0
- **Anomaly counts:** panics=0, SCMI timeouts=0, RCU stalls=0, BUGs=0
- **Boot recipe:** `tests/parallel-boot/run.sh` invocation pattern —
  full NXP 6.12.49 kernel + DTB + busybox initramfs +
  `-device loader,...,m33_image.elf,cpu-num=6` +
  `-device loader,...,cm7_image.elf,cpu-num=7`. Same daemonized setup
  as the v1.0 soak, plus the M7 firmware.

No anomalies of any class across 4260 heartbeats. The M7 instance
(added in commit `867aa98550`), its standalone hello firmware
(`tests/cm7-hello`, commit `c649249025`, with the DTCM-fingerprint fix
`636f561896`), the unit test (`tests/m7-boot`, `e6b138636d`), and the
integration test (`tests/parallel-boot` + `tests/run-all`, `90f73ba1fc`)
all co-exist with the v1.0 A55+M33+Linux boot path without disturbing
it. The default reset policy (machine-init-done BH releases each
heterogeneous core whose ITCM has been populated by `-device loader`)
held across the full run.

This closes v1.x Step 2's deliverables: M7 CPU instance modelled,
standalone firmware loadable, parallel boot validated, 36 h soak
passed with zero anomalies, v1.0 regression intact.

Steps 3–6 of the v1.x execution plan (M7-first / A-first reset
sequencing scenarios, SCMI `CPU_ON` gating + Linux `imx-rproc`
integration, lifecycle cycling, and the v1.x validation report) remain
the gating work for the upstream qemu-arm@ submission. See
[`known-limitations.md`](known-limitations.md) §5 for the step
trajectory.
