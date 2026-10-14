# OSS compliance pass — procedure and findings

Method and results for the licence/provenance review of the i.MX 95 upstream-candidate series.
Written so it can be re-run against the sibling emulator forks in this family and produce
comparable artifacts.

**Scope.** Unless stated otherwise, every count below describes the **upstream-candidate series**
— the 21 files it adds and the 18 pre-existing upstream files it modifies — not the whole fork.
Section 7 covers what is known about the wider tree.

---

## 1. Software-composition analysis (Black Duck and equivalents)

Black Duck is a commercial product: a scan needs a hosted Hub server, an API token and a seat
licence. The `synopsys-detect` client is downloadable but does nothing without a server to post
results to. No such tooling is present on this machine (`detect`, `detect.sh`,
`synopsys-detect`, `bd-scan`, `scan.cli.sh` all absent; no `BD_*`/`DETECT_*` configuration), so
a scan is necessarily run by whoever holds the licence.

**What SCA does and does not cover.** Black Duck identifies **third-party open-source
components** — their licences, their known vulnerabilities, and snippet-level matches against its
KnowledgeBase. **It does not detect AI-generated code.** A clean report establishes that no
third-party code was incorporated; it says nothing about authorship. The provenance question is
addressed separately in [`provenance.md`](provenance.md).

**Scoping recommendation.** Do not scan the whole tree. QEMU is roughly 10 million lines and
resolves as a single known component. Scope the scan to the files a series **adds and
modifies** — here 39 files. That also avoids uploading an entire upstream project to a hosted
scanning service.

## 2. Local tooling: `reuse`

`reuse` 6.2.0 (FSFE REUSE linter) is available and provides `lint-file` for per-file scoping and
`spdx` for an SBOM.

```sh
mapfile -t FILES < <(git diff --name-only --diff-filter=A origin/master..HEAD)
reuse lint-file "${FILES[@]}"
```

Use `lint-file`, not a bare `reuse lint`. QEMU upstream is not REUSE-compliant, so a
whole-project lint returns thousands of findings about files the series does not own.

## 3. Reading `reuse` output

The three messages mean different things and only two can be a defect in the files under review:

| message | meaning | action |
|---|---|---|
| `no license identifier` | the file carries **no `SPDX-License-Identifier` tag** | **fix the file** |
| `no copyright notice` | no `Copyright …` line | **compare to upstream peers first** |
| `missing license 'X'` | the tag **is present**; the licence *text* `LICENSES/X.txt` is absent | **not a per-file defect** |

`missing license` is a project-structure result, not a file result. QEMU carries licence text in
`COPYING`, `COPYING.LIB` and `LICENSE` and has no `LICENSES/` directory, which is what REUSE
expects. Adding one to QEMU would be inappropriate and would not be accepted upstream. Expect
one such line per file reviewed, and disregard all of them.

## 4. Findings — upstream-candidate series

Baseline over the 21 added files: 1 × `no license identifier`, 2 × `no copyright notice`,
20 × `missing license`.

| file | finding | upstream peer convention | resolution |
|---|---|---|---|
| `docs/system/arm/imx95-evk.rst` | no SPDX, no copyright | **2 of 38** `docs/system/arm/*.rst` carry SPDX (`max78000.rst`, `vmapple.rst`) | **fixed** — added `.. SPDX-License-Identifier: GPL-2.0-or-later`, matching those two |
| `tests/functional/aarch64/test_imx95_evk.py` | no copyright | **0 of 2** peer `test_*_evk.py` carry a copyright line; all carry SPDX, as does this one | **no change — already conformant** |
| 19 × `.c`/`.h` | `missing license` only | n/a | **no change** — see section 3 |

**Result:** `checkpatch.pl` reports **0 errors on all 13 patches**. The 11 remaining warnings are
`added, moved or deleted file(s), does MAINTAINERS need updating?`, which are expected because
the series does update `MAINTAINERS`. Commits carry `Signed-off-by` and no other trailers.

## 5. Pitfalls observed during this pass

Recorded because each one produced a wrong answer that looked like a right one.

1. **Do not window a licence header.** Reading the first N lines of a file to check for an SPDX
   tag will report a false absence when the tag sits below the cut — in this series one tag is at
   line 14. Grep the whole file.
2. **`missing license` is not a missing tag.** Taken at face value it implies 19 correct files
   need editing. See section 3.
3. **`checkpatch.pl --branch` reads commits, not the working tree.** A fix that has not been
   committed is invisible to it, so the run grades the unmodified artifact. Amend first, then
   re-check.
4. **Check the upstream peer convention before changing anything.** `docs/` and
   `tests/functional/` have different norms; 36 of 38 board documents carry no SPDX at all. A
   non-finding supported by a peer count is as useful as a fix.
5. **`checkpatch.pl` option names change.** The `-g` (single commit) form was removed upstream in
   favour of `--branch`; an invalid invocation prints help text, which is easy to mistake for a
   clean result.

