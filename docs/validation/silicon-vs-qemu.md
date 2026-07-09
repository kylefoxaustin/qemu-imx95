# i.MX 95 QEMU vs real silicon — fidelity diff

First direct comparison of this emulator against a **real i.MX 95**: an
FRDM-IMX95-PRO reached over the lab network (BSP LF6.18.20, kernel
6.18.20-2.0.0, `fsl,frdm-imx95-pro` / `fsl,imx95`, Neutron firmware 3.1.2),
leased 2026-07-09.

Everything below is measured, not inferred. Register values were read through a
purpose-built aligned-32-bit `/dev/mem` reader with a `SIGBUS`/`SIGSEGV` handler,
so a TRDC-denied or power-gated region reports `FAULT` instead of killing the
run. The reader was verified against a known-bad address before any result was
trusted.

## Board caveat, and what it bounds

The board is an **FRDM-IMX95-PRO**; we model the **19x19 EVK**. Same SoC,
different board. So:

- **SoC-internal behaviour transfers** and is what this report compares: System
  Manager / SCMI, the Messaging Unit, EdgeLock Enclave, GIC, NETC/ENETC, Neutron,
  GPU/VPU identity.
- **Board-level detail does not**: PMIC wiring, Ethernet PHY choice, SD/eMMC
  layout, pinmux. A divergence there is not evidence of an emulator bug, and we
  must not "fix" the EVK to match an FRDM.

A second caveat that matters more than it looks: **real silicon does not bring
everything up cleanly either.** Before treating any QEMU probe failure as our
bug, compare against the board's own baseline (see "What silicon also fails at").

## What the board confirmed we already had right

| Thing | Silicon | Ours | |
|---|---|---|---|
| ENETC PF PCI ID / revision | `1131:e101` rev **4** | same | ✅ |
| ENETC PF devfns | `0.0`, `8.0`, `10.0` | same | ✅ |
| LPUART VERID | `0x04040007` | `0x04040007` | ✅ |
| LPSPI VERID | `0x02000004` | `0x02000004` | ✅ |
| LPI2C PARAM | `0x00000303` | `0x00000303` | ✅ |
| SAI PARAM (sai@42670000) | `0x00050704` | `0x00050704` | ✅ |
| SCMI vendor / protocol | `'NXP:IMX'` v2.1 | same | ✅ |
| SCMI perf domains | 13 | 13 | ✅ |
| SCMI clock IDs | 175 | 175 | ✅ |
| Neutron MMIO bases + IRQ | `0x4ab00000`/`0x4ab00004`, SPI 318 | same | ✅ |
| Neutron delegation shape | 5 NeutronGraph partitions / 82 nodes | identical | ✅ |
| Mali base/size | `0x4d900000` / `0x480000` | same | ✅ |
| VPU-ctrl window | `0x4c4c0000` / `0x10000` | same | ✅ |

The ENETC revision-4 confirmation is worth calling out: the rev1-vs-rev4 PCI
revision silently forks the Linux driver into a different PCS subsystem, and it
cost days to find. Silicon says rev 4. We emit rev 4.

The SM firmware version differs (`0x380` on the board, `0x333` under QEMU) simply
because the board runs a newer System Manager build. That is the firmware image,
not the model.

## Bugs found and fixed

### 1. Neutron: RESET was never honoured — real, behavioural

`mbox_send_reset()` is the one command the driver does **not** ring the doorbell
for. It writes `MBOX3 = RESET` and polls `MBOX0` for `RESET_VAL`, relying on the
running firmware to notice. Our model only acted on a doorbell, so `MBOX0` stayed
at `DONE` from the previous job and the poll never converged.

Consequence: `failed to reset neutron state` after **every** inference, followed
by a hardware reset and a full firmware reload before the next one.

Fixed by handling the command where it actually arrives — the `MBOX3` write.
Verified with a real eIQ-converted yolov8n taken off the board:

- `failed to reset` occurrences: **50 → 0**
- average inference: **165 ms → 41 ms** (the reset/reload churn is gone)
- delegation unchanged at 5 partitions / 82 nodes — matching silicon

