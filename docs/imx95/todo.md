# qemu-imx95 — open work items

Items that survive into the current (post-`imx95-v1.x`) state. The v0.x→v1.x
milestone groups that used to organise this file are gone: those milestones
shipped, and their items were either implemented (deleted here) or recorded
in `known-limitations.md`. See *How items leave this list* at the bottom.

## Before any qemu-devel upstream submission

A few register values to cross-check against the RM / real silicon before
submission. None break current functionality.

> RESOLVED 2026-08-02 (shakeout): the SDHCI SDMA-boundary detection is *already*
> the idiomatic `SDHCI_QUIRK_NO_SDMA_BOUNDARY` quirk flag (`hw/sd/sdhci.c` +
> `include/hw/sd/sdhci.h`), not an `io_ops` pointer-compare — no refactor needed.
> LPUART `VERID` `0x04040007` and LPSPI `VERID` `0x02000004` are CONFIRMED
> silicon-true against the real-board register capture (`regs-verid.txt`,
> 2026-07-09), so they are no longer "fabricated." The items below are the
> genuinely RM/board-only values still to verify — the i.MX95 EVK devmem covers
> them (see the wattson calibration board note; EVK inbound).

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
