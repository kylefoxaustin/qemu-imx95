# Enabling Full Neutron NPU Execution in QEMU i.MX95

**An integration specification for the Neutron execution engine**

Audience: a Claude Code agent inside NXP that has the proprietary Neutron details
(the microcode/command-stream ISA, the tensor-descriptor layout, the Glow-based
neutron-converter internals, the firmware compute loop). This document does
**not** contain those secrets and does not need to. It defines the *shape of the
box* — every interface, seam, register, and contract around the execution
engine — so that once you drop the engine in, a user can:

> boot Linux on QEMU i.MX95 → hand it an eIQ/neutron-converted `.tflite` →
> run the model fully on the emulated Neutron NPU and read back correct outputs.

Everything in this document outside the single "execution engine" box is already
built, tested, and merged on branch `main`. Your job is one well-bounded
C module behind a clean function-call seam.

---

## 0. TL;DR — what exists, what you build

| Layer | Status | Owner |
|---|---|---|
| QEMU device model: MMIO, mailbox protocol, doorbell, IRQ, TCM, remoteproc handshake | ✅ **done** (`hw/misc/imx95_neutron.c`) | us |
| Linux driver `drivers/staging/neutron` + `/dev/neutron0` ABI | ✅ stock NXP, unmodified | NXP kernel |
| eIQ → neutron-converter → `.tflite` with `NeutronGraph` custom op | ✅ host tool (proprietary) | NXP eIQ |
| TFLite delegate + `libNeutronDriver.so` userspace | ✅ stock NXP rootfs | NXP eIQ |
| Full software datapath: ioctls → DMA carveout → mailbox `RUN` → `DONE` ack | ✅ **proven** end to end | us |
| **The microcode interpreter / compute engine** (reads the program + tensors from guest DRAM, computes, writes outputs) | ❌ **the gap — you build this** | **you (internal NXP)** |

The current model ACKs every inference **without computing** (a bring-up stub).
You replace that short-circuit with a real engine. The seam is a single function
call (§4). Nothing else in the stack changes.

> **⚠ Current-state note (this spec was drafted against an earlier revision of
> `imx95_neutron.c`; the plumbing has since moved toward you — your job is
> smaller than some passages below imply).** Three things this doc describes as
> "you will add" or "the current stub does not do" are **already implemented** on
> `main` today. Read the doc with these corrections in mind:
>
> 1. **The two-phase mailbox is already in place.** `neutron_doorbell()` posts
>    `RUN_ACK` (`0xA3`) **synchronously**, then a timer (`done_timer` →
>    `neutron_done()`, `NEUTRON_RUN_NS = 1000 ns`) posts `DONE` (`0xAD0`) +
>    `APPSTATUS` + the IRQ. So the ACK-now / DONE-later structure §6 tells you to
>    build **exists** — you replace the *placeholder timer* with a worker thread
>    that computes, not the single synchronous DONE that older passages (§3, §6)
>    still show.
> 2. **RESET already re-arms `MBOX0 → 0`.** `neutron_dev_write()` handles the
>    `RESET` command (cancels any in-flight `done_timer`, sets `MBOX0 = 0`). Where
>    §4.1 calls this "a latent bug your dispatch fixes," it is already fixed —
>    keep the behaviour, don't reintroduce the bug.
> 3. **`MBOX1` is already written on completion.** `neutron_done()` writes an
>    honest-fault `error_code` (default `0x95E0`, the `neutron-uncomputed-errcode`
>    property; `0` opts into happy-path). Where §5.3 says "the current stub never
>    writes MBOX1," it now does — your engine replaces that placeholder with the
>    *real* `error_code` (`0` on success).
>
> Net effect: the timing seam, the RESET contract, and the MBOX1 path are done.
> What remains is genuinely just the engine (§4.2, §5) behind a worker thread
> (§6). See [`NEUTRON-WIRING.md`](../NEUTRON-WIRING.md) at the repo root for the
> current lifecycle in one page.

There is a fully-working precedent for *exactly this shape* of work in the
sibling **i.MX93 emulator**, which runs real int8 TFLite networks on an emulated
Arm Ethos-U NPU. §2 distills its architecture because you should mirror it.

---

## 1. The end-to-end goal and the runtime path

What the user does, and where each piece lives:

```
 host (offline, eIQ)                         guest (QEMU i.MX95 + Linux)
 ───────────────────                         ────────────────────────────
 int8 model.tflite                            benchmark_model / label_image / app
        │ neutron-converter (Glow, proprietary)        │  TFLite / LiteRT
        ▼                                               ▼
 converted.tflite  ──────hand to QEMU──────▶  libneutron_delegate.so
   • "NeutronGraph" custom op                   claims the NeutronGraph node
   • embeds 3 blobs as its last 3 inputs:       │
       microcode | weights | kernels            ▼
                                              libNeutronDriver.so  (proprietary .so)
                                                 │  opens /dev/neutron0, ioctls
                                                 ▼
                                              drivers/staging/neutron (GPL kernel)
                                                 • BUFFER_CREATE → one contiguous
                                                   DMA carveout; everything staged
                                                   into it by byte-offset
                                                 • INFERENCE_CREATE → programs base
                                                   regs, sends mailbox RUN
                                                 ▼
                                       ┌─────────────────────────────────────┐
                                       │  hw/misc/imx95_neutron.c (QEMU)      │
                                       │  mailbox responder + ►THE ENGINE◄    │
                                       │  reads program+tensors from DRAM,    │
                                       │  computes, writes outputs, ACK DONE  │
                                       └─────────────────────────────────────┘
```

**The deliverable a customer hands the emulator is one file: a neutron-converted
`.tflite`.** The three compiled blobs (microcode/weights/kernels) live *inside*
it as the last three input tensors of the `NeutronGraph` custom op. To run it:

```sh
benchmark_model --graph=/converted.tflite \
    --external_delegate_path=/usr/lib/libneutron_delegate.so \
    --num_runs=1
```

(or any app that calls `ModifyGraphWithDelegate` with the Neutron delegate). The
delegate only claims nodes whose custom op name is `NeutronGraph` (or
`NeutronOp` for the full-firmware variant); a *stock, unconverted* model claims
zero nodes and silently runs on CPU — that is exactly what our current smoke test
(`tests/neutron/run.sh`, stock MobileNet-v1) does, which is why it offloads 0
nodes. A converted model is what exercises your engine.

---

## 2. The proven reference: how i.MX93 runs a real NN on an emulated NPU

