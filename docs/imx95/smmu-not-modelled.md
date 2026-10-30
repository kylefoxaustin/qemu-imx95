# qemu-imx95 — Why the Arm SMMU-v3 is not modelled

A decision record. `known-limitations.md` states *what* this gap costs you when
you use the machine; this file states *why* the gap is still here after two
attempts to close it, what each attempt measured, and what closing it would
actually require.

It exists because the first attempt's failure was misdiagnosed, and that wrong
diagnosis sat in the project's notes for four months. Anyone picking this up
should start from the real constraint rather than the remembered one.

Every number below carries a provenance tag. **[MEASURED]** — run here or on
real silicon. **[DERIVED]** — computed, or established by reading two sources
against each other. **[SOURCED]** — taken from an upstream device tree or a
vendor document. A derived or sourced number is never compared against a
measured one.

---

## 1. What the hardware has

The i.MX 95 integrates one Arm SMMU-v3 at `iommu@490d0000`, with a 1 MiB reg
window and four SPIs — 325 (`eventq`), 326 (`cmdq-sync`), 328 (`gerror`) and
334 (`priq`) [SOURCED — `imx95.dtsi`].
The stock upstream `imx95.dtsi` places **10 DMA masters behind it, covering 31
stream IDs** [SOURCED — `arch/arm64/boot/dts/freescale/imx95.dtsi`]:

| master | node | property | stream IDs | count |
|---|---|---|---|---|
| eDMA3 | `dma-controller@42210000` | `iommus` | `0x0` | 1 |
| uSDHC1 | `mmc@42850000` | `iommus` | `0x1` | 1 |
| uSDHC2 | `mmc@42860000` | `iommus` | `0x2` | 1 |
| uSDHC3 | `mmc@428b0000` | `iommus` | `0x3` | 1 |
| Neutron NPU | `imx95-neutron@4ab00004` | `iommus` | `0xd` | 1 |
| USB3 (dwc3) | `usb@4c100000` | `iommus` | `0xe` | 1 |
| USB2 (ChipIdea) | `usb@4c200000` | `iommus` | `0xf` | 1 |
| pcie0 | `pcie@4c300000` | `iommu-map` | `0x10`-`0x17` | 8 |
| pcie1 | `pcie@4c380000` | `iommu-map` | `0x18`-`0x1f` | 8 |
| NETC / ENETC | `pcie@4ca00000` | `iommu-map` | `0x20`-`0x27` | 8 |

The three `iommu-map` entries matter more than they look, and section 4 returns
to them.

On a real FRDM-IMX95-PRO the BSP runs these masters **translated and in strict
mode**: 13 IOMMU groups, `features 0x01f4deaf`, `ias 40` / `oas 36`
[MEASURED — FRDM-IMX95-PRO]. Device addresses there are IOVAs and never equal
physical addresses.

---

## 2. What qemu-imx95 does instead

`iommu@490d0000` is a **declared stub** (`hw/misc/imx95_smmu_stub.c`) that reads
as zero. Reads-as-zero is load-bearing in two directions:

- U-Boot SPL calls `disable_smmuv3()`, which polls `GBPA.UPDATE` clear, writes
  `GBPA` then `CR0 = 0`, and polls `CR0_ACK == 0`. Zero satisfies all three
  polls, so the SMMU is left disabled exactly as it is on silicon after SPL.
- Linux's `arm-smmu-v3` reads `IDR0`, finds neither S1P nor S2P, prints
  `no translation support!` and fails probe with `-ENXIO`.

The second outcome is deliberate: an SMMU that half-binds and whose stream table
we would not honour is worse than one that visibly refuses (section 5).

One known narrowness: the dtsi declares a 1 MiB reg window, while the stub maps
128 KiB — enough for both register pages `arm-smmu-v3` ioremaps, but not the
whole declared window. Nothing in the kernel or SPL reaches past that today; a
guest that mapped the full window and probed beyond 128 KiB would see unassigned
memory rather than reads-as-zero. Widening the mapping is a one-line change if
that ever matters.

Because no IOMMU registers, the masters fall back to direct DMA ops. **This does
not stop them binding.** `iommus` and `iommu-map` are *optional* `fw_devlink`
supplier bindings, so each consumer takes `-EPROBE_DEFER`, waits out
`driver_deferred_probe_timeout` (10 s), warns `deferred probe timeout, ignoring
dependency`, and then binds identity-mapped — `of_dma_configure` falls through
to direct DMA ops, where IOVA == PA. Measured on a plain boot:

```
[   12.301983] fsl_enetc4 0002:00:10.0: deferred probe timeout, ignoring dependency
[   12.405620] fsl_enetc4 0002:00:10.0: enabling device (0000 -> 0002)
```