## 6. Derivation (snippet) check

Snippet matching is the SCA finding that requires human triage, so the added `.c`/`.h` files were
checked for verbatim overlap against the corpora that are the plausible sources for device models
written against this hardware — including the GPL kernel and bootloader drivers for the same SoC.

Script: [`oss-compliance-derivcheck.py`](oss-compliance-derivcheck.py). It normalizes whitespace,
removes comments, blank and very short lines, hashes overlapping windows of K significant lines,
and reports any collision. `K`, the repository, the base revision and the corpora are all command
line arguments; the default `K=8` reproduces the table below.

Scope: 19 added `.c`/`.h` files, 2,213 distinct shingles at K=8.

| corpus | files scanned | matches @ K=8 | matches @ K=5 |
|---|---|---|---|
| this repository (`hw/`, `include/hw/`, `target/arm/`) | 2,751 | **0** | 12 (boilerplate, below) |
| NXP `linux-imx` `drivers/` | 32,803 | **0** | **0** |
| NXP `uboot-imx` `drivers/` | 3,032 | **0** | **0** |
| NXP `imx-qemu` `hw/` | 1,587 | **0** | 3 (same boilerplate) |

`linux-imx` and `uboot-imx` are zero at both thresholds. That is the result most relevant to a
vendor review: GPL kernel and bootloader driver code is the plausible contamination path for
models of the same silicon, and there is no verbatim overlap.

**The K=5 matches are QEMU framework boilerplate**, established by inspecting the matched block
rather than inferred. Between `hw/misc/imx95_wdog.c` and `hw/alpha/pci.c` the five lines are:

```
.endianness = DEVICE_LITTLE_ENDIAN,
.min_access_size = 4,
.max_access_size = 4,
```

— the `MemoryRegionOps` field sequence used by hundreds of in-tree devices. The pattern confirms
it: the same 11 files match `hw/alpha/pci.c`, `hw/i386/intel_iommu.c` and `hw/gpio/cadence_gpio.c`,
which are unrelated hardware. Shared idiom, not shared lineage. The one expected resemblance is
`hw/arm/fsl-imx8mm.c` / `fsl-imx8mp.c` against the SoC container in this series: the same
container idiom within the same SoC family, which is conventional.

**Stated limits.** K=8 significant lines is a defensible snippet threshold but is not any
particular vendor's algorithm. The method catches reformatted copies; it does not detect
restructured or renamed-variable derivation. Four corpora are not a KnowledgeBase spanning
millions of projects. The corpus revisions are not recorded in this document and should be
captured alongside any result that is relied upon. The claim this method supports is *no
substantial verbatim block was found against these sources at this threshold* — not that the code
is provably original.

**No added file in the series carries a third-party copyright notice.** 19 of 21 carry a
copyright line naming the maintainer; the `.rst` and the functional test carry none, matching
peer convention.

## 7. Wider fork — resolved

Reviewing the fork beyond the series found two items a composition scan would report. Both have
been resolved:

1. A 3.1 MB prebuilt test fixture, `tests/netc/netc-load-initramfs.cpio.gz`, contained
   third-party binaries — `busybox` (GPL-2.0-only), glibc (LGPL-2.1-or-later), `libstdc++` and
   `libgcc_s` (GPL-3.0-or-later with the GCC Runtime Library Exception) and `iperf` — with no
   licence or source notice. Storing compiled GPL binaries carries a source-provision
   obligation, and a GPL-3.0 component inside a GPL-2.0 project is reported as a licence
   conflict even where the runtime exception applies. The fixture has been removed; its two
   consumers build it on demand, fetching the pinned upstream packages at run time, so none of
   those binaries are redistributed from this repository. Versions and licences are recorded in
   `tests/netc/README.md`.
2. Seven added `.c`/`.h` files carried no SPDX tag and now do.

⇒ Generalisable check for the sibling forks: list the tracked binaries the FORK itself adds
(`git ls-files` filtered to binary extensions, minus everything inherited from upstream) and
confirm none of them is a compiled executable or library. Screenshots and small test fixtures are
not the concern; redistributed third-party binaries are. A prebuilt fixture is a cache; if a build script
can regenerate it, storing it converts a convenience into a distribution obligation.

## 8. Re-run checklist

1. Confirm whether licensed SCA tooling is available; if not, the scan belongs to whoever holds
   the licence. Scope it to added and modified files.
2. Create a worktree inside the repository, branched from the series tip.
3. Run `reuse lint-file` over `--diff-filter=A` files only.
4. Classify every finding against section 3. `missing license` is not a per-file defect.
5. For each real finding, establish the upstream peer convention before editing.
6. Amend the owning commit, **then** re-run `checkpatch.pl --no-signoff --branch`.
7. Confirm commit trailers: `Signed-off-by` only.
8. Run the derivation check at K=8, record the corpus revisions, and inspect any match before
   classifying it.
9. Record non-findings with the peer counts that support them, and state the limits of each check.
