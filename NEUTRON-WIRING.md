# How the Neutron NPU is wired up in QEMU i.MX 95 (start here)

**Audience:** an engineer (or agent) about to plumb a real Neutron *execution
engine* — a runner that actually computes inferences — into this emulator, the
way Arm's Ethos-U runner was dropped into the sibling i.MX 93 emulator.

**What this document is:** a ground-truth description of how the Neutron model
works **today** and exactly **what it expects**, so you can orient in minutes
before touching code. It is deliberately short. When you are ready to build the
engine, the full contract — the seam signature, the carveout memory model, the
threading rules, the test plan — is in
[`docs/neutron-full-execution-integration.md`](docs/neutron-full-execution-integration.md).
This file is the map; that file is the spec.

---

## TL;DR

- The i.MX 95 Neutron NPU is a **firmware-driven accelerator**. Linux's
  `drivers/staging/neutron` loads `NeutronFirmware.elf` onto the NPU's own core
  via remoteproc, then drives inference over a **register mailbox**.
- The NPU core ISA, the firmware, and the compiled-model command stream are all
  **NXP-proprietary**, so the *compute* cannot be modelled from public material.
- What the model **does** implement is the **entire software bring-up path**: the
  remoteproc handshake, the clock gate, the two-phase mailbox, the completion
  IRQ. The stock driver binds, `NeutronFirmware.elf` loads into modelled TCM, and
  the TFLite / LiteRT Neutron delegate runs `benchmark_model` inferences **to
  completion** — but the output buffer is **not computed**.
- Because an uncomputed result that looks successful is the worst fidelity bug
  class (a *silent wrong answer*), the model **faults honestly by default**: every
  uncomputed inference returns a recognisable non-zero `error_code` (`0x95E0`) in
  the mailbox, which the driver surfaces to userspace. A guest that trusts NPU
  output learns it did not really compute. This is a one-line opt-out (below).
- **Your job:** replace the one place the model *acks without computing* with a
  real engine. Everything around that seam — MMIO, mailbox protocol, doorbell,
  IRQ, TCM, remoteproc, the two-phase ACK/DONE timing, the DMA carveout the
  driver hands you — is already built, tested, and merged on `imx95-netc`.

---

## Where everything lives

| Path | What it is |
|---|---|
| `hw/misc/imx95_neutron.c` | **The model.** RESETCTRL clock gate, mailbox responder, doorbell, two-phase RUN_ACK→DONE, honest-fault errcode, TCM, IRQ. ~460 LOC, type `imx95.neutron`. |
| `hw/arm/fsl-imx95.c` (~line 649) | **SoC wiring.** Instantiates `imx95.neutron`, maps the 4 windows, connects GIC SPI 318, exposes it at QOM path `/machine/soc/neutron`. |
| `tests/qtest/imx95-neutron-test.c` | **Kernel-free correctness oracle.** Drives the clock-on handshake → doorbell → two-phase RUN_ACK→DONE, and both honest-fault modes. This is where a real engine's bit-exact test goes. |
| `tests/neutron/run.sh` + `README.md` | **Full-stack integration.** Boots Linux, enables the NPU via a dtb splice, runs `benchmark_model` through the real delegate end to end. |
| `docs/neutron-full-execution-integration.md` | **The engine integration spec** (the deep contract; read after this). |
| `docs/imx95/known-limitations.md` / `docs/validation/fidelity-audit.md` | Where the "brings up, does not compute" ceiling is recorded. |

The model is a **single self-contained file** with a clean internal shape. There
is no hidden state elsewhere; the SoC file only maps it and wires the IRQ.

---

## Memory map + IRQ

Four sysbus MMIO regions (registered in `neutron_realize`), mapped in
`fsl-imx95.c`:

| Region | Base | Size | Backing | Purpose |
|---|---|---|---|---|
| RESETCTRL | `0x4ab00000` | 4 B | MMIO | Clock gate. Driver turns ZENV clock on here; model answers with the "firmware up" handshake. |
| Device / mailbox | `0x4ab00004` | `0x400` | MMIO (`regs[]`) | The register file: control, status, mailbox, base registers. |
| DTCM | `0x4ab08000` | 32 KiB | **RAM** | `NeutronFirmware.elf` data segment lands here (remoteproc `memcpy_toio`). |
| ITCM | `0x4ab10000` | 64 KiB | **RAM** | `NeutronFirmware.elf` code segment lands here. |