[MEASURED — `tests/netc/run.sh`]

An earlier version of this project's notes claimed those devices "never probe"
or "can never bind". That was wrong, and wrong in the dangerous direction: it
implied a visible failure where the real behaviour is a silent success. Six
places in the tree said so and have been corrected.

The gap is therefore kept honest by **announcement**, in three places:

1. The stub warns once on the guest's first `IDR0` read, naming the masters and
   the bug classes that pass here and fault on hardware. It fires on `IDR0` only
   — SPL touches just `GBPA`/`CR0`/`CR0_ACK`, so U-Boot boots stay quiet.
2. `hw/arm/imx95-evk.c` audits a supplied dtb and warns, naming each master, if
   the dtb has an `arm,smmu-v3` node but has had the masters' IOMMU properties
   stripped. Such a dtb must never be booted on silicon, where unattached
   streams get ABORT stream-table entries.
3. `README.md` and `known-limitations.md` state the gap and its direction.

Warning 1 fires on *every* Linux boot, because the stock dtb always carries the
`arm,smmu-v3` node even when a test has stripped the masters' properties. A
harness that treats stderr output as a failure can opt out with
`-global driver=imx95.smmu-stub,property=quiet,value=true`. The long form is
required: `-global`'s shorthand splits on the first `.`, so
`-global imx95.smmu-stub.quiet=true` is rejected with "invalid class name". It
suppresses only the host-side announcement — the guest still prints
`no translation support!` and DMA is identity either way
[MEASURED — stderr warnings 2 → 0, guest output unchanged].

Warning 2 is deliberately **not** gated. It is silent for anyone on a stock dtb
and speaks only when that specific dtb has actually been altered, which is the
one case that should be hard to silence.

---

## 3. What the gap costs — plainly

This is the project's largest *silent-wrong* exposure, and it fails in the
**false-pass** direction: code that is wrong about DMA addressing works here and
faults on the real part. The classes are:

- a CPU physical address used where a `dma_addr_t` is required;
- DMA after `dma_unmap`;
- an under-sized mapping;
- a new master, or an ENETC VF, with no IOMMU entry at all;
- IOVA leaks.

**The sharpest single vector is the Neutron NPU's `base_ddr` ioctl field**,
which the kernel writes to hardware unvalidated. A physical address crosses that
ABI and this machine accepts it. That one is independent of everything else in
this document: closing the SMMU gap would expose it, and not closing the SMMU
gap does not make it safe.

Nothing in the announcement machinery fixes any of this. The warnings help a
developer reading a console. They do nothing for a customer whose image works
here and faults on silicon.

---

## 4. Why not simply instantiate QEMU's SMMU-v3

QEMU ships a complete, correct SMMU-v3 — `hw/arm/smmuv3.c` plus
`hw/arm/smmu-common.c` implement stage-1 and stage-2 page-table walks, STE and
CD fetch, the command and event queues, and an IOTLB with ASID/VMID
invalidation. The architecture is not the problem.

The problem is that QEMU's IOMMU plumbing assumes **IOMMU means PCI**, which is
true on the `virt` board and false on every real SoC. Three structural gaps:

1. **`SMMUDevice` has nowhere to put a stream ID.**
   `include/hw/arm/smmu-common.h` stores `PCIBus *bus` + `int devfn`, and
   `smmu_get_sid()` *computes* the stream ID as
   `PCI_BUILD_BDF(pci_bus_num(sdev->bus), sdev->devfn)`. A platform master has
   no BDF, so it cannot be represented.
2. **`primary-bus` is mandatory and must be a PCIe root complex.**
   `smmu_base_realize()` in `hw/arm/smmu-common.c` fails with
   `"SMMU is not attached to any PCI bus!"` when the link is unset, and then
   insists the bus be a root PCIe bus or a `pxb-pcie`. One SMMU therefore serves
   exactly one PCI bus and nothing else.
3. **No accessor.** Nothing hands board code an `IOMMUMemoryRegion *` for a
   stream ID of its choosing.

Note what `smmu_init_sdev()` actually does with `bus`/`devfn`: it uses them to
build the memory region's *name*, and nothing else. The `IOMMUMemoryRegion` and
`AddressSpace` it creates are already master-agnostic. The PCI-ness is skin
deep — which is why the fix in section 7 is small.

**And one piece of real functionality is missing: `iommu-map` arithmetic.**
Generic SMMU-v3 hard-assumes stream ID == BDF, i.e. an `iommu-map` of
`<0x0 &smmu 0x0 0x10000>`. The i.MX 95 does not do that — it gives pcie0 base
`0x10`, pcie1 base `0x18` and NETC base `0x20`. So for NETC `0002:00:10.0`,
whose devfn is `0x80`, QEMU would key its stream-table entry at stream ID
**`0x80`** while Linux programs one at **`0x24`** [DERIVED — `smmu_get_sid()`
read against the dtsi `iommu-map`; not yet confirmed by trace]. Nothing errors
when that happens: the lookup simply misses.

