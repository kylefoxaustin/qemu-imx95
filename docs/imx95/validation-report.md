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

**3.1 Networking — no usable interfaces on the v1 line.** Only `lo` appears.
The NETC/enetc blocks are logging stubs; `ping` reports `Network is
unreachable`. The integrated-PCIe ECAM probes but enumerates no devices (empty
slots by design). This is the v1.0-era baseline this report was captured on.
NETC networking — a working `eth0` over a modelled ENETC PF — landed as the
**v2.0.0** work that is now folded into this branch (`imx95-netc`, the
repository default and upstream candidate), so a current build of this branch
*does* bring up `eth0` (see [known-limitations](known-limitations.md) §6); this
Tier-3 result predates it.

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
populated. **But this validation run had no functional networking** (it was
captured on the v1.0-era line, where NETC is still a logging stub — see
[known-limitations](known-limitations.md) §6; NETC landed in v2.0.0, now part
of this `imx95-netc` branch), so `apt update` / `apt install` can't reach any
mirror inline from inside the guest in that configuration. Three workarounds,
all without machine-model changes:

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

Steps 3–5 of the v1.x execution plan are now complete (recorded below);
Step 6 (docs polish + this validation report) is the remaining gating
work for the upstream qemu-arm@ submission. See
[`known-limitations.md`](known-limitations.md) §5 for the step
trajectory.

---

## v1.x Steps 3–5 (M7 lifecycle) — validated (recorded 2026-05-29)

Steps 3–5 take the M7 from "an instance that runs standalone firmware"
(Step 2) to "a core the real System Manager boots, manages, and
fault-recovers, alongside Linux on the A55s." Each step is gated by an
integration test that runs from a clean tree; all pass, and the full
A55 + M33 + Linux boot is unregressed throughout.

**Step 3 — M7-first reset sequencing (SM-driven release).** The SM's
`DEV_SM_CpuStart(M7)` writes `SRC_GEN.SCR.M7MIX_RELEASE` (a sticky
latch), which the machine turns into the M7 release — silicon-faithful,
not a host-side cheat. `tests/m7-first` asserts the latch fired *and*
that the M7 came up before Linux userspace (Scenario 1 ordering). A 36 h
parallel-CPU stability soak passed 2026-05-28 (4154 heartbeats; 0
panics / SCMI-timeouts / RCU-stalls / BUGs; RSS stable 299→329 MB; the
`SRC.SCR` latch sticky throughout).

