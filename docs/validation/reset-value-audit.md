# Reset-value differential audit (i.MX 95)

The fleet audit that mcxn947qemu started and rt1180emulator hardened:
`memset(regs, 0)` at reset is a **silent claim that every reset bit is 0**, and
a guest that reads such a register believes it. Many silicon registers reset
*non-zero*; when a driver reads one and computes from it, our zero becomes the
guest's own (wrong) configuration.

**Golden:** the reset-value column of `95_docs/IMX95RM.pdf` (Rev 5, 11263 pp) —
a document the model authors did not write. Cross-checked against the fleet's
findings on shared IP (mcxn's eDMA, 91's LPI2C).

## The rule that overrides the golden

> **ON A CAPABILITY REGISTER, MATCHING THE REFERENCE MANUAL IS THE BUG — UNLESS
> YOU ALSO IMPLEMENT THE CHIP BEHIND IT.** (mcxn947qemu)

An **identity** register (VERID, a core version, a PCI revision) should carry the
silicon's value: a wrong one silently forks the driver down a path the chip is
not. A **capability** register is a *contract*, and the only value it may carry
is the one the model can honour. Over-reporting is a promise the emulator makes
on the chip's behalf; under-reporting merely makes the model slower or simpler
than the part. **When you cannot resolve an ambiguity, resolve the asymmetry:
pick the value whose failure mode is under-reporting.**

This audit walked into that trap and had to reverse itself — see the SAI/MICFIL
self-correction below. It is the reason the RM alone is not a sufficient oracle.

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

### FIX/DECISION — ENETC `ECAPR2` ring count 0 → 1  (commit: enetc ECAPR2)
`fsl_enetc4` reads ECAPR2 at probe into `caps.num_{rx,tx}_bdr` (ethtool/devlink).
We memset the PORT window to 0 → Linux saw **0 rings** (false; the model does
service one). Silicon resets ECAPR2 to `0x0008_0008` (8+8), but the model's BDR
handlers are all hardcoded to ring index 0. Advertising 8 would be *worse* than
0 — Linux could spread traffic across rings 1-7 the model silently drops (a ring
count does not degrade gracefully, unlike an SDHCI tuning mode). Report
`0x0001_0001`: the truth about the emulation, the safe under-report direction.
Advertising silicon's 8 belongs with a future multi-ring model. Verified:
enet-lab3 self-test still passes (real L2 through ring 0).

### FIX — XCVR: six non-zero resets, and an invented VERSION  (commit: xcvr)
The whole control window was memset to 0 and handed a made-up VERSION.
`fsl_xcvr` **read-modify-writes** several of these (`regmap_update_bits` on
EXT_CTRL), so the zero reset is laundered into the guest's own config:

| reg | RM reset | |
|---|---|---|
| `EXT_CTRL` | `0xf820_4040` | CORE/CMDC/DPTH held in reset, SLEEP, RX/TX_FWM=64 |
| `RX_CMDC_CTRL` | `0x0028_1b02` | |
| `RX_DATAPATH_CTRL` | `0x0000_2c89` | |
| `DMAC_PRE_MATCH_VAL` | `0x4e1f_f872` | the SPDIF **preamble match pattern** |
| `DMAC_DTS_PRE_MATCH` | `0x1387_fe1c` | |
| `HPD_DBNC_CTRL` | `0x0003_0d40` | |

`VERSION` we returned `0x0001_0000`; it resets to **0** — the RM's column *and*
the driver's own `reg_defaults` table agree, and neither is a document this
model's author wrote. Addresses re-derived, not assumed: the DT gives fsl_xcvr a
separate `"regs"` region at base+0x800, so driver offset 0 = RM 0x800
(91emulator: *the RM is authoritative for VALUES, the driver and DT for
ADDRESSES*). Verified: tests/spdif still passes.

### FIX — DSI reported the wrong IP revision  (commit: dsi)
We returned `DSI_VERSION = 0x3133_3100` (dw-mipi-dsi HWVER_131). The RM resets it
to `0x3031_3531` — ASCII "0151", **v1.51**. Reporting the wrong IP revision is
the ENETC-PCI-Revision-ID class: it silently forks the driver down a path the
silicon never takes. Here we got away with it — both revisions land in the same
timing branch (the driver carries an explicit `hwver_is_151` flag precisely
because `0x30313531` is numerically *less* than `0x31333100`) — but that is luck,
not design. Also seeded `MODE_CFG`=1, `PHY_TST_CTRL0`=1, and `CMD_PKT_STATUS` =
`0x0005_0015` (we returned `0x15`, missing the v1.51 buffered EMPTY bits 16/18).
Verified: tests/dsi still passes (rm67191 panel, card0-DSI-1 @1080x1920).

