# Reset-value differential audit (i.MX 95)

The fleet audit that mcxn947qemu started and rt1180emulator hardened:
`memset(regs, 0)` at reset is a **silent claim that every reset bit is 0**, and
a guest that reads such a register believes it. Many silicon registers reset
*non-zero*; when a driver reads one and computes from it, our zero becomes the
guest's own (wrong) configuration.

**Golden:** the reset-value column of `95_docs/IMX95RM.pdf` (Rev 5, 11263 pp) —
a document the model authors did not write. Cross-checked against the fleet's
findings on shared IP (mcxn's eDMA, 91's LPI2C).

**Classification** (mcxn/91 taxonomy):
- **FIX** — a *documented, consumed* field resets non-zero; our 0 is wrong and
  a consumer reads it. A real bug (silent on QEMU, wrong on silicon).
- **DECISION** — resets non-zero but in *reserved* bits the RM disclaims, or a
  field no consumer reads. Not seeded, **with the reason recorded** here (a
  deviation without a reason is a bug you have agreed not to look at).
- **NULL** — checked and already correct. Reported at the same volume as a hit.

## The i.MX 95's own shape

The **SM-managed blocks (CCM / PLL / SRC / GPC / …) are exercised by the real
NXP System Manager firmware on every boot** and were validated against silicon
`clk_summary`. That firmware is a live oracle the MCU nodes lack — so the audit
yield on those blocks is low (e.g. `imx95_ccm.c` is a clean functional register
file that stores/returns MUX/DIV and fabricates **no** frequency, so it is
immune to rt1180's `OBSERVE`=6 MHz class). The **high-yield targets are the
Linux-facing hand-written stubs no firmware exercises** — eDMA, audio, net,
display — which is exactly where the first bug was found.

## Findings

### FIX — eDMA `CH_SBR` laundered to zero  (commit: edma CH_SBR seed)
RM: `CH0_SBR..CHn_SBR` reset `0x0000_8007` (eDMA3 ch.41, eDMA5 ch.71) /
`0x8015` (32-ch eDMA5 ch.154) — **PAL=1** (privileged) + **MID=7** (bus master
ID). We memset it to 0. Mainline `fsl-edma` **read-modify-writes** CH_SBR to set
its RD/WR direction bit, so Linux read our 0 and wrote it back as MID=0 /
non-privileged — every transfer under the wrong master ID on silicon. Nothing
enforces MID in QEMU, so it was silently wrong. Seeded per-instance
(`ch-sbr-reset` property, default 0x8007); mutation-verified qtest. *mcxn found
the identical bug in the same driver on its chip.*

### FIX — LPI2C `VERID` fabricated  (commit: lpi2c VERID)
RM: `VERID` reset `0x0103_0003` on **every** LPI2C instance (AON 1-2, wakeup
3-8). We returned `0x0100_0003` — a made-up minor version (the source even said
"plausible"). Not read by Linux `i2c-imx-lpi2c` (so latent, not biting), but a
fabricated value silicon never produces; corrected to the RM value.

### FIX — SAI `VERID`/`PARAM` picked to match the driver, not silicon  (commit: sai per-instance)
The SAI model returned one hardcoded VERID `0x0303_0000` / PARAM `0x0005_0704`
for every instance, and the source said it was chosen to "match the soc_data the
driver assumes." `fsl_sai` **reads PARAM at probe** (`sai->param.dataline = PARAM
& DLN_MASK`; spf/wpf likewise). RM: VERID `0x0302_0002` (all); PARAM fields
FRAME[19:16], FIFO[11:8]=2^depth, DATALINE[3:0]. The hardcoded value matched
SAI5 (4 datalines) and was wrong for the wired instances — SAI1 (2 lanes,
32-deep = `0x0005_0502`), SAI3/wm8962 (1 lane, 128-deep = `0x0005_0701`), SAI2
(8 lanes), SAI4 (2). Made VERID/PARAM per-instance properties, set from the RM.
Verified: all 5 SAIs bind, WM8962/SAI3 playback still drains the full stream.

### FIX — MICFIL `VERID`/`PARAM` fabricated, and it hid a real feature  (commit: micfil)
RM: VERID `0x020F_0000` (major 2, minor 15), PARAM `0x010B_0154`. We returned
`0x0100_0000` / `0x0000_0034`. `fsl-micfil` reads both at probe. The old PARAM
reported **NUM_HWVAD=0** deliberately "to keep the voice-activity path out of
probe" — i.e. it hid a feature silicon has (1 HWVAD) to dodge modelling it.
Set the true values; NPAIR is unchanged (8 mics) and FIFO depth comes from
soc_data, so PDM capture still delivers non-silent samples. The HWVAD path is
only set up on demand, not at probe — so reporting the truth costs nothing.

### DECISION — eDMA `MP_CSR` reset is reserved-bits-only
RM: `MP_CSR` reset `0x0031_0000` (eDMA3) / `0x0050_0000` (eDMA5) — non-zero and
per-instance. **Every set bit (16, 20, 21) lands in the RM's "Reserved [23:16]"
field**, which §1.4 says "must not be used." ACTIVE, ACTIVE_ID and all control
bits (GCLC/ERCA/GMRC/HALT) reset to 0. `fsl-edma` masks the control bits it
writes and never depends on [23:16]. **Not seeded** — per the consumer-touched-
fields rule, nothing reads those reserved bits.

### NULL — LPI2C `PARAM` FIFO depth
RM: `PARAM` reset `0x0000_0303` (2^3 = 8-deep tx/rx FIFOs). We already return
`0x0303`. Immune to 91's watermark bug (they had `0x0404`=16-deep, since fixed).

### NULL — LPI2C `MSR` TDF-at-reset
RM: `MSR` reset `0x0000_0001` (TDF, tx-FIFO-empty, bit 0 set). Our model forces
`MSR_TDF` on every read — already matches.

## Method notes / limits

- The SM-tree CMSIS (`imx-sm/devices/MIMX95/MIMX95_*.h`) is a **per-peripheral,
  SM-managed subset** — it has CCM/PLL/SRC/LPI2C/… but not the Linux stubs
  (eDMA/SAI/ENETC/DPU). There is no full i.MX95 MCUXpresso SDK on this host, so
  a full CMSIS-join extractor (rt1180's `extract-rm-golden.py`) covers only the
  SM blocks. For the high-yield Linux stubs the golden is read from the RM's
  register chapters directly (offset + reset), keyed to the model's own base.
- The 95's CCM uses nested SET/CLR/TOG struct-arrays (`CLOCK_ROOT[123]` →
  `CLOCK_ROOT_CONTROL{RW,SET,CLR,TOG}`); a naive CMSIS name-join won't match the
  RM's `CLOCK_ROOTn_CONTROL` without handling the aliasing.
- **COVERAGE IS A FLOOR, NOT A CEILING** (mcxn): rows whose table layout did not
  parse are invisible to every check built on the golden. This is a lower bound
  on the bugs, never an upper one.