The sibling i.MX 93 emulator (this fork's `hw/npu/ethos_u*.c` on the 93 branch)
executes real Vela-compiled int8 networks on its emulated Arm Ethos-U and asserts **bit-exact**
outputs against goldens. Its architecture is the template. Mirror this shape.

**Three concentric layers, with a clean callback seam between plumbing and math:**

```
hw/npu/ethos_u.c              ── QEMU plumbing (NPU-agnostic)
  • MMIO register file, the CMD "run" doorbell, STATUS/IRQ
  • a DETACHED WORKER THREAD so a long inference never stalls the vCPU
  • an AddressSpace (DEFINE_PROP_LINK "dma") for all guest-memory access
  │     ethos_u_cmdstream_run(s, qbase, qsize)   ← snapshots base pointers, spawns worker
  ▼
hw/npu/ethos_u_cmdstream.c    ── PURE decoder (no guest mem, no QOM → unit-testable)
  • walks the Vela command stream, resolves region+offset → absolute guest addr
  │     handler(ctx, opcode, &op)   ← EthosUOpHandler callback; op has ABSOLUTE addrs
  ▼
hw/npu/ethos_u_compute.c      ── thin DMA marshalling: read IFM/weights/scales, run, write OFM
  ▼
hw/npu/ethos_u_kernels.c      ── PURE int8 NN math (conv/depthwise/pool)
hw/npu/ethos_u_requant.c      ── PURE fixed-point requant (gemmlowp/TFLite-Micro numerics)
hw/npu/ethos_u_weights.c      ── PURE weight de-reorder + mlw decode
```

The seam is one typedef (`include/hw/npu/ethos_u_internal.h`):

```c
typedef void (*EthosUOpHandler)(void *ctx, uint16_t opcode, const EthosUOpDesc *op);
```

The decoder is handed `(stream_ptr, size, base_pointers[8], handler, ctx)` and
calls the handler once per op **with absolute guest addresses already resolved**.
The device passes its DMA-marshalling function as the handler. Result: the
**NN-math files have zero QEMU dependencies** (they take plain C buffers + param
structs) and are compiled standalone into unit tests.

**Key engineering rules proven on the 93 side — carry every one of these over:**

1. **Never compute on the vCPU/BQL thread.** A real inference can take many
   milliseconds; doing it synchronously trips guest watchdogs and freezes the
   UI. The 93 model schedules a bottom-half that snapshots the job, spawns a
   *detached worker thread* (`ethos_u_worker`), and the worker does all guest
   memory access via `dma_memory_read/write`, then schedules a second BH back on
   the main loop to publish status + raise the IRQ. Modelled on `hw/misc/edu.c`.
2. **The worker touches only `dma_*` — never QOM state or `qemu_set_irq`.** All
   register/IRQ mutation happens on the main loop under the BQL.
3. **The compute math is pure and host-testable.** This is what lets you assert
   bit-exact correctness in a qtest without booting Linux.
4. **The authoritative correctness proof is a qtest, not a Linux boot.** The 93
   qtest stages a command stream + weights + input into DRAM, writes the run
   doorbell, polls STATUS, then `memread`s the output and asserts it against a
   precomputed golden. The Linux full-stack demo proves the *stack reaches the
   NPU and returns a correct answer*; the qtest proves the *math is right*.

**Where Neutron will diverge from the Ethos-U template (know this going in):**

- Ethos-U has an *open* compiler (Vela) and an *open* weight codec (`mlw_codec`),
  so the 93 model validates its decoder bit-for-bit against the vendor's own
  Python. Neutron's converter (Glow-based) and microcode ISA are **proprietary**
  — that's precisely the part only you have. So your decoder/engine cannot be
  cross-checked against an open reference; your golden vectors must come from
  *your* converter + a trusted reference run (real silicon, the firmware's own
  simulator, or a Glow reference executor). See §9.
- Ethos-U is driven directly via MMIO base-pointer registers (`QBASE`/`BASEP0..7`
  + a `CMD` doorbell). Neutron is driven via a **mailbox command** carrying
  *offsets into a single DMA carveout* (§3, §5). Same idea (the program and
  tensors live in guest DRAM and the model DMA-reads them), different framing.
- On 93 the M33 runs FreeRTOS Ethos firmware that writes the NPU registers; on
  95 the Neutron firmware (`NeutronFirmware.elf`) runs on the NPU's own Zen-V
  core. In **both** emulators we do **not** execute that firmware to get
  compute — the QEMU engine is a *host-side reimplementation of what the firmware
  computes*. Your engine replaces `NeutronFirmware.elf`'s compute loop (§6).

---

## 3. What already exists on i.MX95 (the bring-up front-half)

File: **`hw/misc/imx95_neutron.c`** (single file, ~460 LOC, type `imx95.neutron`).
All of this is done and tested — treat it as the fixed outer wall of the box.

### MMIO regions (registered in `neutron_realize`)

| Region | Base | Size | Backing |
|---|---|---|---|
| RESETCTRL | `0x4ab00000` | 4 B | MMIO (clock gate) |
| Device / mailbox | `0x4ab00004` | 0x400 | MMIO (register file `regs[]`) |
| DTCM | `0x4ab08000` | 32 KiB | RAM |
| ITCM | `0x4ab10000` | 64 KiB | RAM |

IRQ: **GIC SPI 318**. TCM is RAM-backed so `rproc_elf_load_segments` can
`memcpy_toio` `NeutronFirmware.elf` without faulting (the firmware image lands
there; we just don't execute it).

### Mailbox register map (offsets within the `0x4ab00004` device window)

Authoritative names/offsets are `neutron_device.h` (`drivers/staging/neutron`).
The mailbox is **8 scratch registers** `MBOX0..MBOX7` (0x23c..0x258, contiguous)
plus control/status/base registers around them:

| Name | Offset | Role |
|---|---|---|
| `STATUSERR` | 0x000 | status/error |
| `INTENA` | 0x004 | IRQ enable (bits below) |
| `INTCLR` | 0x008 | IRQ clear (driver writes the INTENA value to clear) |
| `APPCTRL` | 0x1fc | **doorbell = bit 2** (`appctrl |= 0x4`); firmware-started handshake in [31:16] |
| `APPSTATUS` | 0x200 | `INFDONE` bit0, `MBOX` bit4 (driver W1C in the IRQ handler) |
| `BASEDDRL/H` | 0x204 / 0x208 | **carveout physical base, low/high** |
| `TAIL` / `HEAD` | 0x234 / 0x238 | FW log ring producer/consumer |
| `MBOX0` | 0x23c | **TX ACK + response retcode** (driver reads via `read_ret`) |
| `MBOX1` | 0x240 | response arg0 = **`error_code`** (read by `INFERENCE_STATE`) |
| `MBOX2` | 0x244 | response arg1 |
| `MBOX3` | 0x248 | **command word** (written *last*, triggers the doorbell) |
| `MBOX4` | 0x24c | command arg0 |
| `MBOX5` | 0x250 | command arg1 |
| `MBOX6` | 0x254 | command arg2 |
| `MBOX7` | 0x258 | command arg3 (max `argc` = 4) |
| `BASEINOUTL/H` | 0x27c / 0x280 | in/out base (driver writes = carveout base) |
| `BASESPILLL/H` | 0x284 / 0x288 | spill/scratch base (driver writes = carveout base) |

Arg placement is mechanical (`neutron_mailbox.c`):
`SEND_MSG_ARG(n) = MBOX3 + ((n+1)<<2)` → arg0→`MBOX4`, arg1→`MBOX5`,
arg2→`MBOX6`, arg3→`MBOX7`. Receive side:
`RECV_MSG_ARG(n) = MBOX0 + ((n+1)<<2)` → the driver reads `retcode = MBOX0`,
`args[0] = MBOX1`, `args[1] = MBOX2` (`MAX_RECV_MSG_ARGC = 2`).

**Magic values** (`neutron_device.h`):

| Constant | Value | Meaning |
|---|---|---|
| `RESET_VAL` | `0x0` | MBOX0 idle/after-reset; driver treats MBOX0≠RESET_VAL at job start as "stuck" → hw reset + firmw reload |
| `RUN_ACK` | `0xA3` | "firmware received the RUN message" (TX ack) |
| `DONE` | `0xAD0` | inference complete (the retcode the completion path waits for) |
| `DM_ACK` / `DM_DONE` | `0xDA3` / `0xDAD` | the "DM" (data-movement/test) variants |

`INTENA`/`INTCLR` bits: `INFERENCE_DONE_IRQ_ENABLE = BIT(1)`,
`MBOX_IRQ_ENABLE = BIT(2)`, `SHUTDOWN_IRQ_ENABLE = BIT(7)`. The driver's
`mbox_irq_handler` runs `recv` when `INTENA & (MBOX|INFERENCE_DONE)`, then
clears via `INTCLR` and W1C-clears `APPSTATUS`.

`RESETCTRL` clock-gate bits (the separate 4-byte window @0x4ab00000):
`ZENV_CLK_ON = (0x1<<0)`, `COMPUTE_CLK_ON = (0xF<<4)`, `TCM_CLK_ON = (0xF<<8)`.

### Mailbox protocol the model already implements

- **Firmware-started handshake.** When the driver writes `RESETCTRL` bit0
  (`ZENV_CLK_ON`), the model sets `APPCTRL[31:16] = 0xF807`. The driver polls
  this to know the "firmware" is up. ✅
- **Doorbell.** Driver writes args to `MBOX4..7`, the command word to `MBOX3`
  (last), then sets `APPCTRL` bit 2. The model intercepts the `APPCTRL` write and
  calls `neutron_doorbell()`. ✅
- **TX ack timing (important, see §6).** Right after the doorbell the driver's
  `mbox_send_data` polls `MBOX0 != 0` up to ~20×(1–10 µs) and returns `-ETIME`
  if it stays `0`. So **MBOX0 must go non-zero quickly** — the model must ACK
  synchronously, *before* a long compute, then write `DONE` when the compute
  actually finishes.
- **Completion (already two-phase).** `neutron_doorbell()` today posts the
  synchronous ack and defers the completion:

  ```c
  /* neutron_doorbell() — synchronous, on the doorbell write */
  s->pending_cmd = ndev_r(s, N_MBOX3);
  ndev_w(s, N_MBOX0, N_RET_RUN_ACK);           /* 0xA3  ← unblocks the tx poll */
  timer_mod(s->done_timer, now + NEUTRON_RUN_NS);   /* PLACEHOLDER for compute */

  /* neutron_done() — deferred, the timer callback (THE STUB: no compute) */
  ndev_w(s, N_MBOX0, N_RET_DONE);              /* 0xAD0 ← the retcode driver waits on */
  ndev_w(s, N_APPSTATUS, ... | INFDONE | MBOX);
  ndev_w(s, N_MBOX1, s->uncomputed_errcode);   /* honest-fault error_code (0x95E0) */
  s->irq_pending = true;
  neutron_update_irq(s);                        /* raises SPI 318 */
  ```

  i.e. it reports `DONE` (0xAD0) and raises the IRQ **without reading the program,
  the tensors, or computing anything** — the output buffer at `BASEINOUT` is left
  untouched. The ACK-now / DONE-later split §6 describes **already exists**; the
  `done_timer` is a placeholder standing in for a real (slow) compute. **This is
  the exact spot you hook:** replace the timer deferral with a worker thread that
  calls your engine and posts `DONE` when it actually finishes (§6).

### Command values (from the driver, `neutron_device.h` + `neutron_inference.c`)

| Command | Value | Mailbox payload | Driver `cmd_type` |
|---|---|---|---|
| `RUN` (run inference) | `0x269` | arg0=`tensor_offset`, arg1=`microcode_offset`, arg2=`tensor_count` (`argc=3`) | `NEUTRON_CMD_RUN_INFERENCE` |
| `KERNELS` (cache kernel blob) | `0x272` | arg0=`kernel_offset` (`argc=1`) | `NEUTRON_CMD_LOAD_KERNEL` |
| `CLEAR_FW_LOG` | `0x270` | — (`argc=0`) | `NEUTRON_CMD_CLEAR_LOG` |
| `GET_FW_LOGLEVEL` | `0x271` | — | — |
| `RESET` | `0x23637` | (sent as `mbox_send_reset`: MBOX4=MBOX5=`RESET_VAL`, then MBOX3=`RESET`) — driver then **polls MBOX0 until it reads `RESET_VAL` (0)** | — |
| `DM_TEST` | `0xD37E57` | data-movement self-test (not on the inference path) | — |

---

## 4. The integration seam — the one hook point and its contract

Mirror the 93 split: keep `imx95_neutron.c` as the plumbing, add a new file
**`hw/misc/imx95_neutron_compute.c`** (+ an internal header) that holds your
engine. The plumbing calls the engine through **one function**.

### 4.1 Replace the stub dispatch in `neutron_doorbell()`

```c
/* Register/value names mirror neutron_device.h. */
#define NEUTRON_CMD_RUN      0x269
#define NEUTRON_CMD_KERNELS  0x272
#define NEUTRON_CMD_CLRLOG   0x270
#define NEUTRON_CMD_RESET    0x23637
#define NEUTRON_RUN_ACK      0xA3      /* MBOX0 "received" ack            */
#define NEUTRON_RET_DONE     0xAD0     /* MBOX0 "inference complete"      */
#define NEUTRON_RESET_VAL    0x0       /* MBOX0 idle / post-reset         */

static void neutron_doorbell(IMX95NeutronState *s)
{
    uint32_t cmd = ndev_r(s, N_MBOX3);

    switch (cmd) {
    case NEUTRON_CMD_RUN:               /* 0x269 — run an inference */
        /*
         * ACK synchronously so the driver's mbox_send_data (which polls
         * MBOX0 != 0 for ~20 µs) returns success, THEN run the job off the
         * vCPU thread. The worker posts MBOX0=DONE + the IRQ when it finishes.
         */
        ndev_w(s, N_MBOX0, NEUTRON_RUN_ACK);
        neutron_submit_job(s, &(NeutronJob){
            .base_ddr        = ((uint64_t)ndev_r(s, N_BASEDDRH) << 32) |
                                ndev_r(s, N_BASEDDRL),
            .tensor_offset   = ndev_r(s, N_MBOX4),
            .microcode_offset= ndev_r(s, N_MBOX5),
            .tensor_count    = ndev_r(s, N_MBOX6),
        });
        return;                          /* completion is posted by the worker */

    case NEUTRON_CMD_KERNELS:            /* 0x272 — cache the kernel blob */
        /* arg0 = kernel_offset; stash {base_ddr + kernel_offset} for the next
         * RUN. The real firmware caches kernels across inferences. */
        neutron_cache_kernels(s, ndev_r(s, N_MBOX4));
        ndev_w(s, N_MBOX0, NEUTRON_RET_DONE);
        break;

    case NEUTRON_CMD_RESET:             /* 0x23637 — driver expects MBOX0→0 */
        ndev_w(s, N_MBOX0, NEUTRON_RESET_VAL);
        break;

    default:                            /* CLEAR_FW_LOG, GET_FW_LOGLEVEL, … */
        ndev_w(s, N_MBOX0, NEUTRON_RET_DONE);
        break;
    }
    /* non-RUN commands: raise APPSTATUS(INFDONE|MBOX) + IRQ here as today */
}
```

Note the driver's stuck-detection: at the start of every job it reads `MBOX0`
and, if it is **not** `RESET_VAL` (0), it concludes the NPU is wedged and does a
hardware reset + firmware reload before proceeding. So the `RESET` command
*must* drive `MBOX0` back to `0`. **This is already implemented** — `RESET` is
not doorbelled, so `neutron_dev_write()` handles it (cancels any in-flight
`done_timer`, then `MBOX0 = RESET_VAL`); keep that behaviour, don't reintroduce
the bug an earlier draft of this doc warned about. The normal cycle is:
`MBOX0: 0 (idle) → 0xA3 (RUN ack) → 0xAD0 (DONE) → [driver sends RESET] → 0`.

### 4.2 The engine entry point (what you implement in the new file)

```c
/* hw/misc/imx95_neutron_compute.h  — the seam */

typedef struct NeutronComputeReq {
    AddressSpace *as;           /* DMA address space (guest system memory)     */
    hwaddr   base_ddr;          /* physical base of the carveout in guest DRAM */
                                /* (= BASEDDR = BASEINOUT = BASESPILL)         */
    uint32_t microcode_offset;  /* program:    base_ddr + microcode_offset     */
    uint32_t tensor_offset;     /* descr table: base_ddr + tensor_offset       */
    uint32_t tensor_count;      /* number of tensor descriptors                */
    uint32_t kernel_offset;     /* kernel blob: base_ddr + kernel_offset       */
                                /* (latched by the last KERNELS command; 0 if  */
                                /*  none)                                      */
} NeutronComputeReq;

typedef struct NeutronComputeRes {
    uint32_t retcode;           /* NEUTRON_RET_DONE (0xAD0) or a non-DONE error */
    uint32_t error_code;        /* surfaced in MBOX1 (INFERENCE_STATE reads it) */
} NeutronComputeRes;

/*
 * Run one inference. ALL guest memory is reached through req->as:
 *   - the program  at  req->base_ddr + req->microcode_offset
 *   - the tensor descriptor table at  req->base_ddr + req->tensor_offset
 *     (req->tensor_count entries; the per-entry layout is the proprietary part)
 *   - every input/weight/scratch tensor the descriptors point at
 * Compute, then DMA-write each output tensor back to its descriptor's location
 * in the carveout. Fill *res and return.
 *
 * MUST be callable from a worker thread: use ONLY dma_memory_read/write through
 * req->as; do NOT touch QOM/registers/IRQ here (the caller publishes results on
 * the main loop). See §6.
 */
void neutron_compute_run(const NeutronComputeReq *req, NeutronComputeRes *res);
```

That signature is the entire contract. Everything to the left of it (gathering
the args, threading, posting `DONE`/`error_code`, the IRQ) is provided.
Everything to the right (parsing the proprietary microcode + tensor descriptors,
the MAC math, writing outputs) is yours. All offsets are carveout-relative and
resolve to a guest physical address as `base_ddr + offset`; never hold a host
pointer into guest memory — always `dma_memory_read/write(req->as, addr, …)`.

---

## 5. The execution-engine contract — inputs, memory model, outputs

This is the precise description of *what the box receives and must produce*. The
**format** of the microcode and the tensor descriptors is proprietary (you have
it); this section pins down *where they are*, *how they got there*, and *what
must be true when you return* — all grounded in the actual driver + delegate
source.

### 5.1 How the carveout is built (userspace → kernel)

Everything the NPU touches lives in **one physically-contiguous DMA buffer** —
not dma-buf fds, not scattered physical addresses. The path:

1. **`NEUTRON_IOCTL_BUFFER_CREATE`** (`struct neutron_uapi_buffer_create
   {__u32 size; __u64 addr;}`). The kernel calls `dma_alloc_attrs(...,
   DMA_ATTR_FORCE_CONTIGUOUS)` and returns the buffer's **DMA address** in
   `addr` plus an anon-inode **fd** (the `buf_id`). The buffer is `mmap`-able, so
   userspace stages bytes straight into it.
2. The **delegate gives the TFLite tensors a `customAllocation` pointing inside
   this buffer** (`neutron_delegate.cc`, `NeutronModelType_CONVERTOR` path) — so
   the model's input/output tensors, plus the converter's microcode/weights/
   kernels blobs, all physically reside in the carveout. `libNeutronDriver.so`
   therefore expresses every address as `ptr - carveout_base` = an **offset**.
3. **`NEUTRON_IOCTL_INFERENCE_CREATE`** submits a `struct
   neutron_uapi_inference_args` (verbatim from `uapi/neutron.h`):

   ```c
   struct neutron_uapi_inference_args {
       union { __u32 args0; __u32 tensor_offset; __u32 kernel_offset; };
       union { __u32 args1; __u32 microcode_offset; };
       union { __u32 args2; __u32 tensor_count; };
       union { __u32 args3; };
       __u32 base_ddr_l;     /* carveout DMA base, low 32  */
       __u32 base_ddr_h;     /* carveout DMA base, high 32 */
       __u32 firmw_id;       /* which firmware variant this model needs */
       __u32 buf_id;         /* the BUFFER_CREATE fd = the carveout      */
       __u32 input_offset;   /* where inputs are  (driver cache-sync only) */
       __u32 input_size;
       __u32 output_offset;  /* where outputs go  (driver cache-sync only) */
       __u32 output_size;
       __u32 reserve[5];
   };
   ```

4. The driver (`neutron_inference.c::neutron_inference_run`) then, for a RUN:
   - `DMA_TO_DEVICE` cache-syncs `[base_ddr+input_offset, +input_size)`;
   - writes the carveout base to **all three** base-register pairs:
     ```c
     writel(base_ddr_l, reg + BASEDDRL);  writel(base_ddr_h, reg + BASEDDRH);
     writel(base_ddr_l, reg + BASEINOUTL); writel(base_ddr_h, reg + BASEINOUTH);
     writel(base_ddr_l, reg + BASESPILLL); writel(base_ddr_h, reg + BASESPILLH);
     ```
   - builds the mailbox message `{command=RUN(0x269), args=[tensor_offset,
     microcode_offset, tensor_count], argc=3}` and rings the doorbell.

So by the time your engine runs, `BASEDDR`/`BASEINOUT`/`BASESPILL` all hold the
carveout base and `MBOX4/5/6` hold `tensor_offset`/`microcode_offset`/
`tensor_count`. (`input_offset`/`output_offset` are **not** in the mailbox — the
firmware finds I/O through the tensor descriptor table, §5.2; the driver uses
them only for cache sync.)

### 5.2 The carveout layout your engine reads

```
 guest DRAM
 ┌──────────────────────── carveout (base = req->base_ddr) ───────────────────────────┐
 │  [ microcode @ +microcode_offset ]  compiled Neutron program (PROPRIETARY ISA)      │
 │  [ tensor descriptor table @ +tensor_offset ]  req->tensor_count entries            │
 │        (PROPRIETARY per-entry layout; classifies & locates every tensor:            │
 │         kind ∈ {input, output, weight, scratch}, its offset in the carveout,        │
 │         shape/rank, dtype, and quant params — zero-point + scale/shift)             │
 │  [ weights @ +kernel/weight offsets ]   reordered/packed (converter output)         │
 │  [ kernels @ +kernel_offset ]   (latched by the prior KERNELS command)              │
 │  [ input tensor(s) ]    staged via customAllocation before RUN                      │
 │  [ output tensor(s) ]   ◄── YOU WRITE HERE; driver cache-syncs + returns them       │
 │  [ scratch / spill ]    intermediate activations (BASESPILL region)                 │
 └────────────────────────────────────────────────────────────────────────────────────┘
```

Resolution rule for every reference: `guest_addr = req->base_ddr + offset`,
accessed only via `dma_memory_read/write(req->as, guest_addr, …)`. Concretely
your engine must:

1. **Read the program** at `base_ddr + microcode_offset` and decode it (the
   proprietary microcode/command-stream ISA — your Glow/firmware knowledge).
2. **Walk the tensor descriptor table** at `base_ddr + tensor_offset`
   (`tensor_count` entries). From each descriptor learn the tensor's kind,
   carveout offset, shape, dtype, and int8/int16 quant params. This is where you
   find the input tensor(s), the weight blob, the scratch region, and — crucially
   — the destination offset(s) for the output tensor(s).
3. **Execute** the program's ops over the input + weight tensors (host-side
   reimplementation of what the firmware's compute loop does), using scratch for
   intermediates.
4. **DMA-write each output tensor** back to its descriptor's carveout offset, in
   the exact dtype/quantization the model expects.

> **The two things only you can supply** (everything else in this doc is fixed
> and public): (a) the **byte layout of one tensor descriptor** at
> `base_ddr + tensor_offset`, and (b) the **microcode ISA** at
> `base_ddr + microcode_offset`. The kernel + delegate treat both as opaque
> blobs (`NeutronModelConfig.microcode/weights/kernels` are passed to
> `libNeutronDriver.so` purely by reference; nothing in the open stack parses
> them). Reconstruct these from the Neutron converter (Glow backend) / firmware,
> and the engine *is* a host-side reimplementation of the firmware's
> interpret-and-compute loop — exactly the role `hw/npu/ethos_u_compute.c` plays
> for Vela's command stream on the 93 side.

### 5.3 What must be true when `neutron_compute_run` returns

1. Every **output tensor**'s bytes are DMA-written into the carveout at the
   offset its descriptor specifies, in the model's output dtype/quant. The driver
   then `DMA_FROM_DEVICE`-syncs `[base_ddr+output_offset, +output_size)` and the
   delegate copies the result up to the TFLite output tensor (`memcpy` for
   shared tensors; otherwise the customAllocation already aliases it).
2. `res->retcode = NEUTRON_RET_DONE` (`0xAD0`) on success — the completion path
   keys on `MBOX0 == DONE`. On failure use any non-`DONE`, non-`RESET_VAL` value
   (so the driver's poll/IRQ wakes) and set `res->error_code`.
3. `res->error_code` is surfaced in **`MBOX1`**, which the driver returns to
   userspace via `NEUTRON_IOCTL_INFERENCE_STATE`
   (`struct neutron_uapi_result_status {status; error_code;}`, read from
   `RECV_MSG_ARG(0)=MBOX1`). `0` on success. **The plumbing already writes
   `MBOX1`** — today with an honest-fault placeholder (`uncomputed_errcode`,
   default `0x95E0`, since nothing computed); your engine replaces it with the
   *real* `error_code` (`0` on a successful compute). See §0's current-state note.

### 5.4 Completion protocol the plumbing already honors (don't re-implement)

- After the worker finishes, the main-loop BH writes `MBOX0 = res->retcode`,
  `MBOX1 = res->error_code`, sets `APPSTATUS` `INFDONE|MBOX`, and raises SPI 318.
- The driver, on `DONE`, issues a `RESET` (`0x23637`); the dispatch (§4.1) drives
  `MBOX0 → RESET_VAL (0)` so the next job's stuck-check passes.
- The `firmw_id` field lets the driver reload a *different* firmware per model
  (`neutron_firmw_reload` → rproc stop / reload ELF / start). For the common
  `NeutronGraph` path the firmware is constant; you can ignore `firmw_id` unless
  you model the `NeutronOp`/full-firmware variant (§7).

---

## 6. Threading & correctness rules (copy from 93)

A converted MobileNet-class model is millions of MACs — do **not** run it inline
in `neutron_doorbell()` (that executes on the vCPU thread under the BQL and will
stall the guest). Use the 93 pattern verbatim:

```
neutron_doorbell()  [vCPU/BQL]  MBOX0=RUN_ACK(0xA3)  ← unblocks driver's tx-done poll IMMEDIATELY
                                snapshot regs → NeutronJob(heap) → schedule kick BH
   │
neutron_kick_bh()   [main loop] spawn DETACHED worker thread(job)
   │
neutron_worker(job) [worker]    neutron_compute_run(&job->req, &job->res);   // dma_* ONLY, slow
   │                            schedule done BH
   │
neutron_done_bh(job)[main loop] MBOX0=res.retcode(DONE); MBOX1=res.error_code;
                                APPSTATUS|=INFDONE|MBOX; qemu_set_irq(SPI318); free(job)
```

The **two-phase MBOX0** is the crux of overlapping a slow compute with the
driver's tight ack poll: set `RUN_ACK` synchronously in the doorbell (so
`mbox_send_data`'s `MBOX0 != 0` check passes within its ~20 µs window), then let
the worker take as long as it needs and write `DONE` from the done-BH. The
driver's result wait is patient — poll-mode loops on `read_ret==DONE` via a
100 µs hrtimer, and IRQ-mode only acts on `retcode==DONE` — so a multi-ms
inference is fine as long as the early ACK landed.

Rules (each one is load-bearing — they're why the 93 engine is stable):

- The worker uses **only** `dma_memory_read/write` through the AddressSpace. No
  register reads, no `qemu_irq`, no QOM pointer chasing.
- Snapshot every needed register value into the `NeutronJob` *before* spawning
  the worker (the guest may scribble registers while it runs).
- Add a DMA `AddressSpace` to the device. The simplest correct choice is system
  memory; for parity with 93, expose it as `DEFINE_PROP_LINK("dma", ...,
  TYPE_MEMORY_REGION)` defaulting to `get_system_memory()` so the board can
  retarget it. (The Neutron node has `iommus = <&smmu 0xd>`; QEMU models no
  SMMU, so the test dtb already strips that phandle and DMA is identity — see
  §7. If an SMMU model lands later, the engine still just uses its AddressSpace.)
- A first bring-up cut *may* run synchronously for tiny models, but ship the
  worker pattern — it's a direct copy and avoids watchdog surprises.

> **Alternative you should consciously reject:** actually executing
> `NeutronFirmware.elf` on an emulated Zen-V (RISC-V) core. That buys nothing
> unless you also model the Neutron MAC-array registers the firmware pokes —
> i.e. far more work for the same result. The host-side engine (what 93 does) is
> the tractable, proven path. Keep the firmware ELF loaded into TCM for fidelity
> of the bring-up handshake; just don't rely on running it for compute.

---

## 7. The artifact & on-device runtime (already wired — context for your tests)

This whole section is *upstream* of your engine — stock NXP userspace you don't
modify — but you need its exact behaviour to build a converted-model test (§9)
and to know what reaches the mailbox.

### 7.1 The artifact a customer hands the emulator

A single **neutron-converted `.tflite`** flatbuffer. The host-side eIQ **Neutron
Converter** (a Glow-based ML compiler; it ingests a quantized int8 LiteRT/TFLite
model) fuses the NPU-mappable subgraph into one **TFLite Custom operator** and
embeds three compiled blobs as that node's **last three input tensors**:

```
 converted.tflite ── custom op  custom_name = "NeutronGraph"
   node->inputs = [ real_input_0, …, real_input_{k}, microcode, weights, kernels ]
                                                       └ [n-3]   [n-2]    [n-1] ┘
   each blob 16-byte aligned; read straight from tensor->data.raw
```

Confirmed in `neutron_delegate.cc::InitOfflineCompiledModel`: it sets
`mcfg.microcode = tensors[inputs[n-3]].data.raw`, `mcfg.weights = …[n-2]`,
`mcfg.kernels = …[n-1]`, where `mcfg` is the `NeutronModelConfig` from
`NeutronDriver.h` (the three blob pointers + a `timeoutSeconds`, default 60).
The blob *contents* — the microcode ISA and the weight/kernel packing — are the
proprietary part you decode in §5.2.

Two delegate model types matter (`neutron_delegate.h`):
- **`NeutronModelType_CONVERTOR`** — `custom_name == "NeutronGraph"`. The common
  path; the engine you build serves it.
- **`NeutronModelType_FFIRMWARE`** — `custom_name == "NeutronOp"`, with a
  whole firmware ELF in a tensor literally named `"NeutronFirmware"` (the
  delegate writes it to `/lib/firmware/NeutronFirmware.elf` and the driver
  `firmw_id`-reloads it per model). This is the "Neutron-S full custom firmware"
  variant; treat as a later extension.
- `NeutronModelType_NORMAL` — no Neutron custom op → the delegate claims nothing.

### 7.2 The on-device runtime sequence (one converted model)

```
benchmark_model --graph=converted.tflite --external_delegate_path=libneutron_delegate.so
  │
  │  TFLite/LiteRT loads the flatbuffer, builds the graph
  ▼
libneutron_delegate.so
  Initialize(): neutronInit()  → opens /dev/neutron0
                scan ops → sees "NeutronGraph" → model_type = CONVERTOR
  IsNodeSupportedByDelegate(): claims every kTfLiteBuiltinCustom / "NeutronGraph" node
  InitOfflineCompiledModel(): per node, mcfg.{microcode,weights,kernels} = last-3 inputs
  Prepare():  neutronModelPrepare(&mcfg, &nmh)        ┐ libNeutronDriver.so:
  Eval():     dcfg.inputs[i]=tensor.data.raw          │  BUFFER_CREATE (carveout),
              dcfg.outputs[i]=tensor.data.raw         │  stage microcode/weights/kernels
              neutronRunBlocking(nmh, &dcfg)          │  + inputs into it by offset,
  ▼                                                   ┘  KERNEL_LOAD then INFERENCE_CREATE
/dev/neutron0  (drivers/staging/neutron)
  BUFFER_CREATE → dma_alloc_attrs(FORCE_CONTIGUOUS), returns dma_addr + buf fd
  INFERENCE_CREATE(neutron_uapi_inference_args) → neutron_inference_run():
     writel base_ddr → BASEDDR/BASEINOUT/BASESPILL ;  mbox {RUN, tensor_off, mc_off, count}
  ▼
  ►► hw/misc/imx95_neutron.c → neutron_compute_run()  ◄◄  (YOUR ENGINE)
  ▼
  DONE → driver DMA_FROM_DEVICE syncs outputs → delegate returns NeutronGraph outputs to TFLite
```

`neutronModelPrepare` "prepares/caches" a model so the same model can be re-run
with new input data in the same buffers — i.e. KERNELS/microcode are staged once,
then each inference is a fresh RUN. The userspace `.so`s
(`libneutron_delegate.so`, `libNeutronDriver.so`) are stock in the
`imx-image-full` rootfs; you never touch them.

### 7.3 Bring-up plumbing (already done; here for your harness)

- **The firmware ELF** (`NeutronFirmware.elf`) is loaded via remoteproc into TCM.
  Its `imx_neutron_rproc.c` att table maps device→system: DTCM
  `0x00040000→0x4AB08000` (32K), ITCM `0x00000000→0x4AB10000` (64K). One special
  segment, `da == 0x50000`, is the **DDR-data** segment — copied into the
  *carveout* (`buf->cpu_addr + data_offset`), not TCM. The model RAM-backs the
  TCM so this `memcpy_toio` doesn't fault. We do **not** execute the firmware —
  your engine replaces its compute loop (§6).
- **The two dtb tweaks our harness applies** (not your concern, but needed to
  reach your engine): a 4 GiB Neutron carveout overlay
  (`imx95-19x19-evk-neutron.dtso`, not in the base dtb), and stripping the
  `iommus = <&smmu 0xd>` phandle on the compute node `imx95-neutron@4ab00004`
  (no SMMU model → otherwise the driver never binds). Both are in
  `tests/neutron/run.sh`.
- **Why today's test offloads 0 nodes:** its model is stock
  `mobilenet_v1_1.0_224_quant.tflite` — *not* neutron-converted, so it has no
  `NeutronGraph` op, the delegate sets `model_type = NORMAL` and claims nothing
  (CPU fallback, correct results). This validates the entire bring-up datapath
  but never calls your engine. **Note the asymmetry:** a *converted* model with
  no engine produces *wrong* output (the delegate hands the node to the NPU; there
  is no CPU fallback for a claimed `NeutronGraph` node), whereas the *unconverted*
  model is correct on CPU. Your validation therefore needs a converted fixture
  (§9).

---

## 8. Files, build wiring, and the device-side glue you add

```
hw/misc/imx95_neutron.c            (exists) — add to IMX95NeutronState: an
                                    AddressSpace `dma_as` + a `kernel_offset`
                                    cache field. Add: the §4.1 command dispatch in
                                    neutron_doorbell() (ACK-now); neutron_submit_job
                                    + the kick/worker/done BHs (§6); on KERNELS,
                                    stash MBOX4 into kernel_offset.
hw/misc/imx95_neutron_compute.c    (NEW)    — neutron_compute_run() + the microcode
                                    interpreter + tensor-descriptor parser + MAC
                                    kernels + requant. THE PROPRIETARY ENGINE.
hw/misc/imx95_neutron_compute.h    (NEW)    — NeutronComputeReq/Res + the seam decl.
```

- **AddressSpace.** In `neutron_realize`, `address_space_init(&s->dma_as,
  get_system_memory(), "neutron-dma")` (or a `DEFINE_PROP_LINK("dma", …,
  TYPE_MEMORY_REGION)` defaulting to system memory, mirroring 93's
  `ethos_u.c`). Pass `&s->dma_as` into each `NeutronComputeReq`.
- **Keep the engine math pure** (plain C buffers in/out, no QOM) so it compiles
  into a standalone qtest/unit test, exactly like 93's `kernels.c`/`requant.c`.
  Only `neutron_compute_run`'s top layer touches `dma_memory_*`.
- `hw/misc/meson.build`: add `imx95_neutron_compute.c` next to `imx95_neutron.c`
  in the same `system_ss.add(when: 'CONFIG_…')` stanza. No new Kconfig symbol
  (fold into the existing Neutron gate).
- No machine-wiring change: `fsl-imx95.c` already instantiates the device, maps
  the 4 windows, and wires SPI 318.

---

## 9. Test & validation plan

Two tiers, mirroring 93. The qtest is the correctness oracle; the Linux boot is
the integration oracle.

**Tier 1 — qtest (authoritative, no Linux):** extend
`tests/qtest/imx95-neutron-test.c`. Stage a known converted program + weights +
a fixed input into guest DRAM, program `BASEDDR*`, write `tensor_offset` /
`microcode_offset` / `tensor_count` to `MBOX4/5/6`, write `RUN` (`0x269`) to
`MBOX3`, ring the doorbell (`APPCTRL` bit2), poll `MBOX0` for `0xAD0`, then
`qtest_memread` the output offset and assert **bit-exact** against a golden.
Generate the golden from *your* trusted reference (real silicon, the firmware
simulator, or a Glow reference run) — there is no open Neutron codec to check
against (unlike Ethos-U/Vela), so this golden is the crux of your correctness
claim. Escalate cases: single conv → depthwise → a small fused subgraph → a real
converted MobileNet tile.

**Tier 2 — full stack (integration):** extend `tests/neutron/run.sh` to also run
a **neutron-converted** model under `benchmark_model
--external_delegate_path=/usr/lib/libneutron_delegate.so`, and assert (a) the
delegate reports it **claimed the NeutronGraph node** (non-zero offload, in the
benchmark log) and (b) the output classification/logits match the expected
result for a known input. This proves eIQ-artifact → delegate → driver → mailbox
→ **your engine** → correct output, the user's exact end goal. Keep the existing
stock-MobileNet run as the "datapath still binds" smoke test.

Add a converted fixture model to the harness (a tiny converted classifier is
ideal so the golden is cheap). Gate the test to SKIP cleanly if the converted
fixture or the proprietary `.so`s are absent, matching our other tests.

---

## 10. Fidelity ceiling & definition of done

- **Done =** a user boots QEMU i.MX95, hands a neutron-converted `.tflite`, runs
  it via the Neutron delegate, the delegate offloads the `NeutronGraph`
  node(s) to `/dev/neutron0`, your engine computes, and the returned tensors are
  numerically correct (Tier-2 passes); plus the Tier-1 qtest asserts bit-exact
  op output.
- **Until then,** a converted model on qemu95 exercises the *entire submission
  datapath* (ioctls, carveout DMA, mailbox `RUN`→`DONE`) but returns **garbage
  output** for the offloaded node — note that there is *no* CPU fallback for a
  `NeutronGraph` node (the delegate already handed it to the NPU), so a converted
  model without your engine produces wrong answers, whereas an *unconverted*
  model is correct via CPU. This is the proprietary-firmware ceiling; your engine
  is exactly what closes it.
- Scope realism: match coverage to the operator set your converter actually
  emits. Ethos-U's 93 engine shipped conv/depthwise/pool first and stubbed
  elementwise; do the same — implement the ops a converted MobileNet needs, then
  widen. Partial op coverage is fine as long as unsupported ops return a clean
  error retcode (so the delegate/driver surface a failure rather than silently
  corrupting).

---

## 11. What you (internal NXP Claude) must supply, and authoritative sources

**You provide (the inside of the box):**
1. The **tensor-descriptor struct** layout at `base_ddr + tensor_offset` (kind,
   offset, shape, dtype, quant params). *Not on the public side.*
2. The **microcode/command-stream ISA** at `base_ddr + microcode_offset` and how
   to interpret it (the firmware's compute loop, or the Glow backend's emission).
3. The **weight/kernel blob** encoding (any reorder/packing to invert), analogous
   to Ethos-U's mlw + Vela reorder.
4. The **reference goldens** for the qtest (from silicon / firmware-sim / Glow
   reference) — the one thing with no open cross-check.

**Already pinned down for you (the outside of the box):** the mailbox protocol,
register map, command values, completion/IRQ contract, carveout memory model,
the seam function signature, the threading rules, and the test harness shape —
all in §3–§9 above.

**Authoritative public sources to cross-reference** (NXP GitHub —
`https://github.com/nxp-imx`, org index `https://github.com/NXP`). Canonical
paths confirmed:

- **Kernel driver + uapi** — `nxp-imx/linux-imx`, branch `lf-6.12.y`,
  `drivers/staging/neutron/`:
  <https://github.com/nxp-imx/linux-imx/tree/lf-6.12.y/drivers/staging/neutron>
  — the ioctl ABI (`uapi/neutron.h`), `neutron_uapi_inference_args`, the `RUN`
  mailbox build + `BASEDDR*` writes (`neutron_inference.c`), the register/command
  constants (`neutron_device.h`), the `SEND_MSG_ARG`/doorbell/`mbox_send_reset`
  (`neutron_mailbox.c`). (Clone this branch locally to read alongside the model.)
- **Remoteproc att table** (TCM device→system map; SRC reset
  `0x1F11`/`0x1F00`): `drivers/remoteproc/imx_neutron_rproc.c` in the same repo.
- **TFLite delegate** — `nxp-imx/tflite-neutron-delegate`:
  <https://github.com/nxp-imx/tflite-neutron-delegate> — `neutron_delegate.{cc,h}`
  (`NEUTRON_CUSTOM_NAME "NeutronGraph"`, the model-type detection, the
  last-3-inputs blob extraction in `InitOfflineCompiledModel`,
  `neutronModelPrepare`/`neutronRunBlocking`). The newer LiteRT variant is the
  `litert-neutron-delegate` package (same delegate contract).
- **Driver API header** `NeutronDriver.h` (the `NeutronModelConfig`
  microcode/weights/kernels contract + `NeutronDataConfig` inputs/outputs/scratch
  + `neutronInit`/`neutronModelPrepare`/`neutronRunBlocking`): in the NXP neutron
  software package (`imx95/include/NeutronDriver.h`).
- **Open Neutron backend (very useful to you)** — `pytorch/executorch`,
  `backends/nxp`: <https://github.com/pytorch/executorch/tree/main/backends/nxp>
  — an open-source eIQ Neutron backend / converter front end; the closest public
  window onto how a graph is lowered toward Neutron microcode.
- **Machine Learning User's Guide** (the conversion + delegate workflow): NXP
  UG10166 (i.MX Machine Learning User's Guide).
- **The proven sibling implementation to mirror:** the i.MX93 emulator
  `hw/npu/ethos_u*.c` (this fork's sibling tree) — the exact three-layer
  plumbing/decoder/kernels split + worker-thread pattern to copy.

If you want the exact tensor-descriptor bytes or a converted fixture wired into
`tests/neutron/`, those are the two things to bring from inside NXP; everything
else in this document is buildable from the public + local sources cited.

---

### Appendix A — quick reference

```
Device:   imx95.neutron  (hw/misc/imx95_neutron.c)
Windows:  RESETCTRL 0x4ab00000(4)  MAILBOX 0x4ab00004(0x400)
          DTCM 0x4ab08000(32K)     ITCM 0x4ab10000(64K)
IRQ:      GIC SPI 318      INTENA bits: INFDONE=BIT1 MBOX=BIT2 SHUTDOWN=BIT7
Doorbell: APPCTRL(0x1fc) |= 0x4 (bit2)   FW-up: APPCTRL[31:16]=0xF807
          RESETCTRL bit0 ZENV_CLK_ON triggers the FW-up handshake
Mailbox:  MBOX0..7 @ 0x23c..0x258 (contiguous, ×4)
          SEND_MSG_ARG(n)=MBOX3+((n+1)<<2)  RECV_MSG_ARG(n)=MBOX0+((n+1)<<2)
Cmd word: MBOX3(0x248)  args: MBOX4/5/6/7 (0x24c/0x250/0x254/0x258)
Resp:     MBOX0(0x23c)=ack/retcode   MBOX1(0x240)=error_code   MBOX2(0x244)=arg1
          values: RESET_VAL=0x0  RUN_ACK=0xA3  DONE=0xAD0  (DM_ACK=0xDA3 DM_DONE=0xDAD)
Base:     BASEDDR L/H 0x204/0x208   BASEINOUT L/H 0x27c/0x280   BASESPILL L/H 0x284/0x288
          (driver writes the carveout base to all three)
Commands: RUN 0x269 {tensor_off→MBOX4, microcode_off→MBOX5, tensor_count→MBOX6}
          KERNELS 0x272 {kernel_off→MBOX4}   CLEAR_FW_LOG 0x270
          GET_FW_LOGLEVEL 0x271   RESET 0x23637 (→ MBOX0 must read back 0)
MBOX0 life: 0(idle) → 0xA3(RUN ack, set in doorbell) → 0xAD0(DONE, set by worker)
            → [driver sends RESET] → 0
Hook:     neutron_doorbell() posts RUN_ACK + arms done_timer; neutron_done()
          (timer cb) posts DONE — replace the done_timer deferral with a worker
          thread that calls neutron_compute_run() and posts DONE when it finishes
Seam:     void neutron_compute_run(const NeutronComputeReq*, NeutronComputeRes*) // §4.2
SRC reset (remoteproc): IMX95_SRC_SCR 0x00, START 0x1F11, STOP 0x1F00, mask 0xFFFF
```

### Appendix B — the run command, end to end (one inference)

```
delegate stages microcode/weights/input into the carveout (by offset)
  → ioctl INFERENCE_CREATE(args: tensor_offset, microcode_offset, tensor_count,
                           base_ddr_l/h, input/output offsets, buf_id, firmw_id)
  → driver: writel base_ddr → BASEDDR/BASEINOUT/BASESPILL
  → driver: writel args → MBOX4/5/6 ; writel RUN(0x269) → MBOX3 ; APPCTRL bit2
  → QEMU neutron_doorbell(): MBOX0=RUN_ACK(0xA3) [unblocks driver tx-poll]
                             → snapshot regs → worker
  → driver: mbox_send_data sees MBOX0≠0 → returns OK; then waits for DONE
  → worker: neutron_compute_run(): DMA-read program@+microcode_offset and the
            tensor table@+tensor_offset; interpret; DMA-write outputs
  → done BH: MBOX0=DONE(0xAD0), MBOX1=error_code, APPSTATUS|=INFDONE|MBOX, IRQ SPI318
  → driver: sees DONE (poll read_ret, or MBOX irq) → DMA_FROM_DEVICE sync
            → sends RESET(0x23637) → QEMU sets MBOX0→RESET_VAL(0)
  → delegate: returns the NeutronGraph node's output tensors to TFLite
```
