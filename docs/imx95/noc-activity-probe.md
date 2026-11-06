# qemu-imx95 — measuring interconnect activity

How to get per-initiator DMA transaction counts out of this machine, what they
are good for, and — the part worth reading before you plan work on this — what
they cannot tell you.

Provenance tags below: **[MEASURED]** run here, **[DERIVED]** computed,
**[SOURCED]** from a document. A derived number is never compared against a
measured one.

---

## 1. Turning it on

```
QEMU_DMA_ACCOUNT=1                             # required; totals at exit
QEMU_DMA_ACCOUNT_CSV=/path/to/out.csv:100      # optional; windowed CSV, ms
```

`QEMU_DMA_ACCOUNT=1` is the master switch — the CSV is ignored without it. The
window defaults to 100 ms of **guest** time (`QEMU_CLOCK_VIRTUAL`), so rates
are per guest second.

Two practical traps, both hit while building this:

- **`tests/*/run.sh` discards QEMU's stderr** (`>/dev/null 2>&1`), so the exit
  totals vanish when you run a test. Use the CSV with those.
- **A qtest finishes before any window elapses**, so its CSV is empty. Use the
  exit totals there.

CSV columns: `guest_ms,dev,chan,dir,txns,bytes,bytes_per_s`. Idle rows are
skipped, so a quiescent window costs nothing.

---

## 2. What it can and cannot see

| source of traffic | visible? | why |
|---|---|---|
| instrumented device DMA | yes | the model calls `dma_account()` |
| un-instrumented device DMA | **no** | nothing calls it; see the gap list |
| CPU load/store | **never, by this route** | see below |

**CPU traffic has no hook here, and this is structural.** Guest RAM is reached
through a direct host pointer — `memory_access_is_direct()` in
`system/physmem.c` routes RAM to a plain `memcpy`, so no `MemoryRegionOps`
callback runs. Instrumenting the CPU side means a TCG plugin
(`qemu_plugin_register_vcpu_mem_cb`), with a cache model in front of it, since
without one every load looks like a DRAM access.

**Equally, TCG plugins cannot see device DMA**, and that is also structural:
every plugin callback is `qemu_plugin_register_vcpu_*` and the memory callback
takes a `CPUState`. A DMA access has no vCPU to attribute it to. The two
instruments are complementary and neither is sufficient.

So **the total is a lower bound, never a total.** A master absent from the
report is missing data, not an idle master. The exit report says so in as many
words, deliberately.

### Instrumented today

| initiator | channels | sites |
|---|---|---|
| `dpu` | `scanout`, `blit` | 23 |
| `enetc` | `txbd`, `txdata`, `rxbd`, `rxdata` | 6 |
| `edma3` | `m2m`, `tx`, `rx`, `periph`, `tcd` | 5 |
| `isi` | `capture` | 1 |

### Known gaps

| master | sites | note |
|---|---|---|
| uSDHC1-3 | 11 | in generic `hw/sd/sdhci.c`, shared with other boards |
| USB2 (EHCI) | 2 | |
| NeoISP | 2 | |
| USB3 (xHCI) | — | not yet surveyed |
| Neutron NPU | 0 | compute is a stub, so it issues no DMA to count |

---

## 3. Reading the numbers: burst size is per-master

`bytes / txns` is the mean burst size, and **its validity differs by master**,
because it reflects the model's access granularity rather than the hardware's:

| initiator | bytes/txn | is it meaningful? |
|---|---|---|
| `dpu`/`scanout` | 4320.0 [MEASURED] | yes — exactly one 1080-pixel scanline at 4 B |
| `enetc` | varies | plausible — one descriptor or one frame per access |
| `edma3` | 4.0 [MEASURED] | **no** — our model moves one element per access; real eDMA bursts |

Byte totals are sound everywhere. **Do not feed the eDMA3 transaction count to
a fabric model as a burst count.** If burst behaviour for eDMA3 matters, the
model's inner loop has to coalesce first.

---

## 4. How the counters were validated

Not against each other — against three independent oracles.

**DPU, against arithmetic.** On the DSI panel path at 1080x1920: 4320.0
bytes/transaction, exactly `1080 x 4`; and 8,294,400 bytes/frame, exactly
`1080 x 1920 x 4` [MEASURED]. Both to the byte.

**ISI, against the guest.** 3840.0 bytes/transaction and 1,843,200 bytes/frame
— and 1,843,200 is precisely the `used` length the guest's own V4L2 buffer
reports in `tests/camera` [MEASURED]. The guest and the host agree without
either being told the other's answer. (12 frames written while userspace
dequeues 5 is correct: capture runs continuously.)