### 2. Mali: we reported a GPU revision that doesn't exist

`GPU_ID` was `0xAC040000` (arch_rev 0, version_status 0). The register on real
silicon reads **`0xAC740001`** (arch_rev 7, version_status 1). kbase tolerated
the old value, so nothing looked broken — the guest was just told the wrong
revision. With the real value the emulated kernel prints exactly what the board
prints:

```
mali 4d900000.gpu: Register LUT 000a0a00 initialized for GPU arch 0x000a0c07
mali 4d900000.gpu: GPU identified as 0x4 arch 10.12.7 r0p0 status 0
```

### 3. NETC EMDIO: wrong PCI class and revision

Silicon presents `1131:ee00` as a **system peripheral, class 0880 prog-if 01,
revision 4**. We declared an Ethernet controller (class 0200) at revision 1.
Identification only — `enetc_pci_mdio` binds by vendor/device ID and never reads
the revision on this path — but `lspci` in the guest should agree with the board.

### 4. RGPIO PARAM held the wrong quantity

We returned the pin count (32) in `PARAM[7:0]`. Every RGPIO instance on silicon
reports `0x00000002`, so that field is plainly not a pin count. `gpio-vf610`
hardcodes 32 pins per port and reads neither `VERID` nor `PARAM`, so nothing
consumed it — a latent trap, now reporting the silicon values.

### 5. Our own test gate was broken, and had never run