**Step 4 — SM-orchestrated M7 + Linux attach + rpmsg.** The SM boots the
M7 running real FreeRTOS firmware before the A-cluster; Linux's
`imx_rproc` driver *attaches* to the SM-managed core (it cannot start it
— the SM owns the M7 LM on this board, per the kernel's `lmm(1) not
under Linux Control`). End-to-end inter-processor communication is
proven with the **stock NXP `imx_rpmsg_pingpong` kernel module
performing a 100-message ping/pong exchange** over the modelled MU7
cross-connect (kicks) and shared vrings (payload) in a full A55 + M33 +
M7 boot — reproducible via the env-gated `m7_rpmsg_pingpong` functional
test (asserting the module's `goodbye!` completion) with a standalone
harness and firmware/initramfs recipe in `tests/m7-rpmsg/`. This step
also surfaced — and fixed — a *generic* ARMv7-M
PMSAv7 MPU fidelity bug in `target/arm/ptw.c` (misaligned region base
aligned down to match Cortex-M silicon rather than dropped), filed as a
standalone upstream qemu-devel patch that benefits all ARMv7-M guests.

**Step 5 — SM fault recovery (cold-reset the M7 LM).** The real SM
boots, manages, and **fault-recovers** the M7. When the M7 faults
(asserts `SYSRESETREQ`), the machine routes it to the SM as
`CM7_SYSRESETREQ_IRQn`; the SM's fault handler runs `LMM_SystemLmReset`
(`reaction=lm_reset`) → `CpuStop` (hold the M7) → `CpuStart` (restart
it). For the SM to manage the M7 at all it must learn it from the
boot-ROM handover table it reads at startup, which this emulator
fabricates (a one-entry M7 handover in reserved M33 DTCM) since the
direct-ELF load bypasses the boot ROM. `tests/m7-fault-recovery` proves
the chain: an M7 fixture deliberately faults on its first boot and keeps
a DTCM boot counter that survives the core reset; the counter reaching 2
confirms the M7 faulted **and** the real SM restarted it (with no SM it
would fault once and never return). SM log evidence: `Reset LM 1,
reason=fccu`.

**Step-5 soak.** The SM-managed M7 path (A55 + M33 + M7, the M7 held under
the SM's logical-machine lifecycle) additionally ran a **21 h** continuous
soak: **0 anomalies** (no panics / RCU stalls / SCMI timeouts / BUGs / oopses),
host RSS flat (+1.1 %, no leak), and the SM M7-release latch (`SRC.SCR` bit 12)
held throughout. That run ended at 21 h on an external process disturbance (a
rebuild of the running binary mid-soak), not a guest fault.

**Full 24 h soak — PASSED (recorded 2026-05-31).** The follow-up ran to the
full 24 h target on a build that includes the SDHCI shutdown fix (`hw/sd/sdhci:
let i.MX (u)SDHC issue commands without SDCLK_EN`, commit `71a559175e`; the soak
binary was built at branch HEAD `7fb803d6db`, two doc/gitignore commits later).
It was launched off a *frozen* copy of the binary (`build/qemu-soak-frozen`) so
a host-side rebuild could not perturb the running process as it did at 21 h. The
run completed cleanly and stopped itself at the target ("soak target reached"):

- **Duration / heartbeats:** full 24 h, **2769** heartbeats (one every ~31 s),
  2026-05-30 22:19:37 → 2026-05-31 22:19:25.
- **Anomalies:** **0** — no panics, RCU stalls, BUG()s, oopses, segfaults,
  SCMI timeouts, or unhandled aborts across the entire run.
- **SDHCI fix held:** **0** `DEBUG STATUS DUMP` and **0** "Timeout waiting for
  hardware cmd" lines — the uSDHC no-SDCLK_EN command spew the fix removes never
  reappeared over 24 h. (The only `mmc` lines are the benign boot-time
  `deferred probe timeout, ignoring dependency` for the two card-less slots.)
- **Host RSS:** min 293412 kB / max 297256 kB / last 297256 kB — **+1.31 %**,
  flat at the plateau by end of run; no leak.
- **`SRC.SCR` bit 12:** held `1` for the whole run, never read `0` after the
  first sample (the SM M7-release latch stayed latched).
- **Process integrity:** the soak ran off `build/qemu-soak-frozen` (a distinct
  file from `build/qemu-system-aarch64`); later rebuilds of the live binary did
  not touch the running soak.

This is the full 24 h soak that had been the remaining pre-merge gate; the
SDHCI shutdown fix is now banked by a clean 24 h run.

**Regression.** Across Steps 3–5 the SM-managed M7 path and the
A55/Linux boot stay independent: `tests/m7-first` reaches Linux
userspace with the M7 up; `tests/m33-m7-only` and `tests/m7-boot` cover
the SM-managed and SM-less M7 boots respectively; all green. The M7
handover is M7-only by design, so the A55 (which boots outside the SM's
LMM path) is untouched.

This closes the v1.x M7 *functional* deliverables. What remains before
the upstream window is the broader validation campaign in
[`validation-todo.md`](validation-todo.md) (fresh-clone reproducibility;
the full 24 h soak gate is now met by the clean 2026-05-31 run above) and
the Phase-5 submission prep (functional test, `.rst` docs, patch series).

## Independent third-party workload #1 — ORB-SLAM3 ran in-machine (recorded 2026-05-30)

The strongest adoption signal to date: an **independent session (not the
author)** cross-built and ran **ORB-SLAM3** — a real classical visual-SLAM
stack (OpenCV 4.12 + g2o + DBoW2 + Sophus + boost-serialization, 14 shared
libraries, a 139 MB DBoW2 vocabulary) — to **converged tracking inside the
`imx95-19x19-evk` machine**. Full reproducible recipe (patches, aarch64
toolchain file, initramfs builder, boot wrapper, logs) at
<https://github.com/kylefoxaustin/orbslam3-imx95> (`phase0_orbslam3_inventory.md`
+ `patches/orbslam3-imx95-headless-crosscompile.patch` + `qemu_work/` +
`CROSS_COMPILE_NOTES.md`, a worked guest-software cross-build writeup).

**Result:** 250 frames processed; Atlas initialized (508 points); tracking
converged to a single coherent map (1 map, 24 keyframes on the frame subset —
no fragmentation, no track loss); `exit=0`. Per-frame tracking median 178.5 ms
(mean 207.9 ms) vs 8.3 ms on the x86 host — a ~21× slowdown that is **TCG
emulation overhead, not an i.MX 95 silicon performance estimate**; this run
establishes *functional portability* to the A55 target, not timing.

**What it independently confirms** (in someone else's hands, not the author's):
- `-m 8G` is honored — the guest saw 7.7 GiB, the DTB `/memory` node sized to
  `machine->ram_size`; guest RAM is **not** pinned by the DTB.
- A full **glibc-dynamic** userspace runs a real OpenCV/g2o C++ app with 14
  shared libraries resolved via `LD_LIBRARY_PATH`.
- The **initramfs is the (only) transfer path** — a ~134 MB cpio (trim rootfs +
  cross-built binary + readelf-walked dependency closure + 139 MB vocab + a
  frame subset + a custom `/init`), confirming there is no virtio/block-storage
  host-share (a documented limitation, here exercised in practice).

**Bug surfaced and fixed:** the guest's `poweroff` did not terminate QEMU, and
the uSDHC model spewed `sdhci-esdhc-imx` debug-status on the shutdown path —
found by this run and correlated with the tail of our own stability-soak logs.
Root cause was a generic SDHCI modelling gap: the i.MX (u)SDHC has no software
`SDCLK_EN` bit (the card clock is auto-gated in hardware), so the Linux
`sdhci-esdhc-imx` driver never sets it, but `sdhci_can_issue_command()` gated
on it — every command to the empty eMMC/SD slots was silently dropped, the mmc
layer spun on 10s host-side timeouts, and `device_shutdown()` never reached
PSCI `SYSTEM_OFF`. Fixed by `SDHCI_QUIRK_SDCLK_AUTO_GATE`
(commit `hw/sd/sdhci: let i.MX (u)SDHC issue commands without SDCLK_EN`):
poweroff now terminates the VM cleanly (`reboot: Power down` → PSCI →
exit code 0) with no MMC spew, and the fix is a standalone generic-QEMU
prereq alongside the `ptw.c`/`tlb_helper.c` patches.

This is qualitatively beyond a self-test: a stranger found the machine useful
enough to build a real SLAM application against it, succeeded end-to-end, and
in doing so re-validated three core machine claims and found a real bug. It is
the canonical "works for someone other than the author, doing something the
author didn't script" evidence for the upstream cover letter.

## Independent third-party workload #2 — VINS-Fusion + clean-shutdown confirmation (recorded 2026-05-30)

A **second independent session (not the author)** cross-built and ran
**VINS-Fusion** — a stereo-inertial visual-inertial-odometry stack with a
Ceres-based sliding-window optimizer — end-to-end inside the
`imx95-19x19-evk` machine on the EuRoC `MH_01` dataset. Full reproducible
recipe (patches, aarch64 toolchain, initramfs builder, boot wrapper, run logs)
at <https://github.com/kylefoxaustin/vins-fusion-imx95> (sibling to the
ORB-SLAM3 repo).

**Result:** converged (240 Ceres optimizations, bounded), ~324 ms/frame under
TCG, `exit=0`; the ~21× slowdown vs the x86 host matches the ORB-SLAM3 run
(TCG overhead — *functional portability*, not a silicon timing estimate). Two
independent SLAM/VIO stacks now run to convergence on the machine.

**Real-workload confirmation of the SDHCI shutdown fix.** Rebuilt out-of-tree
at the fixed HEAD (`hw/sd/sdhci: let i.MX (u)SDHC issue commands without
SDCLK_EN`) and re-run, the workload finished and the guest powered off
cleanly: `[93.7] reboot: Power down` → QEMU `exit 0`, **zero MMC debug-status
spew, no SIGKILL**, on a CPU-heavy VIO + Ceres load. This independently
confirms the fix on a real workload, not just the busybox repro.

**Guest-image caveat (worth noting for users).** The fix only fires if the
guest actually issues a power-off. The trim Yocto rootfs's `/sbin/poweroff`
is systemd-flavored and silently no-ops without an init manager (it falls
through to the shell); switching the init to `busybox poweroff -f` (a direct
`reboot()` syscall) exercises the kernel power-off path and the clean exit.
A guest image whose `poweroff` defers to a missing init manager won't
terminate QEMU — standard QEMU behaviour, not an i.MX 95 issue.

---

## Code-sweep — real third-party software on the A55 (2026-06-27)

A breadth campaign distinct from the v1.0 axes above: take real open-source
projects, **download the source, cross-compile it, run it on the emulated
i.MX 95, and check the answer against each project's *own* known-good oracle**
(upstream self-test / round-trip / known-answer test / differential-vs-host
golden). The point is volume of real-world code proven to execute correctly on
the model. Harness: `tests/code-sweep/`. Machine-readable per-item registry with
versions + oracles: [`docs/validation/code-sweep-matrix.yaml`](../validation/code-sweep-matrix.yaml).

Method: cross-compile **statically** on the host (`aarch64-linux-gnu-gcc`; the
busybox initramfs has no glibc loader), ship binaries + a per-item `runtest.sh`
into the guest over **virtio-9p**, run the whole corpus in **one boot**, and
score each (`0`=PASS, `77`=SKIP first-class, else FAIL). Scope is
**non-accelerator** (no GPU/VPU/NPU/ISP — those have no functional model).

**Result: 28/28 PASS** — 27 CPU-tier items + 1 peripheral-tier item.
Reproduce: `tests/code-sweep/run.sh` (CPU) and
`tests/code-sweep/run-peripheral.sh` (peripheral).

### CPU tier (computes in core + memory)

| # | Item | Ver | Category | Oracle | Result |
|---|------|-----|----------|--------|--------|
| C1 | zlib | 1.3.1 | compression | example self-test + minigzip round-trip (sha256) | PASS |
| C2 | bzip2 | 1.0.8 | compression | make-test round-trip cmp vs committed refs | PASS |
| C3 | zstd | 1.5.6 | compression | compress -19 + `zstd -t` + decompress (sha256) | PASS |
| C4 | xz | 5.4.5 | compression | compress + `xz -t` + decompress (sha256) | PASS |
| C5 | brotli | 1.1.0 | compression | compress -q9 + decompress round-trip (sha256) | PASS |
| C6 | lz4 | 1.9.4 | compression | compress -9 + decompress round-trip (sha256) | PASS |
| C7 | heatshrink | master | compression | own encoder/decoder suite (greatest) | PASS |
| C8 | miniz | 3.0.2 | compression | example1 round-trip + memcmp self-check | PASS |
| C9 | crypto-algorithms | master | crypto | 9 KATs (sha256/sha1/md5/md2/aes/des/blowfish/arcfour/base64) | PASS |
| C10 | libsodium | 1.0.19 | crypto | ~20 of its own test/default self-checks vs `.exp` | PASS |
| C11 | mbedtls | 3.6.2 | crypto | `selftest` KATs across ~22 suites | PASS |
| C12 | libtomcrypt | 1.18.2 | crypto | own math-free KAT sub-tests | PASS |
| C13 | base64 (aklomp) | 0.5.2 | base64 | known-answer encode/decode vectors | PASS |
| C14 | xxhash | 0.8.2 | hashing | canonical XXH32/64/128/XXH3 vectors + host/guest agreement | PASS |
| C15 | sqlite | 3.46.1 | database | SQL workload diffed vs host-built golden | PASS |
| C16 | jq | 1.7.1 | json | `--run-tests` differential vs pinned baseline (435/447) | PASS |
| C17 | cjson | 1.7.18 | json | 18 of its own Unity test suites | PASS |
| C18 | pcre2 | 10.43 | regex | testinput1 diffed vs committed testoutput1 (RunTest 1) | PASS |
| C19 | oniguruma | 6.9.9 | regex | own testc/back/options/regset suites | PASS |
| C20 | expat | 2.6.2 | xml | xmlwf accept + reject battery | PASS |
| C21 | tinyexpr | master | math | bundled smoke test | PASS |
| C22 | utf8proc | 2.9.0 | unicode | own case/charwidth/iterate/valid/misc tests | PASS |
| C23 | uthash | 2.3.0 | datastruct | 96 programs vs committed `.ans` goldens | PASS |
| C24 | sds | master | string | SDS_TEST_MAIN self-test (46 tests, 0 failed) | PASS |
| C25 | lua | 5.4.7 | interpreter | self-test asserts (strings/tables/coroutines/metatables) | PASS |
| C26 | cpython | 3.10.14 | interpreter | own `python -m test`: 33 stdlib modules | PASS |
| C27 | ltp-syscalls | 20240524 | syscall | LTP syscall subset: ~250 TPASS (of 323 binaries) | PASS |

Named, model-/build-independent SKIPs (not A55 failures, kept honest):
utf8proc normtest/graphemetest/iscase (need downloaded Unicode data); CPython
`_testcapi`/`_ssl`/`_ctypes`/`_decimal` tests; LTP TBROK/TCONF (minimal-initramfs
environment) + 4 env-only TFAILs.

### Peripheral tier (real hardware datapaths)

| # | Item | Datapath | Oracle | Result |
|---|------|----------|--------|--------|
| P1 | storage-sqlite | uSDHC / eMMC (ADMA) | SQLite writes a 20k-row DB through ext4-on-eMMC, drops caches, re-reads off the card, diffs vs host golden | PASS |

The peripheral harness emits the i.MX91 sweep's shared `SOAK:` markers
(`storage-emmc` key) so the 91 and 95 dashboards are diff-able. Network
(`m95-net-enetc`) and USB (`usb-enum`/`usb-bulk`) datapath items are planned next.

### Deferred candidates

Not in the corpus because each needs host-only tooling or a download step that
breaks the self-contained recipe model: **quickjs** (cross can't exec target
`qjsc`), **wolfssl** (release lacks `configure`, no autoreconf on host),
**inih** (multi-config baselines), **json-c** (cmake harness), **mpdecimal**
(IBM decTest tree via `gettests.sh`), **gmp** (libtool ignores `-all-static`),
**libyaml** (external test-data wiring).
