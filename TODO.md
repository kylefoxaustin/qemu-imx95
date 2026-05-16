# qemu-imx95 — open work items

Items deferred from prior milestones, grouped by the milestone that
makes them load-bearing. The intent is that this list shrinks over
time as items either get fixed or get demoted into the milestone
review doc as "wontfix" with rationale.

## Before v0.0.3 (when the LPUART RX path comes online)

- **LPUART overrun + `CTRL.ORIE`** — `imx.lpuart` does not raise
  the IRQ on RX overflow. The Linux fsl_lpuart driver enables
  `UARTCTRL_ORIE = 0x08000000` during normal RX flow
  (`references/linux-imx/drivers/tty/serial/fsl_lpuart.c:173`), so
  without this, lost-byte conditions during heavy RX will go
  silent. Fine for v0.0.2 because the hello binary is TX-only.
  - Files: `hw/char/imx_lpuart.c` (IRQ update in `imx_lpuart_update_irq`,
    chardev byte-arrival in `imx_lpuart_receive`),
    `include/hw/char/imx_lpuart.h` (add `LPUART_CTRL_ORIE`).

## Before v0.2 (SPL → U-Boot proper handoff)

Open items remaining for the v0.2 milestone:

- **AHAB / SECO container on the SD image** — with the uSDHC
  stack landed, SPL now successfully reads sector 0 of the SD
  card and immediately rejects an all-zero image with
  `Parse seco container failed -14`. v0.2 closes when we can
  point `-drive` at a disk image that contains either a real
  NXP AHAB container or a minimal hand-rolled fake whose layout
  satisfies `parse_container_hdr_v3()` enough to let SPL load
  *some* payload and jump to it. The artifact lives in
  `tests/spl-banner/uboot-build/spl/u-boot.itb` for the FIT
  path, but the i.MX95 SPL build is configured for AHAB.

- **`scmi_pinctrl` SET should SUCCESS-no-op** (not return -1).
  v0.1's `scmi_protocol_stub` returns `NOT_SUPPORTED` for any
  message_id it doesn't know, including `PINCTRL_SETTINGS_CONFIGURE`
  (msg 0x06). SPL logs "Failed to set PAD = X" for every pad
  it tries to mux for uSDHC. Cosmetic since SPL doesn't bail
  on the failure, but noisy. Move pinctrl into the same
  warn_report_once + LOG_GUEST_ERROR pattern as the clock
  no-op handlers.

- **Banner-text smoke regression script.** `tests/spl-banner/` has
  a `README.md` documenting the run; v0.2 should add an actual
  script that runs SPL with a wallclock timeout, greps for the
  expected banner line, and exits non-zero if it's not there
  within 30s. Catches both functional regressions and the
  `-device loader` / `arm_load_kernel` ordering invariant
  (whichever is last-write-wins on the reset PC).

- **VMState snapshot/restore test.** Every v0.1 device declares
  VMState; none of it is exercised. Add a savevm/loadvm round-
  trip during SPL execution (savevm mid-boot, loadvm, verify SPL
  resumes coherently). Catches silent VMState bugs before they
  become "every snapshot is corrupted" surprises later.

## Before v0.3 (Linux to login)

- **SCMI protocols beyond what v0.1 stubs.** v0.1 will respond to
  the SCMI protocols U-Boot SPL exercises (base, clk, pinctrl,
  power-domain). Linux exercises more: perf (DVFS), sensor
  (thermal), system-power, plus the imx-vendor extensions
  (`scmi_lmm`, `scmi_bbm`, `scmi_cpu`, `scmi_misc`) seen in
  `imx95.dtsi:406-420`. Audit gaps before Linux bring-up; either
  extend the stub or accept that Linux probes will log
  "unsupported protocol" entries and degrade.

- **ELE `GET_INFO` soc_rev = 0xA1 may trip Linux errata code.**
  Per the v0.1 review: U-Boot uses soc_rev mainly for printf, but
  Linux's i.MX silicon-rev-aware errata paths branch on it. If
  v0.3 boot shows weird per-rev behavior, check this first.
  Cross-check against the i.MX 95 RM "Identification" chapter
  and set the value the silicon actually reports.

## Before v0.5 (PCIe + MSI consumers arrive)

- **GIC ITS wiring** — currently modelled as a freestanding
  top-level memory region at `0x48040000`. It looks right in
  `info mtree` but will NOT function as an ITS for MSI delivery.
  When PCIe (or any other MSI-capable IP) lands, the ITS must be
  instantiated as `TYPE_ARM_GICV3_ITS` *inside* the GICv3 device
  rather than as a separate region.
  - Files: `hw/arm/fsl-imx95.c` (the GIC realize block ~line 180),
    `include/hw/arm/fsl-imx95.h` (`FSL_IMX95_GIC_ITS` enum).

## Before any qemu-devel upstream submission (not blocking development)

These are fabricated values that don't break current functionality
but a maintainer will flag in review.

- **OCRAM-A total size.** `FSL_IMX95_OCRAM` covers
  `0x20480000-0x204DFFFF` (384 KiB), sized to fit U-Boot SPL's
  TEXT base through its BSS+stack. The Linux DTS only exposes a
  96 KiB sram1 slice within this range, so the real silicon
  OCRAM-A is at least 384 KiB but likely larger. Cross-check
  against the i.MX 95 RM "Memory Map" chapter and right-size.

- **`VERID = 0x04040007`** in `include/hw/char/imx_lpuart.h:88`.
  Plausible but not from the RM. The Linux driver does not branch
  on VERID for `imx*ulp` variants so v0.0.2 functions fine, but
  this needs to be the silicon-true value before upstream.
  Cross-check against RM "LPUART register descriptions."

- **BAUD POR value `0x0F000004`** in `hw/char/imx_lpuart.c:65`.
  Default OSR = 4 plus a plausible SBR field; matches common LPUART
  POR convention but not RM-verified. Cross-check against RM
  "LPUART reset values" table.

---

## How items leave this list

- Implemented → delete the bullet (the code change tells the story).
- Decided as out of scope → move into the relevant milestone review
  doc under "Known issues / fabrications" with an explanation.
- Demonstrably wrong (e.g., reviewer corrected my understanding) →
  delete and note in the next review doc.