A separate fidelity point, easy to miss: even fully plumbed, QEMU's SMMU-v3
advertises `features 0x00008205`, `ias 44` / `oas 44` [MEASURED — in-tree spike],
where the real part reports `features 0x01f4deaf`, `ias 40` / `oas 36`
[MEASURED — FRDM-IMX95-PRO]. The ID registers would need overriding to match the
integration, and 11 IOMMU groups formed in the spike against 13 on silicon — a
discrepancy that has not been explained.

---

## 5. What the two spikes actually measured

**2026-06.** An integration spike attached `TYPE_ARM_SMMUV3` to the NETC PCI bus
and hit an abort during GIC wiring. It was recorded as *"MSI/ITS INTID
corruption"* and the branch was reverted.

**That diagnosis was wrong.** The real cause was the machine's GIC SPI budget:
`FSL_IMX95_NUM_IRQS` was 320, while the silicon implements 384 and the stock DT
wires SPIs up to 375. QEMU's GICv3 lays its incoming GPIO array out as
`[0..N-1]` SPIs followed by `[N..N+31]` PPIs per CPU, so wiring an SPI at or
above `N` does not fail — it **silently delivers as a CPU0 PPI**, and anything
landing in the SGI range trips the assert in `gicv3_set_irq()`. The SMMU's four SPIs
are 325-334, all above the old budget of 320.

The budget is now 384, matching silicon
[MEASURED — guest prints `GICv3: 384 SPIs implemented`, byte-identical to the
FRDM dmesg]. That fix also repaired a second, latent defect: ISI channels 1-7
were wired at SPIs 344-350 and were landing on CPU0 PPIs 24-30. No current guest
claims those lines, so the ISI fix is preventive, not curative.

**2026-10.** Re-running the same spike with the budget corrected gets much
further than the record suggested. On the **stock** dtb, with no patching and no
`iommu.passthrough` [all MEASURED — in-tree]:

- no abort; `arm-smmu-v3` probes cleanly;
- 11 IOMMU groups form, platform masters included;
- zero translation faults; boot reaches userspace;
- devices bind at **0.8 s** instead of 12.3 s.

It is still not shippable, for a sharper reason than before. Because QEMU's
SMMU-v3 translates only its single `primary-bus`, a master that *keeps* its
`iommus` property lands in a translating domain that Linux programs and QEMU
does not honour:

| test | result | why |
|---|---|---|
| `tests/sd-emmc/run.sh` | **FAIL** | keeps `iommus`; gets a translating domain QEMU ignores |
| `tests/netc/run-2port.sh` | **FAIL** | the original 2026-06 symptom, reproduced |
| `tests/usb3/run.sh` | PASS | **only because the test strips the property** |
| `tests/neutron/run.sh` | PASS | **only because the test strips the property** |

That last column is the whole finding. `sd-emmc` is the only one of the three
that leaves its IOMMU property alone, and it is the only one that fails — so the
configuration breaks precisely the masters that are configured honestly.

**A partially-plumbed SMMU is worse than none.** Identity mapping is at least
self-consistent. A half-plumbed SMMU makes Linux believe translation is
happening while QEMU does not do it, and raises no fault when they disagree.
That is the worst class of defect this project recognises.

The experiment is preserved on branch `smmu-spike-2`, marked DO NOT MERGE, with
the measurements in its commit message. That branch is **not published** — it is
retained in the maintainer's tree only, because a DO-NOT-MERGE branch on the
public remote is an invitation to merge it. Ask if you need it.

---

## 6. Why not write a custom i.MX 95 SMMU

Because the architecture is not what is missing, and having the specification in
hand does not change that. A from-scratch `imx95-smmu` would mean reimplementing
STE and CD walks, both translation stages, the command and event queues, and
IOTLB invalidation semantics — all of which Linux's `arm-smmu-v3` exercises in
full, and all of which already exist and are already correct in QEMU.

The failure mode of getting any corner of that subtly wrong is not a crash. It
is a **wrong translation**: plausible-looking DMA to the wrong address, with no
fault. Writing several thousand lines of new architecture code whose worst
failure mode is silent data corruption, in order to avoid writing roughly 150
lines of plumbing, is a bad trade in every direction.

---

## 7. What closing it would actually take

Extend, do not fork. The changes should be **additive** — a new field, a new
function, an optional property, with the existing PCI path untouched — so that
rebases onto QEMU master stay quiet and the work stays upstreamable. This is a
genuine generic gap: every real Arm SoC with an SMMU has platform masters, and
QEMU has not needed to express that because `virt` does not.