**IRQ: GIC SPI 318** (the inference-completion / mailbox interrupt).

TCM is RAM-backed *only* so `rproc_elf_load_segments` can copy the firmware image
in without faulting. **The firmware is never executed** — it sits in TCM for
bring-up fidelity; the compute is (will be) a host-side reimplementation, exactly
as on the i.MX 93 Ethos-U side.

---

## The mailbox register map (device window, offsets from `0x4ab00004`)

Names/offsets mirror `neutron_device.h` in `drivers/staging/neutron`. The mailbox
is 8 contiguous scratch registers `MBOX0..MBOX7` plus control/status/base
registers around them. The ones the model acts on:

| Name | Offset | Role |
|---|---|---|
| `STATUSERR` | `0x000` | Status/error (model returns 0 = no fault). |
| `INTENA` | `0x004` | IRQ enable. Non-zero → the completion line can assert. |
| `INTCLR` | `0x008` | IRQ clear (driver ack lowers the line). |
| `APPCTRL` | `0x1fc` | **Doorbell = bit 2.** `[31:16]` carries the firmware "up" handshake (`0xF807`). |
| `APPSTATUS` | `0x200` | Event flags: `INFDONE` (bit 0), `MBOX` (bit 4). Driver W1C-clears. |
| `BASEDDRL/H` | `0x204/0x208` | Carveout physical base (low/high). |
| `MBOX0` | `0x23c` | **Response retcode** — the completion word the driver polls. |
| `MBOX1` | `0x240` | **Response `error_code`** — surfaced to the guest app (`INFERENCE_STATE`). |
| `MBOX3` | `0x248` | **Command word** (written last; the doorbell triggers on it). |
| `BASEINOUTL/H`, `BASESPILLL/H` | `0x27c…/0x284…` | In/out and spill bases (driver writes the carveout base to all three). |

**Key magic values** (`neutron_device.h`):

| Constant | Value | Meaning |
|---|---|---|
| `RESET_VAL` | `0x0` | MBOX0 idle / post-reset. Driver treats MBOX0 ≠ 0 at job start as "stuck". |
| `RUN_ACK` | `0xA3` | "Doorbell received, running" — the synchronous TX ack. |
| `DONE` | `0xAD0` | Inference complete — the retcode the completion path waits for. |
| `CMD RUN` | `0x269` | Run an inference. |
| `CMD RESET` | `0x23637` | Re-arm the mailbox (sent *without* a doorbell). |

`INTENA`/`INTCLR` bits: `INFERENCE_DONE = BIT(1)`, `MBOX = BIT(2)`,
`SHUTDOWN = BIT(7)`. `RESETCTRL` clock bits: `ZENV_CLK_ON = BIT(0)`,
`COMPUTE_CLK_ON = 0xF<<4`, `TCM_CLK_ON = 0xF<<8`.

---

## The lifecycle, exactly as the model runs it today

This is the whole state machine. Follow it register by register:

```
1. remoteproc start
   driver → RESETCTRL |= ZENV_CLK_ON (0x1)
   model  → s->started = true; APPCTRL[31:16] = 0xF807   (firmware "I'm up")
   driver polls APPCTRL[31:16] == 0xF807, proceeds.

2. Submit an inference (drivers/staging/neutron → neutron_inference_run)
   driver → writes carveout base to BASEDDR / BASEINOUT / BASESPILL (all three)
   driver → MBOX4/5/6 = {tensor_offset, microcode_offset, tensor_count}
   driver → MBOX3     = RUN (0x269)          (command word, written last)
   driver → APPCTRL  |= bit2                 (rings the doorbell)

3. Two-phase mailbox  ── neutron_doorbell()  [synchronous, vCPU thread]
   model  → MBOX0 = RUN_ACK (0xA3)           ← unblocks the driver's ~20 µs
                                                mbox_send_data "MBOX0 != 0" poll
   model  → arms done_timer (NEUTRON_RUN_NS = 1000 ns)   [PLACEHOLDER — see below]

4. Completion  ── neutron_done()  [deferred, timer callback]
   model  → MBOX0 = DONE (0xAD0)             ← the retcode the driver waits on
   model  → APPSTATUS |= INFDONE | MBOX
   model  → MBOX1 = uncomputed_errcode (0x95E0 by default)   ← honest fault
   model  → raise SPI 318 (if INTENA set)
   *** THE OUTPUT BUFFER AT BASEINOUT IS LEFT UNTOUCHED — nothing was computed ***

5. Driver handles DONE, then re-arms
   driver → reads MBOX0 == DONE (poll or ISR), reads MBOX1 = error_code
   driver → MBOX3 = RESET (0x23637)          (NOT a doorbell — polled)
   model  → neutron_dev_write: timer_del(done_timer); MBOX0 = RESET_VAL (0)
   Next job's "stuck" check (MBOX0 == 0) passes.

MBOX0 life:  0 → 0xA3 (RUN_ACK) → 0xAD0 (DONE) → [driver RESET] → 0
```

Two subtleties that already bit us and are handled — do not regress them:

- **The two-phase split is load-bearing.** The driver's `mbox_send_data` polls
  `MBOX0 != 0` for only ~20 µs and returns `-ETIME` otherwise. So the ack **must**
  be synchronous in the doorbell; the completion lands later. Today the "later"
  is a 1 µs timer (`done_timer` → `neutron_done`) — a placeholder, since there is
  no real compute to wait for. **This is exactly the seam a real runner slots
  into:** replace the timer with a worker thread that computes, then posts DONE.
- **RESET must drive MBOX0 back to 0.** RESET is the one command the driver does
  *not* doorbell (`mbox_send_reset` writes MBOX3 and polls). If the model does not
  re-arm MBOX0 to 0, the driver declares "failed to reset neutron state" and
  hardware-resets + reloads firmware before *every* inference. `neutron_dev_write`
  handles this (and cancels any in-flight `done_timer` so it can't clobber the
  RESET).

---

## The honest-fault knob (why an uncomputed inference is not silent)

The driver completes purely on `MBOX0 == DONE`; it then reads `MBOX1` as an
`error_code` and copies it to userspace via `NEUTRON_IOCTL_INFERENCE_STATE`. So a
**non-zero MBOX1 alongside MBOX0 == DONE** means "this inference did not really
compute" **without hanging the driver**.

- **Default (`0x95E0`)** — honest fault. Reads as "95, Error, 0"; recognisable in
  a guest's `INFERENCE_STATE`. A guest that trusts NPU output learns it was
  uncomputed rather than getting garbage that looks valid.
- **Opt-out (`0`)** — silicon-faithful happy-path success, for a guest that
  tolerates uncomputed output (e.g. a delegate that offloads 0 nodes and falls
  back to CPU). Set at runtime:

  ```
  qom-set /machine/soc/neutron neutron-uncomputed-errcode 0
  ```

The value is **operator config**, not device state — it is seeded in
`instance_init` and deliberately *not* cleared on reset, so a `-global` / `qom-set`
override survives a guest reboot. Two more runtime-queryable honesty signals hang
off the same QOM node: `compute-modelled` (always `false` today — flip it when
your engine lands) and `inferences-acked-uncomputed` (a counter).

**When your engine computes for real, this whole mechanism inverts cleanly:** the
engine returns a real `retcode`/`error_code`, `compute-modelled` becomes `true`,
and the honest-fault default becomes a genuine success (`error_code = 0`).

---

## How to run what exists

**Kernel-free qtest (the correctness oracle — no Linux, seconds):**

```
# from a configured build/ dir
meson test -C build qtest-aarch64/imx95-neutron-test        # or:
QTEST_QEMU_BINARY=./build/qemu-system-aarch64 ./build/tests/qtest/imx95-neutron-test
```