**eDMA3, against a qtest.** `imx95-edma-test` drives a 24-byte memory-to-memory
transfer; the counter reports `rd=24 wr=24` in 6 transactions each [MEASURED],
and the peripheral rows come out as writes, confirming the direction is derived
correctly from which side has a zero offset.

### What the validation found

The DPU frame tick was running at **46.7 Hz with the period set to 60**
[MEASURED]. The byte count per frame was exactly right, so the rate had to be
wrong — which is the sort of thing an activity probe is for and an eyeball is
not.

Cause: Linux masks and unmasks the display interrupt once per frame inside
`handle_level_irq`. Each mask made the model drop the vblank deadline, and the
unmask re-armed from *now* — after the frame composite had run — so the
effective period was `PERIOD + composite` (16.67 + 4.46 ms [MEASURED]). The
error scaled with scene complexity, so `FGTIMESTAMP` advanced at a rate that
varied with what was on screen.

Fixed by keeping the phase across a transient mask:

| | fps | DPU read rate |
|---|---|---|
| before | 46.7 | 387.2 MB/s |
| re-arm tick from deadline | 47.1 | 390.7 MB/s |
| keep the phase | **60.2** | **499.2 MB/s** |
| analytic oracle at 60 Hz | 60.0 | 497.7 MB/s |

All [MEASURED] except the oracle row, which is [DERIVED] from resolution,
format and refresh.

The middle row is kept here on purpose. Re-arming the tick from the previous
deadline was the first diagnosis; it is a real improvement in isolation and it
moved the rate by 0.4 Hz. Only measuring the deadline's own advance — rather
than reasoning about the arming logic — separated the two mechanisms.

---

## 5. Latency and stalls: why not in QEMU

The recurring request is "make transactions to this region block for 50 ns."
The short answer is that QEMU has no timing model to block *in*, and bolting
one on where everything around it is untimed produces a confidently wrong
number.

TCG executes translated blocks as fast as the host allows; nothing accounts
cycles to a memory access. Without `-icount`, virtual time is host time, so a
stall is both meaningless and irreproducible. With `-icount shift=N` it becomes
expressible and deterministic — but a flat 50 ns inserted into a machine with
no cache hierarchy, no prefetcher, no bank conflicts, no refresh, no read/write
turnaround and no arbitration is a number that *looks* precise and is not.

The project's standing principle applies unchanged: **QEMU supplies activity,
the model supplies the physical quantity.** For timing that means QEMU supplies
transactions and a DRAM or NoC simulator (DRAMSim3, Ramulator) supplies latency
— computed from queueing and bank state, and labelled DERIVED.

The one case that genuinely needs in-QEMU stalls is when the delay must be
**guest-visible**: software timing itself, a real-time deadline test,
arbitration the guest should feel. Then the honest mechanism is `-icount` plus
converting the region of interest from RAM into an `io` MemoryRegion, so every
access takes a handler that advances virtual time. That is correct and
deterministic, unusably slow across all of DRAM, and fine for a bounded
aperture — a 16 MiB window standing in for a slower memory partition. Measure
the slowdown rather than estimating it.

---

## 6. Natural next steps

1. **Close the coverage gaps**, uSDHC first — it is the largest, though it
   lives in shared generic code.
2. **Record the target address** per transaction, so traffic can be binned by
   DDR region or channel. Today only `(dev, chan, dir)` is recorded. This needs
   call-site changes.
3. **Surface the counts through `imx95_ddr_pmu`**, which is already registered
   with Linux's `fsl_imx9_ddr_perf` driver and currently honest-zero. In-guest
   `perf stat -e imx9_ddr0/.../` would then work as it does on silicon, with no
   bespoke side channel.
4. **A TCG-plugin DMA callback** is the upstreamable version of all of this.
   The record format here was chosen to match what such a callback should emit,
   so the methodology does not change when the accounting moves.
5. **Determinism**: without `-icount`, MTTCG leaves ordering between vCPUs and
   DMA irreproducible, so traces are statistically useful but not bit-identical
   run to run. `-icount shift=auto` buys determinism at a speed cost.

---

## See also

- `include/hw/misc/dma-account.h` — the API and its limits.
- `hw/misc/dma-account.c` — implementation, including why accumulation is
  atomic.
- `hw/misc/imx95_ddr_pmu.c` — the perf PMU that should eventually carry these
  counts.
