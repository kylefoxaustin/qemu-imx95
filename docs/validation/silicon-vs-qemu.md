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

### 6. NETC Timer (PTP) did not exist — now modelled

Silicon binds `ptp_netc` to PCI function `1131:ee02` at `0002:00:18.0` and
registers an IEEE 1588 PHC. We had no such function, so the guest had no NETC
PTP clock. Added as `hw/net/imx95_netc_timer.c`; its BAR0 now lands at
`0x4ccc0000`, the same address silicon assigns.

The trap worth recording: `gettimex64()` reads `TMR_CUR_TIME` (BAR0 + 0xf0), not
the `TMR_CNT` registers it wrote — on hardware `CUR_TIME` is the free-running
counter plus offset. A backing-store register file would have registered a PHC
whose `clock_gettime()` **succeeds and returns a constant, plausible, wrong
time**. Nothing errors; the clock simply lies. `CUR_TIME` is computed on read
from `QEMU_CLOCK_VIRTUAL` instead, and `tests/ptp/run.sh` asserts the clock
*advances* rather than merely existing.

PPS, periodic output and external-timestamp capture are left unimplemented and
degrade honestly. `TMR_STAT.ETSx_VLD` stays clear on purpose:
`netc_timer_handle_etts_event()` spins while that bit is set, so faking a
capture would hang the guest.

### 7. A latent PCI window bug, exposed by adding a device

Adding a fourth PCI function to the NETC bus immediately produced a synchronous
external abort in `msix_prepare_msi_desc()` while enabling **ENETC PF0**.

The `netc-pcie-mmio` alias covered `0x4cc00000..0x4cd20000`, but the dtsi
declares four ECAM ranges reaching `0x4cddffff`. Every BAR had happened to fit
below the alias. The new function's BAR0 filled the first range, pushing the
three ENETC MSI-X BARs into the third — which Linux happily assigned and the
machine did not back.

This bug was always there. It needed only one more PCI function to fire, and it
would have fired on a user's `-device` too.

### 8. TCP over ENETC never worked — TX checksum offload was never implemented

The biggest find of the night, and it was not silicon that found it. Chasing the
flake below turned up a red gate, and behind that gate was this.

When an skb is `CHECKSUM_PARTIAL` the ENETC driver does **not** checksum in
software. It sets `TXBD FLAGS_L4CS` (plus `ipcs` for IPv4), leaves only the
pseudo-header sum in the L4 field, and expects the MAC to finish the job. Our TX
path transmitted the frame untouched. Every TCP segment therefore arrived at the
peer with a wrong checksum and was dropped — counted in the peer's
`TcpExt InCsumErrors`, logged nowhere.

ICMP kept working, because the kernel checksums ICMP in software. So the link
looked perfectly healthy: carrier up, ping fine, counters climbing, TCP dead.
The peer's `/proc/net/snmp` told the whole story at a glance:

```
Tcp: ... InSegs 77   InErrs 77   InCsumErrors 77   OutSegs 0
```

`enetc4` only takes the offload path when checksum offload is active, which the
driver enables **at revision 4** — the revision we report, and correctly so per
silicon. **Reporting the right revision opened a code path we had never
implemented.** That is a general hazard: a fidelity fix can activate driver
behaviour the model does not have.

Fixed for IPv4 (via `net_checksum_calculate()`) and for IPv6 (own pseudo-header
computation, since the shared helper is IPv4-only). The interconnect test now
proves a byte-exact payload over both, and the IPv6 leg was checked by disabling
the new code and confirming that leg — and only that leg — fails.

Two harness faults had hidden this for months: `tests/interconnect-imx95/run-eth.sh`
drove `nc`, which this busybox does not build (both ends failed silently under
`2>/dev/null`), and its replacement `httpd -p PORT` binds the IPv6 wildcard so
IPv4 SYNs reached no listener. The gate had been red since at least `imx95-v2.4.0`.

### 9. A ~30% flake in the two-port NETC test — and a near-miss false bisect

While regression-testing the above, `tests/netc/run-2port.sh` failed. It looked
exactly like an intermittent silent packet-drop in the NIC model: links up, zero
packets received, both directions.

It was not. The machine has three ENETC PFs but the test gives `-nic hubport` to
only the first two. It then drove `eth0`/`eth1` by name — and Linux names
netdevs in probe-completion order, with the three PFs probing concurrently. In
about a third of boots one of those names landed on `0002:00:10.0`, which has no
peer on the hub. `/proc/interrupts` settled it: **every** failing run had
`eth0` or `eth1` bound to `0002:00:10.0`. Fixed by resolving both interfaces
from their PCI addresses; 3/10 failures before, 12/12 passes after.

Two process lessons, both expensive:

- **A single run of a flaky test produced a confident, wrong bisect.** It
  pointed at the PCI-window commit, which was innocent. Re-running the "known
  good" base commit five times (4 pass, 1 fail) exposed the noise. Never bisect
  on n=1.
