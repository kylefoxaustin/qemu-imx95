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

- **VMState snapshot/restore test.** Every v0.1 device declares
  VMState; none of it is exercised. Add a savevm/loadvm round-
  trip during SPL execution (savevm mid-boot, loadvm, verify SPL
  resumes coherently). Catches silent VMState bugs before they
  become "every snapshot is corrupted" surprises later.

## Before v0.4 (Linux preboot via -kernel direct load)

(v0.3 ended up being "U-Boot interactive prompt" rather than "Linux
to login." Two items below were originally scoped for the v0.3
prereq pass; both are now done. The remainder are v0.4 prereqs.)

- **DONE in v0.2 — SDHCI long-transfer stall.** Fixed in
  `91c0548604` via SDMA buffer-boundary bypass for i.MX uSDHC.
  Full 1.3 MiB u-boot.bin reads through cleanly.

- **SCMI protocols beyond what v0.1 stubs.** Still outstanding.
  v0.2-prep extended the server (CLOCK_ATTRIBUTES always-SUCCESS,
  imx-misc protocol 0x84, POWER_DOMAIN protocol 0x11). Linux will
  exercise more: perf (DVFS), sensor (thermal), system-power, plus
  remaining imx-vendor extensions (`scmi_lmm`, `scmi_bbm`,
  `scmi_cpu`, more of `scmi_misc`) seen in `imx95.dtsi:406-420`.
  Audit gaps before Linux bring-up; either extend the stub or
  accept that Linux probes will log "unsupported protocol"
  entries and degrade.

- **ELE `GET_INFO` soc_rev = 0xA1 may trip Linux errata code.**
  Per the v0.1 review: U-Boot uses soc_rev mainly for printf, but
  Linux's i.MX silicon-rev-aware errata paths branch on it. If
  v0.4 Linux boot shows weird per-rev behavior, check this first.
  Cross-check against the i.MX 95 RM "Identification" chapter
  and set the value the silicon actually reports.

- **DDR controller model decision.** Document explicitly:
  "DRAM is RAM; DDRC is logging-stub." Works for boot-to-userspace;
  does not work for DRAM training, power management, suspend/resume.
  Tag in any v0.4 user-facing README.

- **Kernel image + DTB + initramfs availability.** Before
  committing to Option B' (`-kernel` direct load), verify
  `references/linux-imx/` has a buildable imx95 defconfig and that
  we can produce the Image/DTB pair the `-kernel`/`-dtb` args
  expect. 30-min check; gates the v0.4 plan.

## Before v0.4 (usability + verification, reviewer-flagged)

- **`tests/uboot-prompt/` regression test.** Mirror of
  `tests/spl-banner/` but with the socat key-send dance to verify
  the v0.3 interactive prompt hasn't regressed. Document as
  manual-only (not CI) since the socket-chardev setup is per-
  developer. README in the test dir should give the exact
  invocation. Reviewer-flagged in v0.3 round-3 review.

- **README user-facing note for the no-key autoboot path.**
  Without a keystroke during the countdown, U-Boot tries MMC
  (voltage-select -110), NVMe (no PCIe clocks - graceful), USB
  (crash on usb@4c100000). One-line warning in any user-facing
  doc: "press any key during 'Hit any key to stop autoboot:' to
  reach the interactive prompt; otherwise the board resets."
  Cheap; prevents issue reports.

- **PCA9450 PMIC over LPI2C as known-issue.** v0.3 stubs all 8
  LPI2C controllers as logging-only. Any v0.4 path that probes
  the PMIC for actual power-domain reads/writes will hang on I2C
  transfer-complete polling. Tag as known-issue in v0.4 docs
  before the Linux pm subsystem touches it.

- **SDHCI class-flag refactor (upstream prep, parked).** Convert
  the v0.2 `s->io_ops != &usdhc_mmio_ops` guard into an explicit
  `SDHCIClass::no_sdma_boundary` flag. Reviewer-flagged in
  v0.2-prep round 1; deferred through v0.2 + v0.3. Lands in the
  "upstream-prep" milestone (likely v0.4.1 or a dedicated tag).

- **No-key autoboot reach shell.** Currently autoboot resets on
  the unmodeled USB controller chain (usb@4c010010, usb@4c100000,
  usbmisc@4c200200). Path-C-bucket-1 stubs for each, plus the
  MMC voltage-select fix, would let autoboot complete without
  resetting and drop the user to the prompt automatically.
  Cosmetic but improves first-impressions-on-running-the-emulator.
  Lower-priority than Linux preboot.

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
