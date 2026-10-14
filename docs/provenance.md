# Verification record — licence, derivation and validation checks

A record of the checks run on this work and what they returned. It states results, methods and
limits, and nothing else.

**Revision covered.** Upstream-candidate series at commit `3d82d068f32` (branch
`imx95-v2-compliance`), 21 added files and 18 modified pre-existing upstream files, based on
upstream QEMU `origin/master`. Results below describe that series unless a section states
otherwise. Section 8 records what is known about the wider fork. Later revisions are not covered.

---

## 1. How the code was produced

The implementation was produced by the maintainer working with an AI coding assistant
(Anthropic's Claude) over a series of sessions.

- Hardware behaviour was specified from the **NXP i.MX 95 Reference Manual** and NXP's published
  BSP — device trees, Linux drivers, U-Boot and System Manager firmware — used as reference to
  determine register-level behaviour.
- The assistant generated the majority of the device-model implementation code.
- Scope, architecture and fidelity decisions, hardware bring-up, test design and debugging were
  directed by the maintainer, who reviewed, built, executed and validated the result on emulation
  and on physical hardware.

## 2. Licence headers

| check | result |
|---|---|
| added files carrying `SPDX-License-Identifier: GPL-2.0-or-later` | 21 of 21 |
| added files carrying a copyright notice | 19 of 21 |
| added files carrying a **third-party** copyright notice | 0 |

The two files without a copyright notice are `docs/system/arm/imx95-evk.rst` and
`tests/functional/aarch64/test_imx95_evk.py`. Upstream peer convention: 0 of 2 peer
`test_*_evk.py` files carry a copyright line, and 2 of 38 `docs/system/arm/*.rst` files carry
an SPDX tag.

Per-file detail: [`oss-compliance-manifest.csv`](oss-compliance-manifest.csv).

## 3. REUSE lint

Tool: `reuse` 6.2.0, scoped with `lint-file` to the added files. Findings: 21 ×
`missing license 'GPL-2.0-or-later'`, 2 × `no copyright notice`.

`missing license` reports that the licence *text* is not at `LICENSES/GPL-2.0-or-later.txt`.
QEMU carries licence text in `COPYING`, `COPYING.LIB` and `LICENSE` and has no `LICENSES/`
directory, so this result is a property of the project layout and is returned for every file.

## 4. Static checks

`scripts/checkpatch.pl --no-signoff --branch`: **0 errors on all 13 patches**. 11 warnings, all
`added, moved or deleted file(s), does MAINTAINERS need updating?`; the series updates
`MAINTAINERS`. Commits carry `Signed-off-by` and no other trailers.

## 5. Derivation (verbatim overlap) check

Method: comments, blank lines and lines shorter than 12 characters are removed and whitespace
normalized; overlapping windows of K significant lines are hashed; any collision between the
series and a reference corpus is reported. Script:
[`oss-compliance-derivcheck.py`](oss-compliance-derivcheck.py) (K, repository, base revision and
corpora are command-line arguments; K defaults to 8).

Scope: the 19 added `.c`/`.h` files, 2,213 distinct shingles at K=8.

| corpus | files scanned | matches @ K=8 | matches @ K=5 |
|---|---|---|---|
| this repository (`hw/`, `include/hw/`, `target/arm/`) | 2,751 | 0 | 12 |
| NXP `linux-imx` `drivers/` | 32,803 | 0 | 0 |
| NXP `uboot-imx` `drivers/` | 3,032 | 0 | 0 |
| NXP `imx-qemu` `hw/` | 1,587 | 0 | 3 |

The 15 matches at K=5 were inspected. They are the `MemoryRegionOps` field sequence used by many
in-tree QEMU devices; the five lines matched between `hw/misc/imx95_wdog.c` and
`hw/alpha/pci.c` are:

```
.endianness = DEVICE_LITTLE_ENDIAN,
.min_access_size = 4,
.max_access_size = 4,
```

The same 11 files match `hw/alpha/pci.c`, `hw/i386/intel_iommu.c` and `hw/gpio/cadence_gpio.c`,
which are unrelated hardware. `hw/arm/fsl-imx8mm.c` and `fsl-imx8mp.c` also match the SoC
container file in this series, which uses the same container idiom.

**Limits of this check.** K=8 significant lines is one threshold, not any particular vendor
tool's algorithm. The method detects reformatted copies; it does not detect restructured or
renamed-variable derivation. Four corpora are not a commercial KnowledgeBase. The corpus
revisions were not recorded at the time of the run and should be captured alongside any result
relied upon. Register names, offsets and bit definitions are taken from the public Reference
Manual and will resemble any other implementation of the same hardware, including NXP's own
drivers.

## 6. Dependencies

The series adds no external dependencies, no vendored third-party source, and no firmware or
binary files. Build-system changes add only its own files behind `CONFIG_*` gates. See section 8
for the wider fork.

## 7. Validation record

Recorded in-repo. Classifications below follow the source documents rather than summarising them.

**Against physical silicon.** `docs/validation/silicon-vs-qemu.md` records nine numbered issues
found and fixed. They are not all of one kind: four are model defects found by diffing registers
against a physical board (items 1–4); one is a missing device subsequently modelled (item 6); two
are latent model bugs (items 7 and 8), of which item 8's entry states that silicon was not what
found it; and two are faults in the project's own test harness (items 5 and 9) — item 5 being a
test gate that had never run. The same document records eight divergences deliberately **not**
changed, with reasons.

**The board diffed against was an FRDM-IMX95-PRO** (BSP LF6.18.20, kernel 6.18.20-2.0.0); the
machine modelled is the **19x19 EVK**. Same SoC, different board; the source document states this
and the constraint it implies.

**Boot.** `tests/functional/aarch64/test_imx95_evk.py` boots a stock NXP BSP Linux kernel to a
userspace shell on 6 Cortex-A55 cores with NXP's System Manager firmware running on the
Cortex-M33, and exercises the Cortex-M7 via rpmsg and via fault recovery. The kernel binary is
unmodified; the board applies device-tree fix-ups at machine init and some harnesses patch the
DTB before boot.

**Third-party software.** `tests/code-sweep/` cross-compiles 36 programs plus 8 peripheral
recipes and runs them in-guest against each program's own expected output
(`docs/validation/code-sweep-matrix.yaml`). `tests/in-guest-build/` builds and runs C and C++
natively inside the guest with `tcc`, `gcc` and `clang`, including upstream projects passing
their own test suites. `tests/full-stack/` is a single-boot coexistence gate.

An ORB-SLAM3 run to converged tracking is recorded in `docs/imx95/validation-report.md`; that
run was performed by a separate session and its recipe and logs are in an external repository,
so it is not reproducible from this tree.

## 8. Wider fork

Two items outside the series were identified and resolved:

1. `tests/netc/netc-load-initramfs.cpio.gz` — a 3.1 MB prebuilt test fixture containing
   third-party binaries (`busybox`, glibc, `libstdc++`, `libgcc_s`, `iperf`) with no accompanying
   licence or source notice — has been removed from the tree. Both consumers
   (`tests/netc/load-soak.sh`, `load-soak-2port.sh`) build it on demand via
   `tests/netc/build-load-initramfs.sh`, which fetches the pinned upstream packages at build
   time. `tests/netc/README.md` records those packages, their versions and their licences.
2. Seven added `.c`/`.h` files on the development branch carried no SPDX tag
   (`hw/net/imx95_netc_timer.c` and six fixtures under `tests/in-guest-build/`). All seven now
   carry `SPDX-License-Identifier: GPL-2.0-or-later`.

With those resolved, every added `.c`/`.h` file on the development branch carries an SPDX tag,
and the fork adds no compiled binaries. The image files it tracks are screenshots and test
fixtures under `docs/images/` and `tests/` (PNG/JPEG); the compiled binaries in the tree are
upstream QEMU's own (`pc-bios/*`, `linux-user/*/vdso*.so`, `tests/data/acpi/*`).

## 9. Reproduction

Method, tool invocations, how to read each tool's output, and the pitfalls that produced wrong
results during this pass are in [`oss-compliance-log.md`](oss-compliance-log.md).