### FIX — ISI `CHNL_SCALE_FACTOR` = 0 was a dangerous zero  (commit: isi)
`CHNL_SCALE_FACTOR` resets to `0x1000_1000` — **unity** in the scaler's Q12
format. We memset it to 0. A scale factor of zero is not merely wrong, it is
*legal and catastrophic* — the same family as 91's QDC `POSDPER` (a period
register whose zero means "infinitely fast" to a speed observer) and mcxn's ADC
gain-of-zero. **The dangerous zeros are the ones where zero is a meaningful
value, not the ones where it is obviously junk.** imx8-isi always *writes* the
factor before streaming (so it is not laundered) but debugfs dumps it. Also
`CHNL_IMG_CFG` = `0x0438_0780` (1920x1080). Verified: tests/camera still passes.

### FIX — LPUART `DATARO` answered 0, which means "the FIFO has data"  (commit: lpuart)
Chasing mcxn947qemu's LPUART capability-contract bug on our console block.
`DATARO` (0x30) is the **non-destructive read-only alias** of DATA. We did not
model it at all, so it fell to the default case and returned **0** — and on this
register 0 means **RXEMPT is clear**, i.e. *"the receive buffer has data."* A
reader polling the alias — which is what an alias is *for* — takes a phantom
byte out of an empty FIFO. Linux's `fsl_lpuart` never reads it (so latent), but
a bootloader or bare-metal firmware would believe it. **An unmodelled register
is not a free register: it still answers, and zero is an answer** (mcxn's rule,
their `MRDROR`). Now reports the pending byte, or RXEMPT, without popping.