It asserts the clock-on handshake (`APPCTRL[31:16] == 0xF807`), the two-phase
RUN_ACK→DONE transition (RUN_ACK synchronously, DONE only after the servicing
delay, with the event flags), and both honest-fault modes (`0x95E0` default, `0`
after opt-out). **This is the file a real engine extends** with a bit-exact
"stage program+weights+input → RUN → assert output against a golden" test.

**Full-stack integration (boots Linux, runs the real delegate):**

```
KBUILD=<kernel build>  SM_ELF=<m33_image.elf>  \
BSP_ROOTFS=<imx-image-full rootfs>   ./tests/neutron/run.sh
```

It boots with the NPU enabled (dtb splice adds the carveout + strips the SMMU
phandle), loads `NeutronFirmware.elf` via remoteproc, and runs `benchmark_model`
through the LiteRT Neutron delegate. With today's **stock, unconverted** model
the delegate offloads 0 nodes (CPU fallback, correct results) — this proves the
whole datapath binds. A **neutron-converted** `.tflite` is what will exercise a
real engine (see the spec §7, §9). SKIPs cleanly if the NXP userspace is absent.

---

## The fidelity ceiling and exactly where your runner plugs in

Today the model **acks every inference DONE without computing**. The single place
this happens is `neutron_done()` in `hw/misc/imx95_neutron.c` — it posts DONE +
the honest-fault errcode and leaves the output buffer untouched. The submission
seam is `neutron_doorbell()`, which already does the synchronous RUN_ACK and
snapshots the command.

To make the NPU compute, you replace the **timer-based DONE deferral** with a
**worker thread that runs a real engine** and then posts DONE — mirroring how the
i.MX 93 emulator runs Ethos-U. Concretely (all detailed in the spec):

- Keep `imx95_neutron.c` as the plumbing. Add `hw/misc/imx95_neutron_compute.c`
  (+ header) holding the engine behind **one function**:

  ```c
  void neutron_compute_run(const NeutronComputeReq *req, NeutronComputeRes *res);
  ```

- The engine reads the program at `base_ddr + microcode_offset`, walks the tensor
  descriptor table at `base_ddr + tensor_offset`, computes, and DMA-writes the
  output tensors back into the carveout — **all guest memory via an
  `AddressSpace`, never a host pointer.** The two proprietary pieces (the
  microcode ISA and the tensor-descriptor byte layout) are the only things you
  bring from inside NXP; every register, offset, and contract around them is
  pinned down in the spec.
- Run it **off the vCPU thread** (a detached worker, like `hw/misc/edu.c` and the
  93 Ethos-U model) so a multi-millisecond inference never stalls the guest. The
  two-phase mailbox already gives you the ack-now / done-later structure to hang
  this on — you are swapping the placeholder timer for the worker, not inventing
  the timing.

**What you inherit vs. what you build:**

| Already done (fixed outer wall) | You build (inside the box) |
|---|---|
| MMIO regions, IRQ (SPI 318), TCM, remoteproc handshake | The microcode interpreter + tensor-descriptor parser |
| Mailbox protocol, doorbell, command decode | The int8/int16 MAC kernels + requant math |
| **Two-phase RUN_ACK → DONE timing** (swap the timer for your worker) | The DMA marshalling (read inputs/weights, write outputs) |
| RESET re-arm, stuck-detection contract | Golden vectors for the bit-exact qtest |
| Honest-fault MBOX1 (becomes your real `error_code`) | Op coverage matched to your converter's emissions |
| The DMA carveout the driver builds and hands you | — |

**The proven precedent to mirror** is the i.MX 93 emulator's `hw/npu/ethos_u*.c`
— a three-layer plumbing / pure-decoder / pure-kernels split with a detached
worker thread. The spec's §2 distills its architecture; copy that shape.

---

## Read next

[`docs/neutron-full-execution-integration.md`](docs/neutron-full-execution-integration.md)
— the full engine integration spec: the seam signature and its contract (§4), the
carveout memory model and what the engine must produce (§5), the threading rules
copied from the 93 side (§6), the on-device runtime path (§7), the test &
validation plan (§9), and the authoritative kernel/delegate source pointers (§11).
