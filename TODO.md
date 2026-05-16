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

- **SCMI vendor protocol 0x84 (`scmi_misc`)** — SPL queries
  `rom_boot_info` via this protocol just after the banner. Our
  SCMI server doesn't advertise or handle protocol 0x84, so SPL
  prints `SCMI: failure at rom_boot_info` and resets. Extend the
  SCMI server to advertise + stub-respond to scmi_misc messages.

- **uSDHC / eMMC storage model** — SPL tries to load the next
  stage (U-Boot proper, ATF, M33 SM firmware) from SD/eMMC and
  prints `SPL: failed to boot from all boot devices`. Need a
  Freescale eSDHC model at the appropriate base, plus boot media
  (a disk image with the FIT or AHAB container).

- **Container loading** — Pick a story: stock NXP AHAB container,
  FIT, or a custom unwrap. Affects the storage layout above.

## Before v0.3 (Linux to login)

- **SCMI protocols beyond what v0.1 stubs.** v0.1 will respond to
  the SCMI protocols U-Boot SPL exercises (base, clk, pinctrl,
  power-domain). Linux exercises more: perf (DVFS), sensor
  (thermal), system-power, plus the imx-vendor extensions
  (`scmi_lmm`, `scmi_bbm`, `scmi_cpu`, `scmi_misc`) seen in
  `imx95.dtsi:406-420`. Audit gaps before Linux bring-up; either
  extend the stub or accept that Linux probes will log
  "unsupported protocol" entries and degrade.

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