- **Instrumenting the model made the bug disappear** (8/8 pass with
  `-d guest_errors`). That is itself a signal — it says "race, not logic" — but
  it also means the probe must go where it does not perturb timing. The guest's
  own `/proc/interrupts` cost nothing and answered the question immediately.

A flaky datapath test is indistinguishable from an intermittent silent
packet-drop, which is why it is worth fixing rather than re-running.

## Remaining gaps

| Function | Silicon | Ours |
|---|---|---|
| `1131:e001` Root Complex Event Collector | two instances bind | not modelled |
| VPU codec window | `0x4c480000` size `0x10000` | we map `0x40000` (4x oversized, overlapping three *disabled* codec instances) |

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

## Silent-wrong sweep: what we answer 0 for

Booting to userspace with `-d guest_errors,unimp` yields **2376 write-warnings
but only 104 read-warnings** across a whole boot. Writes to an unimplemented
register are absorbed and harmless; a *read* that returns 0 is the silent-wrong
candidate. Reading those exact addresses on silicon:

| Register | Silicon | Ours | |
|---|---|---|---|
| netcmix-blk-ctrl `0x4c810004` | `0x00000000` | 0 | ✅ our zero is correct |
| netc-prb `0x4cdf0104` | `0x00000000` | 0 | ✅ correct |
| display-csr `0x4b010000` | `0x00000002` | 0 | diverges |
| lvds-csr `0x4b0c0000` | `0x00000014` | 0 | diverges |
| camera-csr `0x4ac10000` | `0x00000073` | 0 | diverges |
| usb3-phy `0x4c1f0000` | `0x0020f003` | 0 | diverges |
| vpu-csr `0x4c410000` | (power-gated, faults) | 0 | unknown |

All the divergences are `syscon`/CSR regmap blocks that drivers read-modify-write
rather than branch on, and display, LVDS, camera and USB3 all come up in the
emulator today. Left alone; recorded here so a future consumer that *does* branch
has the real values to hand.

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

**What the board says about that choice.** Reading the real mailbox during a
successful inference:

```
MBOX3 = 0x00000269 (RUN)      MBOX0 = 0x000000a3 (RUN_ACK)   MBOX1 = 0x00000000
MBOX3 = 0x00023637 (RESET)    MBOX0 = 0x00000000 (RESET_VAL) MBOX1 = 0x00000000
APPCTRL = 0xf8070400          APPSTATUS = 0x00000010
```

Three things follow. First, **`MBOX0` really does return to `RESET_VAL` when
`RESET` lands in `MBOX3`** — direct silicon confirmation of the fix above, which
was inferred from the driver before the board was available. Second, `APPCTRL`
carries the `0xF807` firmware-up handshake exactly as we model it. Third, and
most relevant to the policy call: **real hardware reports `error_code = 0` on a
successful inference.** So our default of 0 is *faithful* — turning on the
honest-fault errcode is a deliberate, deliberate-and-documented departure from
silicon, trading fidelity-of-the-register for honesty-about-the-result. That is
exactly the trade the directive exists to adjudicate, and it is Kyle's call.

One further delta: silicon posts `MBOX0 = 0xA3` (`RUN_ACK`) before `DONE`; our
model jumps straight to `DONE`. The driver only tests `MBOX0 != 0` for tx-ack
and `== DONE` for completion, so both work — but the intermediate ack is real.

## Follow-ups not done tonight

- **The netdev-naming race probably affects more tests.** `run-2port.sh` and the
  interconnect `run-eth.sh` are fixed. `tests/netc/load-soak-2port.sh` (13
  hard-coded `ethN` references, no speed-based identification) is the most
  exposed; `load-soak.sh` and `code-sweep/run-peripheral.sh` also name-match.
  The 10G tests already identify ports by reported speed and are safe. Not fixed
  here because a soak takes too long to validate in one sitting, and an unvalidated
  change to a soak is worse than a known-flaky one.
- **IPv6 extension headers** are not walked in the TX checksum path: if the next
  header is neither TCP nor UDP the frame is left untouched rather than
  corrupted. Fine today; would matter for IPsec/fragmentation.
- **`ptp_netc` PPS / periodic output / external timestamp** are unmodelled and
  degrade honestly (no event ever fires). `TMR_STAT.ETSx_VLD` must stay clear.

## Open questions for Kyle

1. **Flip the Neutron default?** Making `neutron-uncomputed-errcode` non-zero by
   default trades "the stack runs end to end" for "the guest is never lied to".
   The directive says silent-wrong is the worst class. This is the one real
   policy call left, and it is yours.
2. **uSDHC capabilities**: leave conservative, or model uSDHC's own capability
   register properly (not the generic SDHCI default) and only advertise modes we
   can honour?
3. **VPU window size** — trim `0x40000` to the real `0x10000`?
4. **Model the Root Complex Event Collectors** (`1131:e001`)? Low value; nothing
   depends on them beyond `lspci` symmetry.

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