Four pieces:

1. an explicit stream ID in `SMMUDevice`, used by `smmu_get_sid()` when set;
2. `primary-bus` made optional, with a stream-ID-keyed lookup alongside the
   existing PCI-bus-keyed one (`smmu_find_sdev()` currently reverse-maps a
   stream ID through the PCI bus array, which a platform master cannot use);
3. an accessor returning the `IOMMUMemoryRegion` for a chosen stream ID;
4. `iommu-map` arithmetic — `base + (rid & mask)` — so a PCI bus can carry a
   stream-ID base other than zero.

**An unmapped stream ID must abort and record an event, never silently bypass.**
This is not optional: it is the only thing standing between a mis-wired master
and section 5's worst case.

The consumer side is close to free, which is the part most likely to surprise
someone estimating this work:

| master | how it gets its DMA address space | work |
|---|---|---|
| uSDHC1-3 | `dma` MEMORY_REGION link property already exists (`hw/sd/sdhci.c`) | board link only |
| USB3 (xhci-sysbus) | same `dma` link pattern | board link only |
| USB2 (EHCI/ChipIdea) | indirects through `ehci->as` | expose a link |
| Neutron NPU | `hw/misc/imx95_neutron.c` sets `req->as = &address_space_memory` | one line |
| eDMA3 | not yet checked | unknown |
| pcie0/1, NETC | `pci_dma_*`, so automatic via `pci_setup_iommu` | stream-ID mapping (4) |

**This flips an earlier verdict recorded in this project.** "There is no
incremental path" was true of the *generic* device only. With a per-master
attach, every master is in one of two honest states — attached and genuinely
translated, or still dtb-stripped and honestly identity-mapped. The lethal third
state (dtb says translated, QEMU does identity, nothing faults) disappears, and
enabling one master at a time becomes legal.

**The hard part is verification, not construction.** A wrong stream ID does not
announce itself: the STE lookup misses and aborts without recording an event, or
hits another master's entry and translates through its page tables. Any attempt
needs instrumentation before it needs code — the trace events already exist
(`smmuv3_cmdq_cfgi*`, `smmuv3_translate_abort`), and a real FRDM board is
available to compare IOMMU groups against.

If resumed, the order that keeps the cost bounded:

1. A traced `tests/netc/run-2port.sh` first, to turn section 4's `0x80` vs `0x24`
   derivation into a measurement. If the mechanism is something else, everything
   below is aimed at the wrong target. This is about an hour.
2. The additive stream-ID patch, with abort-on-unmapped.
3. Wire **uSDHC only**, and run `tests/sd-emmc/run.sh` with its `iommus` property left
   intact. It is the one test that fails honestly today, which makes it the best
   oracle available. Pass there and the design is proved; fail and the cost so
   far is two days.
4. Measure host CPU and memory per instance before the change grows. The
   board-farm density target depends on that number, and every DMA access would
   now walk an IOMMU region.
5. Read the real `IDR0`-`IDR5` off an FRDM rather than trusting any document.
   Precedent: on the Mali GPU model a wrong `arch_minor` produced an incomplete
   register map and a NULL-pointer oops in the guest driver, not a clean
   failure.

---

## 8. The decision

**The SMMU-v3 is deliberately not modelled, and work on modelling it is
parked.** The reason is not difficulty — section 7 is a small, boring change.
The reason is that it touches the DMA path of every master in the machine, and
this repo has users whose working configurations should not be disturbed for a
gap that is now documented rather than silent.

What shipped instead: the GIC budget corrected to match silicon, the gap
announced in three places, the wrong 2026-06 diagnosis retired, and the
false-pass risk written down where a user will meet it.

What would reasonably reopen it:

- a customer image that passes here and faults on silicon in a way traceable to
  DMA addressing — the failure this gap is a proxy for;
- SMMU fault reporting or the PMCG counters becoming a requirement rather than
  a nice-to-have;
- upstream QEMU growing per-master IOMMU attach for its own reasons, which would
  reduce the work to board wiring.

Until then the honest position is the one this document takes: the machine does
not translate DMA, it says so, and the places where that matters are named.

---

## See also

- `docs/imx95/known-limitations.md` — this gap's user-facing impact, in context
  with the machine's other limitations.
- `hw/misc/imx95_smmu_stub.c` — the stub, and the reasoning for reads-as-zero.
- `hw/arm/imx95-evk.c` — `imx95_evk_audit_iommu_dtb()`, the dtb audit.
- `include/hw/arm/fsl-imx95.h` — the GIC SPI budget and why it is load-bearing.
- branch `smmu-spike-2` — the preserved experiment, unpublished. DO NOT MERGE.