`tests/neutron/run-converted.sh` greps case-insensitively for `timeout` as an
anomaly signature. That matches benign lines every boot prints (`SCMI
max-rx-timeout: 5000ms`, mali's `*_TIMEOUT is capped ...`), so the gate would
fail unconditionally the moment anyone supplied a converted model. Nobody ever
did — with no fixture the test SKIPs, and the broken assertion sat there.

This is the silent-SKIP failure mode the fidelity directive warns about: a test
that never runs is not a passing test. Fixed to match kernel fault signatures,
and now exercised with a real converted model.

## Divergences deliberately NOT changed

These are wrong versus silicon but changing them is either risky or pointless.
Flagged for discussion rather than "fixed".

| Item | Silicon | Ours | Why not touched |
|---|---|---|---|
| uSDHC `HOST_CTRL_CAP` | `0x07f3b407` | `0x057834b4` (QEMU generic) | **Risky direction.** The driver keeps the upper half as capabilities and synthesises `CAPABILITIES_1` from the lower half. The real value advertises SDR50/SDR104, which QEMU's SD model cannot tune — advertising modes we can't honour is exactly how you get tuning timeouts. Ours advertises *fewer, slower* modes: conservative and safe. |
| SAI `VERID.FEATURE` (`TSTMP_EN`) | `0x0002` | `0x0000` | Read and branched: gates the timestamp sysfs group. Ours claims fewer features, so the driver never pokes timestamp registers we may not implement. Flipping it is the riskier direction. |
| SAI `VERID` minor / `PARAM` per-instance | `0x03020002`; `sai@443b0000` PARAM `0x00050502` | `0x03030000`; one PARAM for all | `PARAM` fields are dead stores in `fsl_sai`; FIFO depth comes from a compile-time SoC table. Version compare is `>= 0x0301`, true either way. |
| MU `VER` / `PAR` | `0x0309000f` / `0x03040404`; ELE MU `0x01000000` / `0x00000808` | `0x00020000` / `0x00000404` | Nothing reads `VER`. Only `PAR[15:0]` (TR/RR counts) is read, and ours is *derived from* our physical register count, so the two can never disagree. **Do not "fix" ELE PAR to 8/8 without also giving that instance 8 TR/RR** — that would create a genuine dropped-word bug. Self-consistency matters more than the absolute value here. |
| LPSPI `PARAM.PCSNUM` | 2 | 4 | Only read for `fsl,imx93-spi`; i.MX95 uses `cs-gpios`. |
| LPI2C `VERID` minor | 3 | 0 | Never read. |
| ENETC `SICAPR0` ring counts | 6 TX / 6 RX | 1 / 1 | Honest simplification; the driver sizes itself from what we advertise. |
| LPUART `PARAM` | `0x00000404` | `0` | Never read on the i.MX95 path (the driver's FIFO sizing reads hw offset `0x28`). Depth 1 is conservative. |

## Gaps: hardware silicon has that our guest never sees

| Function | Silicon | Ours |
|---|---|---|
| `1131:ee02` NETC Timer (PTP) | `ptp_netc` binds at `0002:00:18.0` | **not modelled** — guest has no NETC PHC |
| `1131:e001` Root Complex Event Collector | two instances bind | not modelled |
| VPU codec window | `0x4c480000` size `0x10000` | we map `0x40000` (4x oversized, overlapping three *disabled* codec instances) |

The PTP timer is the substantive one, and the directive is explicit that Linux
must *see* the hardware. Its DT node carries no `clocks` property yet probes
fine, so a model is feasible: one MMIO BAR, exactly one MSI-X vector, and an
init path that is almost entirely writes with no ID gate.

## What silicon also fails at (the baseline)

Do not chase these in QEMU. The real board prints all of them:

- `mali ...: r0p0 status 1 not found in HW issues table`, `Protected memory
  allocator not available`, and five `*_TIMEOUT is capped ... to 4500ms` lines.
- `scmi-cpufreq: failed to register for limits change notifier for domain 8`,
  `cpu cpu0: EM: invalid power: 0`.
- `imx95-ldb ...: Failed to create device link`, same for
  `imx95-pixel-interleaver`.
- `pci 0000:01:00.0: Failed to enable PASID`.
- `imx8mq-usb-phy ...: supply vbus not found, using dummy regulator`.
- `hwmon hwmon2: temp1_input not attached to any thermal zone`.

## Neutron: the standing silent-wrong

Real silicon runs yolov8n at **22.2 IPS**, delegating 5 NeutronGraph partitions
of 82 nodes. Our model accepts the same converted artifact and produces the same
partitioning — but the NPU compute is proprietary and unmodelled, so the output
buffer is **never written** while `MBOX0` returns `DONE`.

By default a guest app therefore sees `STATUS_DONE` and reads uncomputed output.
That is the worst bug class in the fidelity taxonomy, and it is the current
default. The honest-fault lever exists and is off:

```
qom-set /machine/soc/neutron neutron-uncomputed-errcode 0x<errcode>
```

Whether that default should flip is a policy call, not a code call. Raised for
discussion — see the open questions below.

## Open questions for Kyle

1. **Flip the Neutron default?** Making `neutron-uncomputed-errcode` non-zero by
   default trades "the stack runs end to end" for "the guest is never lied to".
   The directive says silent-wrong is the worst class.
2. **Model `ptp_netc` (ee02)?** Real gap, self-contained, spec in hand.
3. **uSDHC capabilities**: leave conservative, or model uSDHC's own capability
   register properly (not the generic SDHCI default) and only advertise modes we
   can honour?
4. **VPU window size** — trim `0x40000` to the real `0x10000`?

## Nothing here implicates upstream QEMU

No divergence found so far traces to generic QEMU code. The SDHCI capability
question is about *our* board wiring (we never set `capareg`), not about
`hw/sd/sdhci.c`. The 15-patch upstream v1 series is headless and contains none
of the devices where divergences were found (Mali, Neutron, NETC, RGPIO).

## Method notes worth keeping

- **Reads are not always side-effect-free.** Reading a live MU's `RR` pops a
  message and would steal it from the running ELE driver. Status/ID registers
  only, on any device a driver currently owns.
- **`FAULT` is data, not failure.** Power-gated peripherals (GPU, VPU, Neutron
  when idle) bus-fault on read. Pin them awake with
  `echo on > /sys/bus/platform/devices/<dev>/power/control`, read, then restore
  `auto`.
- **Verify the verifier.** The reader was pointed at a known-unmapped address
  first; only after it returned `FAULT(SIGBUS)` with the parent surviving were
  its zeros and constants trusted.
- Diff two snapshots taken seconds apart to separate constants (IDs, config)
  from counters — it turns an opaque register file into a labelled one.