### DECISION — LPUART FIFO depth: we advertise 1 and deliver 1
mcxn's bug was the reverse of ours and worth stating: their LPUART *advertised*
an 8-deep RX FIFO and *implemented* a one-byte holding register — a capability
register is a contract, and they promised eight and delivered one. It stayed
invisible because `STAT[RDRF]` means "count > RXWATER" and RXWATER resets to 0,
so a 1-deep and an 8-deep receiver are behaviourally identical until someone
sets a watermark. **We are self-consistent:** PARAM and FIFO report size 0
(depth 1) and the model *is* a one-byte holding register. Silicon is 16-deep
(`PARAM = 0x0000_0404`, `FIFO = 0x00C0_0033`), so this is a deliberate
**under-report — the safe direction** (over-reporting is a promise the emulator
makes on the chip's behalf). Implementing a real 16-deep FIFO with
watermark-level RDRF is a *feature*, not a reset-value fix; filed separately.
Everything else on this block was already correct and is a NULL result:
`VERID = 0x0404_0007`, `BAUD = 0x0F00_0004`, `STAT = 0x00C0_0000` (TDRE|TC),
and DATA already returns RXEMPT when empty.

### SELF-CORRECTION — SAI/MICFIL PARAM: I promised hardware I do not have
The SAI and MICFIL fixes above set `PARAM` to the RM's reset value. **`PARAM` is
a capability register, and both of those values promised hardware this model does
not contain.** Corrected (commit: *a capability register reports what it
DELIVERS*):

- **MICFIL** — I set `NUM_HWVAD=1` + the HWVAD_ZCD/ENERGY bits, because silicon
  has one hardware voice-activity detector. **This model has no HWVAD at all** —
  a guest enabling it would wait forever for a detection that cannot come. My own
  commit message had scolded the previous author for reporting 0 "to keep the
  voice-activity path out of probe." *They were right; 0 was an honest
  under-report.* Restored.
- **SAI** — I set `DATALINE` per instance from RM Table 491 (SAI1=2, SAI2=8,
  SAI4=2, SAI5=4). **This model implements exactly one data line** (only
  `TDR0`/`RDR0`), so a TDM setup would push audio at lines it silently drops.
  `DATALINE` is now **1** on every instance. The FIFO field was already safe and
  stays: we hold 128 words and never advertise more, and SAI1 still reports its
  true 32-deep silicon FIFO — which *also* under-reports our own capacity, so
  guest code sized against this model still fits the real part.

**Structural fix so it cannot drift again:** *a capability that is a CONSTANT can
drift from the thing it describes; one COMPUTED FROM it cannot.* PARAM is now
built from `SAI_PARAM_FIFO_EXP` / `SAI_PARAM_DATALINES`, with `QEMU_BUILD_BUG_ON`
making any disagreement with `IMX95_SAI_FIFO_DEPTH` — or with the single data line
we implement — a **compile error**. Negative-tested both ways.

### NULL — MICFIL `NPAIR` is honest, and my bug report was not
I filed MICFIL's `NPAIR=4` (8 mic inputs) as an over-report, on the reasoned
assumption that only `DATACH0` was fed. **Checking disproved it.** The model
serves all eight `DATACHn` registers, and with the board limit lifted (DT
`num-channels` 2 → 8) an 8-channel S32 capture succeeds, non-silent. The
capability is real *and* the model can honour it.

The 2-channel cap on the EVK is a **board** constraint — the dmic node says
`num-channels = <2>` because the board wires two PDM mics — and the emulator
refuses an 8-channel open *exactly as the real hardware would*. (`fsl-micfil`
also only *stores* `param.npair`; the channel count comes from the driver's own
constant and the DT.)

Recorded because a null is a result: **I raised a bug from reasoning and the
measurement killed it.** The same instinct that finds real over-promises
manufactures false ones, and only checking tells them apart.

### DECISION — eDMA `MP_CSR` reset is reserved-bits-only
RM: `MP_CSR` reset `0x0031_0000` (eDMA3) / `0x0050_0000` (eDMA5) — non-zero and
per-instance. **Every set bit (16, 20, 21) lands in the RM's "Reserved [23:16]"
field**, which §1.4 says "must not be used." ACTIVE, ACTIVE_ID and all control
bits (GCLC/ERCA/GMRC/HALT) reset to 0. `fsl-edma` masks the control bits it
writes and never depends on [23:16]. **Not seeded** — per the consumer-touched-
fields rule, nothing reads those reserved bits.

### FIX — WM8962 `R27` reset 0, and it made the codec mute forever
**The most consequential reset value in this audit, and it is not even on the
SoC — it is on the far side of an I2C bus, in a codec I had swept right past.**

Datasheet: `R27 (ADDITIONAL_CONTROL_3)` resets to `0x0010` — `SR=0` with
`INT_MODE` set, i.e. **48 kHz**. Our model `memset` the register file to zero
and left `R27` at 0. That is not a cosmetic difference, because of how the
driver writes it:

```
  wm8962_hw_params() -> snd_soc_component_update_bits() -> regmap
                                    ^ WRITES NOTHING IF THE VALUE IS UNCHANGED
```

regmap compares against **its own cache, seeded from the datasheet defaults —
not from us**. At 48 kHz the driver computes exactly `0x0010`, sees no change,
and **never puts a byte on the I2C wire**. The codec is never told; it never
tells the SAI; the SAI runs on whatever it guessed.

And what it guessed was `SAI_DEFAULT_RATE` = 48000. **The invented number and
the true number were the same**, so the playback test passed — for months — on a
rate that no device in the machine had ever asserted. A duration oracle
*structurally cannot* see this: both models play 2 seconds in 2 seconds.

> **A DEFAULT THAT HAPPENS TO EQUAL THE ANSWER IS NOT AN ANSWER. IT IS A GREEN
> LIGHT WITH NO WITNESS BEHIND IT.**

Found by **93emulator**, who hit it on their own board, then predicted it on
mine sight-unseen and was right. The trace, before the fix:

```
48 kHz: TX enable: rate=48000 Hz announced_by_codec=0   <- INVENTED
16 kHz: TX enable: rate=16000 Hz announced_by_codec=1   <- told
```

**Fix**: reset `R27` to `0x0010`, and **announce the decoded rate out of reset**
— in the reset *exit* phase, so it cannot be clobbered by a device that resets
after us. We are the bit-clock master; whoever we clock cannot know the rate
unless we say it, and after power-on the driver may never ask again.

Two latent traps found while fixing it, both of which would have made the fix
*look* applied while changing nothing:
- the SAI's rate handler returned early when the announced rate **equalled the
  rate it already believed** — so an announcement that merely confirmed the
  guess would never mark itself heard. *Being told 48 kHz when you already
  believed 48 kHz is not a no-op: it is the difference between knowing and
  guessing.* Record that you were TOLD before deciding whether anything CHANGED.
- the SAI seeded `s->rate = 48000` at realize. A seeded rate is **a belief with
  no source**, indistinguishable from one the codec supplied. It now starts at 0.

The test now gates on **provenance**, not just duration: it reads the SAI's own
trace and refuses a pass whose rate no codec ever announced — even a correct one.
Mutating the fallback to a wrong value (8000) no longer perturbs the 48 kHz pass,
which is the proof that the fallback is now a guard rather than a source.

### NULL — LPI2C `PARAM` FIFO depth
RM: `PARAM` reset `0x0000_0303` (2^3 = 8-deep tx/rx FIFOs). We already return
`0x0303`. Immune to 91's watermark bug (they had `0x0404`=16-deep, since fixed).

### NULL — LPI2C `MSR` TDF-at-reset
RM: `MSR` reset `0x0000_0001` (TDF, tx-FIFO-empty, bit 0 set). Our model forces
`MSR_TDF` on every read — already matches.

## Off-SoC parts — the golden's blind spot (added 2026-07-14)

This audit's golden was the i.MX 95 **RM's reset column**, which describes on-SoC
blocks only. 93emulator's law: *an audit's blind spot is its golden's blind spot,
and an RM-golden reset-value audit writes "checked clean" over every off-SoC part,
because "the RM doesn't mention it" and "it's correct" produce the same empty
grep.* So the parts on the I²C/I3C buses — PMICs, codec, IO-expanders — were never
in scope above. Swept here, each against **its own** datasheet/consumer, not the
RM.

### DECISION (scaffold, labelled) — PF09 / PF53 PMIC `memset(0)` reset
`hw/misc/imx95_pmic.c` zeroes the PF09 and PF53 register files at reset and seeds
only `REV_ID`. A real PMIC powers up with per-rail **OTP defaults** in its
`SW*_VRUN`/`LDO*_RUN` VOUT registers — the same fabrication class 93/91 found in
their PCA9451A (BUCK4 off by 2× on the SD rail).

**Severity measured from the consumer, not inherited from 91/93 — and it is the
"95 behaves as 95" difference:** on i.MX 95 the PMIC is owned by the **M33 System
Manager**, not read directly by Linux. The only voltage domain the SM exposes to
the AP is `DEV_SM_VOLT_ARM = PF09 SW1` (`configs/mx95evk/config_lmm.h`), and the
SM *writes* SW1 to `BOARD_VOLT_ARM` before the AP runs (the A55 can't execute
without its rail set), so the AP-facing `BRD_SM_VoltageLevelGet → PF09_VoltageGet`
read-back returns the **written** value, never our 0. Rails the SM manages are
written before read; rails neither written nor AP-read are not consumer-touched.
**No fabricated voltage reaches Linux** — verified from `brd_sm_voltage.c` +
`config_lmm.h`, and the machine boots to userspace on the real SM.

**Not "fixed" with a guess:** no PF09/PF53 datasheet is in `95_docs/` (RM only), so
a seeded OTP byte would read as measured while being unsourced — the exact
laundering audited against all week. Left as **labelled scaffold** (comment in
`imx95_pmic.c`, not "checked clean"). The sourceable path if a future consumer
makes a rail reachable: 93's method — target voltages from the board DT + vsel
encoding from mainline `drivers/regulator/pf0900-regulator.c` — a **DERIVED**
value, labelled. Belt-and-suspenders empirical check (SCMI `VOLTAGE_LEVEL_GET` on
the ARM domain never returns a 0-derived level) deferred to board time, stated not
claimed.

### NULL — PCAL6408A / PCAL6524 / PCA9554 / ADP5585 IO-expanders
Reset `memset(0)` + the ID/manufacturer register where the driver gates on it
(ADP5585 `0x00 = 0x20`). For a pca953x/adp5585-class expander the POR **is**
all-zero for output/polarity and all-**input** for the direction register — and
the direction default (`0xFF`) is produced by the driver's own regcache, not read
from our reset (91's point #4: one file can hold both a real POR and a fabrication;
each judged separately). No consumer reads a fabricated non-zero here. Left as-is.

## Not audited, and why (no silent caps)

Stating these plainly, because a sweep that quietly skips a block reads as
"covered" when it isn't (mcxn: *coverage is an assertion, not a print*).

- **DPU — CANNOT be audited against this RM.** Chapter 170 ("Display
  Controller") is 159 lines of prose and contains **zero reset-value tables**;
  the register file is not in this document. There is no golden here to diff
  against. Not "clean" — *unmeasured*.
- **SRC / GPC — real gap, deliberately deferred.** Both have many non-zero
  resets (`SRC AUTHEN_CTRL = 0xFFFF_0000`, `LPM_SETTING_1/2 = 0x3333_3333`,
  `SLICE_SW_CTRL = 0x7F00_0003`, `SSAR/PSW_ACK_CTRL`, GPC's `CMC_*`) and our
  models memset them to 0. Neither model *fabricates* a value — they are pure
  register files. Their consumer is the **M33 System Manager firmware**, which
  appears to write these with computed full values (`AUTHENCTRL_SW/HW/CPU`)
  rather than read-modify-writing them — but that is not proven. Seeding them is
  a **boot-critical** change to the block the SM drives, so it needs a seed +
  SM-boot A/B, not a drive-by. (This is the MU TR-count lesson: a change that
  "matches the spec" broke boot and had to be retracted. Boot-test boot-critical
  changes.) **Next task.**

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
