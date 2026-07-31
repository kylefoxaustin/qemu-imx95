# qemu-imx95 — open work items

Items that survive into the current (post-`imx95-v1.x`) state. The v0.x→v1.x
milestone groups that used to organise this file are gone: those milestones
shipped, and their items were either implemented (deleted here) or recorded
in `known-limitations.md`. See *How items leave this list* at the bottom.

## Before any qemu-devel upstream submission

Mostly fabricated-but-plausible register values, plus one refactor a maintainer
will flag. None break current functionality.

- **SDHCI class-flag refactor.** `sdhci_sdma_transfer_multi_blocks()` detects
  the i.MX/FSL uSDHC variant with `s->io_ops != &usdhc_mmio_ops` — a static
  `MemoryRegionOps`-pointer compare. Convert to an explicit
  `SDHCIClass::no_sdma_boundary` flag: the idiomatic QEMU way, and what a
  maintainer will ask for. (`hw/sd/sdhci.c`.)

- **SRC `RSTR_STAT` mirror — confirm uniform slice semantics.** The v1.x M7
  fault-recovery model mirrors all four `SLICE_SW_CTRL.RST_RSTR[3:0]` →
  `RSTR_STAT.RST_STAT[3:0]` bits instantly, for every slice
  (`hw/misc/imx95_src.c`). This assumes every reset slice tracks identically
  with no acknowledgement delay. Cross-check the RM "SRC reset slice" semantics;
  if any slice differs, the mirror needs slice-specific handling. ~30 min RM
  read; if ambiguous, leave as a documented known-unknown. Not blocking.

- **OCRAM-A total size.** `FSL_IMX95_OCRAM` covers `0x20480000-0x204DFFFF`
  (384 KiB), sized to fit U-Boot SPL's TEXT base through BSS+stack. The Linux
  DTS exposes only a 96 KiB sram1 slice within this range, so real OCRAM-A is
  ≥384 KiB, likely larger. Right-size against the RM "Memory Map" chapter.

- **`VERID = 0x04040007`** (`include/hw/char/imx_lpuart.h`). Plausible but not
  from the RM. The Linux driver doesn't branch on VERID for `imx*ulp`, so it
  functions fine — but set the silicon-true value before upstream. Cross-check
  the RM "LPUART register descriptions."

- **BAUD POR value `0x0F000004`** (`hw/char/imx_lpuart.c`). OSR=4 plus a
  plausible SBR field; matches common LPUART POR convention but not RM-verified.
  Cross-check the RM "LPUART reset values" table.

- **ELE `GET_INFO` soc_rev = 0xA1.** Fabricated; Linux's i.MX silicon-rev errata
  paths can branch on it. Three kernels boot clean today, so it hasn't
  manifested — but cross-check the RM "Identification" chapter and set the
  silicon-true value before upstream.

## Known gaps (future work, not pre-upstream blockers)

Behaviour/boot-arg caveats — `cpuidle.off=1`, the earlycon address offset
(`0x44380010`), DDR-is-RAM — are
documented in [`known-limitations.md`](known-limitations.md) and
[`../system/arm/imx95-evk.rst`](../system/arm/imx95-evk.rst), not here.

---

## How items leave this list

- Implemented → delete the bullet (the code change tells the story).
- Decided out of scope → move into `known-limitations.md` (or a milestone
  review doc) with the rationale.
- Demonstrably wrong (e.g. a reviewer corrected the premise) → delete and note
  in the next review doc.
