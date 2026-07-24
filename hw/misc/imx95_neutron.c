/*
 * NXP i.MX 95 Neutron NPU - bring-up model (mailbox responder + remoteproc)
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See docs/neutron-state-machine-spec.md for the normative FSM this file
 * implements: phase enum (IDLE/RUNNING/COMPLETE), monotonic job_seq /
 * active_seq counters, heap NeutronJob per accepted RUN, and a done-BH with
 * a stale-completion guard (job.seq == active_seq AND phase == RUNNING).
 * The guard is the whole reason for the seq bookkeeping: RESET during
 * RUNNING disowns the worker via active_seq=0, and the worker's later
 * done-BH observes the mismatch and drops silently.
 *
 * The mailbox front end is invariant across backends (see
 * NeutronBackendOps in imx95_neutron_compute.h). Today the backend's
 * run() is synchronous under BQL, so the done-BH runs on the next main
 * loop iteration after the doorbell; a future worker-thread backend
 * schedules the same BH from its tail and the stale-completion guard
 * still applies unchanged (spec section 6.3: workers never touch FSM
 * or registers).
 *
 * Historical fidelity note (retained from Phase 0.5): the null backend
 * does not compute. It reports retcode=DONE with a non-zero MBOX1
 * (neutron-uncomputed-errcode, default 0x95E0) so a guest that trusts
 * NPU output sees an honest fault instead of a silent wrong answer.
 * See docs/validation/fidelity-audit.md.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/imx95_neutron_compute.h"
#include "migration/blocker.h"
#include "qom/object.h"
#include "system/address-spaces.h"

#define TYPE_IMX95_NEUTRON "imx95.neutron"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95NeutronState, IMX95_NEUTRON)

/*
 * NPU-internal tightly-coupled memory the remoteproc loads NeutronFirmware.elf
 * into (imx_neutron_rproc att table): DTCM @0x4ab08000 (32K), ITCM @0x4ab10000
 * (64K). Backed as RAM so rproc_elf_load_segments' memcpy_toio does not abort.
 */
#define NEUTRON_DTCM_SIZE     0x8000
#define NEUTRON_ITCM_SIZE     0x10000

/* RESETCTRL (remoteproc reg @0x4ab00000): clock gates. */
#define RESETCTRL_SIZE        0x4
#define RCTL_ZENV_CLK_ON      (0x1u << 0)
#define RCTL_COMPUTE_CLK_ON   (0xfu << 4)
#define RCTL_TCM_CLK_ON       (0xfu << 8)

/* Device register window (@0x4ab00004), offsets relative to its base. */
#define NEUTRON_DEV_SIZE      0x400
#define N_STATUSERR           0x000
#define N_INTENA              0x004
#define N_INTCLR              0x008
#define N_APPCTRL             0x1fc
#define N_APPSTATUS           0x200
#define N_BASEDDRL            0x204
#define N_BASEDDRH            0x208
#define N_TAIL                0x234
#define N_HEAD                0x238
#define N_MBOX0               0x23c   /* response retcode (driver completion) */
#define N_MBOX1               0x240   /* response error_code -> guest app */
#define N_MBOX3               0x248   /* command word */
#define N_MBOX4               0x24c   /* cmd arg0 (RUN: tensor_offset) */
#define N_MBOX5               0x250   /* cmd arg1 (RUN: microcode_offset) */
#define N_MBOX6               0x254   /* cmd arg2 (RUN: tensor_count) */
#define N_BASEINOUTL          0x27c
#define N_BASESPILLL          0x284

/* APPCTRL: bit2 = doorbell; [31:16] = firmware MBWR handshake */
#define N_APPCTRL_DOORBELL    (1u << 2)
#define N_APPCTRL_MBWR_UP     (0xF807u << 16)

/* APPSTATUS event bits */
#define N_APPSTATUS_INFDONE   (1u << 0)
#define N_APPSTATUS_MBOX      (1u << 4)

/* mailbox return codes (uapi: the driver keys completion on DONE) */
#define N_RET_RUN_ACK         0xA3     /* "doorbell received, running" */
#define N_RET_DONE            0xAD0
#define N_RET_RESET           0x0

/*
 * Nominal servicing delay between RUN_ACK and the deferred completion. Not
 * modelled inference time (the compute is proprietary); just a small
 * non-zero gap on QEMU_CLOCK_VIRTUAL so guests / qtests observe the
 * two-phase RUN_ACK -> DONE transition. Well within the driver's patient
 * result wait (100 us hrtimer + completion IRQ).
 */
#define NEUTRON_RUN_NS        1000

/* mailbox commands in MBOX3 (spec section 3.1). */
#define N_CMD_RUN             0x269
#define N_CMD_KERNELS         0x272
#define N_CMD_RESET           0x23637

/*
 * Guest-visible error_code family (MBOX1). 0x95E0..0x95E5 are backend-side
 * (see imx95_neutron_compute.h). 0x95E6 is the mailbox/protocol BUSY code
 * used when the front end rejects a new RUN/KERNELS while a job is live
 * (spec section 5, section 7).
 */
#define NEUTRON_ERR_BUSY      0x95E6u

/*
 * Default guest-visible error_code for an uncomputed inference. See the
 * long comment on IMX95NeutronState::uncomputed_errcode below.
 */
#define NEUTRON_UNCOMPUTED_ERRCODE_DEFAULT  0x95E0u

/*
 * FSM phase (spec section 2). ACKED is folded into RUNNING: the RUN_ACK
 * write and the backend submit both happen in the same BQL critical
 * section of neutron_doorbell(), so the guest never observes a stable
 * ACKED-that-isn't-RUNNING.
 */
typedef enum NeutronPhase {
    PHASE_IDLE = 0,
    PHASE_RUNNING,
    PHASE_COMPLETE,
} NeutronPhase;

struct IMX95NeutronState {
    SysBusDevice parent_obj;
    MemoryRegion rctl_iomem;        /* RESETCTRL @0x4ab00000 */
    MemoryRegion dev_iomem;         /* device regs @0x4ab00004 */
    MemoryRegion dtcm;              /* DTCM @0x4ab08000 (firmware data) */
    MemoryRegion itcm;              /* ITCM @0x4ab10000 (firmware code) */
    qemu_irq irq;                   /* GIC SPI 318 */

    uint32_t resetctrl;
    uint32_t regs[NEUTRON_DEV_SIZE / 4];
    bool started;                   /* NPU clocked + firmware "up" */
    bool irq_pending;

    /* FSM state (spec section 1.1). */
    NeutronPhase phase;
    uint64_t     job_seq;           /* monotonic, ++ on accepted RUN */
    uint64_t     active_seq;        /* seq of the still-valid job (0 = none) */
    uint32_t     pending_cmd;       /* MBOX3 latched at doorbell */
    QEMUTimer   *done_timer;        /* fires the guarded completion */
    struct NeutronJob *done_job;    /* job whose completion done_timer must publish */

    /*
     * Migration blocker registered while phase == RUNNING (spec section 9).
     * Rearmed on entry to RUNNING, removed on exit (COMPLETE / IDLE via
     * reset). Prevents saving a half-flighted inference; the blocker is
     * short-lived (a single inference is ms-scale).
     */
    Error *migrate_blocker;

    /* Compute backend seam (see imx95_neutron_compute.h). */
    const NeutronBackendOps *backend;
    void *backend_ctx;

    /*
     * Backend-selection knob (QOM property). The runner-only knobs live in
     * the runner backend TU or, in a null-only build, are absent entirely.
     */
    char    *compute_backend_name;

    /*
     * Runner-backend storage fields. Present here so a single front-end
     * struct works for both builds, but the QOM property NAMES for these
     * fields live in the runner backend (imx95_neutron_backend_runner.c) or
     * are absent entirely in a null-only build (see the stub's empty
     * neutron_backend_runner_add_props()). That is how this file stays free
     * of Track A property-name strings, which the hygiene test enforces.
     */
    char    *runner_path;
    char    *runner_fixtures;
    char    *runner_scratch_dir;
    uint32_t runner_timeout_ms;
    bool     keep_scratch;

    /*
     * Honesty/telemetry: the null backend acks inferences without computing.
     * This counts RUNs that completed with uncomputed output so an operator
     * can query via QMP qom-get.
     */
    uint64_t acked_uncomputed;

    /*
     * Operator opt-in honest-fault to the guest. See long comment retained
     * from Phase 0.5 in the property registration path. Preserved across
     * reset (operator config, not device state).
     */
    uint32_t uncomputed_errcode;
};

/*
 * Heap job: everything the (possibly asynchronous) backend needs, plus the
 * seq the done-BH validates. Freed in the done-BH (both valid and stale
 * paths). Register writes never happen from the backend/worker; they only
 * happen in neutron_done_bh() under BQL after the guard passes.
 */
typedef struct NeutronJob {
    IMX95NeutronState *dev;
    uint64_t           seq;
    NeutronComputeReq  req;
    NeutronComputeRes  res;
} NeutronJob;

static inline uint32_t ndev_r(IMX95NeutronState *s, hwaddr off)
{
    return s->regs[off >> 2];
}

static inline void ndev_w(IMX95NeutronState *s, hwaddr off, uint32_t v)
{
    s->regs[off >> 2] = v;
}

static void neutron_update_irq(IMX95NeutronState *s)
{
    /* HW raises the line on a pending event while any IRQ source is enabled. */
    bool level = s->irq_pending && ndev_r(s, N_INTENA) != 0;
    qemu_set_irq(s->irq, level);
}

/* ---- migration blocker (spec section 9) ---- */

static void neutron_migrate_block(IMX95NeutronState *s)
{
    if (s->migrate_blocker) {
        return;
    }
    error_setg(&s->migrate_blocker,
               "imx95-neutron: cannot migrate while an inference is in flight");
    if (migrate_add_blocker(&s->migrate_blocker, NULL) != 0) {
        /*
         * Non-fatal: if the blocker cannot be registered (e.g. very early
         * bring-up), we let the run proceed. Real migration would still
         * refuse via VMState, and the inference is short.
         */
        error_free(s->migrate_blocker);
        s->migrate_blocker = NULL;
    }
}

static void neutron_migrate_unblock(IMX95NeutronState *s)
{
    if (!s->migrate_blocker) {
        return;
    }
    migrate_del_blocker(&s->migrate_blocker);
    s->migrate_blocker = NULL;
}

/* ---- BUSY reject helper (spec sections 7.1, 7.2, 7.3) ---- */

/*
 * Unified BUSY reject path. Called for:
 *   - RUN while phase != IDLE (RUNNING: 7.1; COMPLETE: 7.3 stuck-guard)
 *   - KERNELS/OTHER while phase == RUNNING (7.2)
 *
 * Posts DONE + NEUTRON_ERR_BUSY, raises IRQ, does NOT touch phase,
 * active_seq, or job_seq (the live job keeps its claim).
 */
static void neutron_reject_busy(IMX95NeutronState *s, uint32_t cmd)
{
    (void)cmd;
    ndev_w(s, N_MBOX0, N_RET_DONE);
    ndev_w(s, N_MBOX1, NEUTRON_ERR_BUSY);
    ndev_w(s, N_APPSTATUS,
           ndev_r(s, N_APPSTATUS) | N_APPSTATUS_INFDONE | N_APPSTATUS_MBOX);
    s->irq_pending = true;
    neutron_update_irq(s);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "imx95-neutron: cmd 0x%x rejected as BUSY (active_seq=%"
                  PRIu64 ", phase=%d)\n",
                  cmd, s->active_seq, (int)s->phase);
}

/* ---- job lifecycle ---- */

static void neutron_job_free(NeutronJob *job)
{
    g_free(job);
}

/*
 * Snapshot the guest-posted request into the heap job (spec section 3.2).
 * Everything the backend needs must be captured here under BQL; after this
 * point the worker never re-reads a register, which is what makes register
 * scribbles during RUNNING harmless.
 */
static void neutron_snapshot_request(IMX95NeutronState *s, NeutronComputeReq *req)
{
    req->cmd              = s->pending_cmd;
    req->carveout_base    = ((uint64_t)ndev_r(s, N_BASEDDRH) << 32) |
                            ndev_r(s, N_BASEDDRL);
    req->tensor_offset    = ndev_r(s, N_MBOX4);
    req->microcode_offset = ndev_r(s, N_MBOX5);
    req->tensor_count     = ndev_r(s, N_MBOX6);
    req->as               = &address_space_memory;
    req->uncomputed_errcode_hint = s->uncomputed_errcode;
}

static void neutron_raise_irq(IMX95NeutronState *s)
{
    s->irq_pending = true;
    neutron_update_irq(s);
}

/* ---- doorbell dispatch (spec section 4) ---- */

/*
 * The synchronous half of a RUN: RUN_ACK is posted, a heap job is
 * snapshotted, and the backend's run() is invoked. Today's backends are
 * synchronous under BQL, so run() fills job->res inline; the done-BH
 * (scheduled here) will do the guarded publish on the next main-loop
 * iteration. A future worker-thread backend calls run() from its worker
 * and only schedules the done-BH from the worker tail - the guard is the
 * same. See spec section 6.3.
 */
static void neutron_submit_run(IMX95NeutronState *s, NeutronJob *job)
{
    /* Backend fills job->res. Synchronous today; may become async later. */
    s->backend->run(s->backend_ctx, &job->req, &job->res);

    /*
     * Publish the completed job to the done-BH. Because we serialize on
     * BQL and queue depth is 1 (spec section 7), there is at most one
     * pending job at a time.
     */
    s->done_job = job;
    timer_mod(s->done_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NEUTRON_RUN_NS);
}

static void neutron_doorbell(IMX95NeutronState *s)
{
    uint32_t cmd = ndev_r(s, N_MBOX3);

    s->pending_cmd = cmd;

    switch (cmd) {
    case N_CMD_RUN: {
        NeutronJob *job;

        if (s->phase != PHASE_IDLE) {
            /* 7.1 (RUNNING) or 7.3 (COMPLETE stuck-guard). */
            neutron_reject_busy(s, cmd);
            return;
        }

        job = g_new0(NeutronJob, 1);
        job->dev = s;
        job->seq = ++s->job_seq;
        s->active_seq = job->seq;
        neutron_snapshot_request(s, &job->req);

        /* Post the synchronous half of the two-phase completion. */
        ndev_w(s, N_MBOX0, N_RET_RUN_ACK);
        s->phase = PHASE_RUNNING;
        neutron_migrate_block(s);

        neutron_submit_run(s, job);
        return;
    }

    case N_CMD_KERNELS:
        if (s->phase == PHASE_RUNNING) {
            /* 7.2: reject KERNELS mid-inference. */
            neutron_reject_busy(s, cmd);
            return;
        }
        /*
         * KERNELS in IDLE or COMPLETE latches the kernel_offset (arg in
         * MBOX4) and completes immediately. Item 1 keeps the simplistic
         * legacy handling (immediate DONE + IRQ); a later item can plumb
         * the offset into the job snapshot per spec section 3.2.
         */
        ndev_w(s, N_MBOX0, N_RET_DONE);
        ndev_w(s, N_MBOX1, 0);
        ndev_w(s, N_APPSTATUS,
               ndev_r(s, N_APPSTATUS) | N_APPSTATUS_INFDONE | N_APPSTATUS_MBOX);
        neutron_raise_irq(s);
        return;

    default:
        if (s->phase == PHASE_RUNNING) {
            /* 7.2: reject OTHER while a job is live. */
            neutron_reject_busy(s, cmd);
            return;
        }
        /* Silent-ack legacy path for unknown/OTHER commands. */
        ndev_w(s, N_MBOX0, N_RET_DONE);
        ndev_w(s, N_MBOX1, 0);
        ndev_w(s, N_APPSTATUS,
               ndev_r(s, N_APPSTATUS) | N_APPSTATUS_INFDONE | N_APPSTATUS_MBOX);
        neutron_raise_irq(s);
        return;
    }
}

/* ---- deferred completion (spec section 6, "done-BH") ---- */

static void neutron_done_run(void *opaque)
{
    IMX95NeutronState *s = opaque;
    NeutronJob *job = s->done_job;

    if (!job) {
        return;
    }
    s->done_job = NULL;

    /* Spec section 6.2 stale-completion guard. */
    if (s->phase != PHASE_RUNNING || job->seq != s->active_seq) {
        qemu_log_mask(LOG_UNIMP,
                      "imx95-neutron: stale completion dropped "
                      "(job_seq=%" PRIu64 ", active_seq=%" PRIu64
                      ", phase=%d)\n",
                      job->seq, s->active_seq, (int)s->phase);
        /* TODO(Item 3): trace_imx95_neutron_stale_done(job->seq, s->active_seq) */
        neutron_job_free(job);
        return;
    }

    /* Valid completion: publish. */
    ndev_w(s, N_MBOX0, job->res.retcode);
    ndev_w(s, N_MBOX1, job->res.guest_errcode);
    ndev_w(s, N_APPSTATUS,
           ndev_r(s, N_APPSTATUS) | N_APPSTATUS_INFDONE | N_APPSTATUS_MBOX);
    s->phase = PHASE_COMPLETE;
    neutron_raise_irq(s);

    /*
     * Migration blocker: an inference is no longer in flight. RUNNING was
     * the only phase that blocked; COMPLETE waits for the guest's RESET
     * and is safe to migrate.
     */
    neutron_migrate_unblock(s);

    if (job->req.cmd == N_CMD_RUN && !job->res.output_written) {
        s->acked_uncomputed++;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "imx95-neutron: inference #%" PRIu64 " acked (DONE) but "
                      "NOT computed - output is uncomputed (backend '%s' does "
                      "not model compute). guest error_code (MBOX1) = 0x%x%s\n",
                      s->acked_uncomputed, s->backend->name,
                      job->res.guest_errcode,
                      job->res.guest_errcode ? "" :
                      " (happy-path opt-out: neutron-uncomputed-errcode = 0, "
                      "guest sees success despite uncomputed output)");
    }

    neutron_job_free(job);
}

static uint64_t neutron_rctl_read(void *opaque, hwaddr off, unsigned size)
{
    IMX95NeutronState *s = opaque;
    return s->resetctrl;
}

static void neutron_rctl_write(void *opaque, hwaddr off, uint64_t val,
                               unsigned size)
{
    IMX95NeutronState *s = opaque;

    s->resetctrl = val;
    /*
     * The driver gates the NPU by turning the ZENV/compute/TCM clocks on via
     * RESETCTRL. Once clocked, the firmware would set APPCTRL[31:16]=0xF807 to
     * report itself started; mirror that so wait-for-startup polls complete.
     */
    if (val & RCTL_ZENV_CLK_ON) {
        s->started = true;
        ndev_w(s, N_APPCTRL,
               (ndev_r(s, N_APPCTRL) & 0xffff) | N_APPCTRL_MBWR_UP);
    } else {
        s->started = false;
        ndev_w(s, N_APPCTRL, ndev_r(s, N_APPCTRL) & 0xffff);
    }
}

static uint64_t neutron_dev_read(void *opaque, hwaddr off, unsigned size)
{
    IMX95NeutronState *s = opaque;

    if (off >= NEUTRON_DEV_SIZE) {
        return 0;
    }
    if (off == N_STATUSERR) {
        return 0;                    /* no fault */
    }
    return ndev_r(s, off);
}

/*
 * Mailbox RESET (spec section 6, "soft reset"). Not doorbelled - the driver
 * writes RESET to MBOX3 and polls MBOX0 for RESET_VAL. Disown any live
 * worker via active_seq=0; a subsequent stale done-BH will fail the guard
 * and drop silently. The migration blocker is released too.
 */
static void neutron_soft_reset(IMX95NeutronState *s)
{
    s->active_seq = 0;
    s->phase = PHASE_IDLE;
    ndev_w(s, N_MBOX0, N_RET_RESET);
    ndev_w(s, N_MBOX1, 0);
    neutron_migrate_unblock(s);
    /* job_seq is retained across soft reset per spec section 8. */
    /* TODO(Item 3): trace_imx95_neutron_reset("soft") */
}

static void neutron_dev_write(void *opaque, hwaddr off, uint64_t val,
                              unsigned size)
{
    IMX95NeutronState *s = opaque;

    if (off >= NEUTRON_DEV_SIZE) {
        return;
    }

    switch (off) {
    case N_APPCTRL:
        /* Keep the firmware-owned MBWR handshake in [31:16]; take the rest. */
        ndev_w(s, N_APPCTRL, (val & 0xffff) |
               (s->started ? N_APPCTRL_MBWR_UP : 0));
        if (val & N_APPCTRL_DOORBELL) {
            neutron_doorbell(s);
        }
        return;
    case N_INTCLR:
        s->irq_pending = false;      /* driver ack lowers the line */
        neutron_update_irq(s);
        return;
    case N_APPSTATUS:
        /* W1C: the driver clears event bits it has handled. */
        ndev_w(s, N_APPSTATUS, ndev_r(s, N_APPSTATUS) & ~(uint32_t)val);
        return;
    case N_INTENA:
        ndev_w(s, N_INTENA, val);
        neutron_update_irq(s);
        return;
    case N_MBOX3:
        ndev_w(s, N_MBOX3, val);
        /*
         * RESET is the one command the driver does NOT ring the doorbell
         * for; it writes RESET into MBOX3 and polls MBOX0 for RESET_VAL.
         * Handled synchronously here per spec section 6/8.
         */
        if (val == N_CMD_RESET) {
            neutron_soft_reset(s);
        }
        return;
    default:
        ndev_w(s, off, val);
        return;
    }
}

static const MemoryRegionOps neutron_rctl_ops = {
    .read = neutron_rctl_read,
    .write = neutron_rctl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static const MemoryRegionOps neutron_dev_ops = {
    .read = neutron_dev_read,
    .write = neutron_dev_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/*
 * Device reset (spec section 8, "hard reset"). Strongest transition: any
 * state -> IDLE, all MBOX/APPSTATUS/INTENA cleared, IRQ dropped, worker
 * disowned via active_seq=0. job_seq is retained (monotonic across soft
 * resets; only cold DeviceReset would zero it, but even that is optional).
 */
static void neutron_reset(DeviceState *dev)
{
    IMX95NeutronState *s = IMX95_NEUTRON(dev);

    s->resetctrl = 0;
    memset(s->regs, 0, sizeof(s->regs));
    s->started = false;
    s->irq_pending = false;
    s->acked_uncomputed = 0;
    s->pending_cmd = 0;
    s->phase = PHASE_IDLE;
    s->active_seq = 0;
    /* Do NOT touch s->done_job here: any pending BH will fail the guard. */
    neutron_migrate_unblock(s);
    /* TODO(Item 3): trace_imx95_neutron_reset("device") */
    /*
     * uncomputed_errcode is operator config (property), not device state - do
     * not clear it on reset, so a -global / qom-set value survives a guest
     * reboot.
     */
    qemu_set_irq(s->irq, 0);
}

/*
 * Honest fidelity flag: does the bound backend actually compute? False for
 * the default null backend, true once a runner backend was successfully
 * bound in realize (see compute-backend=runner path).
 */
static bool neutron_get_compute_modelled(Object *obj, Error **errp)
{
    IMX95NeutronState *s = IMX95_NEUTRON(obj);
    return s->backend && s->backend != &neutron_backend_null;
}

static void neutron_realize(DeviceState *dev, Error **errp)
{
    IMX95NeutronState *s = IMX95_NEUTRON(dev);

    /*
     * Runtime-queryable fidelity signal (QMP qom-get): does the NPU actually
     * compute, and how many inferences have completed with uncomputed output.
     */
    object_property_add_bool(OBJECT(dev), "compute-modelled",
                             neutron_get_compute_modelled, NULL);
    object_property_add_uint64_ptr(OBJECT(dev), "inferences-acked-uncomputed",
                                   &s->acked_uncomputed, OBJ_PROP_FLAG_READ);
    /*
     * Honest-fault error_code the model returns to the guest (MBOX1) on each
     * uncomputed inference. Non-zero by default (seeded in instance_init) =
     * guest-visible honest fault; set to 0 for silicon-faithful happy-path
     * success. Settable at runtime via qom-set.
     */
    object_property_add_uint32_ptr(OBJECT(dev), "neutron-uncomputed-errcode",
                                   &s->uncomputed_errcode,
                                   OBJ_PROP_FLAG_READWRITE);

    s->done_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, neutron_done_run, s);
    s->done_job = NULL;

    /*
     * Bind the requested compute backend. "null" (default) preserves the
     * historical honest-fault behaviour; "runner" spawns the operator-supplied
     * neutron-runner ELF per RUN.
     */
    if (!s->backend) {
        const char *want = s->compute_backend_name ? s->compute_backend_name
                                                   : "null";
        if (g_strcmp0(want, "runner") == 0) {
            NeutronRunnerCfg cfg = {
                .runner_path   = s->runner_path,
                .fixtures_path = s->runner_fixtures,
                .scratch_dir   = s->runner_scratch_dir,
                .timeout_ms    = s->runner_timeout_ms,
                .keep_scratch  = s->keep_scratch,
            };
            Error *local = NULL;
            void *rctx = neutron_backend_runner_new(&cfg, &local);
            if (rctx) {
                s->backend = &neutron_backend_runner;
                s->backend_ctx = rctx;
            } else if (neutron_backend_runner_is_stub()) {
                error_setg(errp, "imx95-neutron: compute-backend=runner was "
                           "requested, but this build has no runner backend "
                           "(%s). Rebuild with CONFIG_IMX95_NEUTRON_RUNNER=y "
                           "or use compute-backend=null.",
                           local ? error_get_pretty(local) : "stub");
                error_free(local);
                return;
            } else {
                warn_report("imx95-neutron: compute-backend=runner unavailable "
                            "(%s); falling back to null",
                            local ? error_get_pretty(local) : "unknown");
                error_free(local);
                s->backend = &neutron_backend_null;
                s->backend_ctx = NULL;
            }
        } else if (g_strcmp0(want, "null") == 0) {
            s->backend = &neutron_backend_null;
            s->backend_ctx = NULL;
        } else {
            error_setg(errp, "imx95-neutron: unknown compute-backend '%s' "
                       "(expected 'null' or 'runner')", want);
            return;
        }
    }

    memory_region_init_io(&s->rctl_iomem, OBJECT(dev), &neutron_rctl_ops, s,
                          "imx95.neutron.resetctrl", RESETCTRL_SIZE);
    memory_region_init_io(&s->dev_iomem, OBJECT(dev), &neutron_dev_ops, s,
                          TYPE_IMX95_NEUTRON, NEUTRON_DEV_SIZE);
    memory_region_init_ram(&s->dtcm, OBJECT(dev), "imx95.neutron.dtcm",
                           NEUTRON_DTCM_SIZE, &error_fatal);
    memory_region_init_ram(&s->itcm, OBJECT(dev), "imx95.neutron.itcm",
                           NEUTRON_ITCM_SIZE, &error_fatal);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->rctl_iomem);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->dev_iomem);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->dtcm);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->itcm);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void neutron_init(Object *obj)
{
    IMX95NeutronState *s = IMX95_NEUTRON(obj);

    /*
     * Seed the honest-fault default here (not in reset) so it behaves like
     * operator configuration: a -global or qom-set override survives, and it
     * is not clobbered on a guest reboot. reset deliberately leaves it
     * untouched.
     */
    s->uncomputed_errcode = NEUTRON_UNCOMPUTED_ERRCODE_DEFAULT;
}

static const Property neutron_props[] = {
    DEFINE_PROP_STRING("compute-backend",           IMX95NeutronState,
                       compute_backend_name),
};

static void neutron_unrealize(DeviceState *dev)
{
    IMX95NeutronState *s = IMX95_NEUTRON(dev);

    if (s->done_timer) {
        timer_free(s->done_timer);
        s->done_timer = NULL;
    }
    /*
     * A pending job at unrealize time is a leak on the guest side (the
     * completion will never be observed); free the memory so valgrind is
     * clean.
     */
    if (s->done_job) {
        neutron_job_free(s->done_job);
        s->done_job = NULL;
    }
    neutron_migrate_unblock(s);
    if (s->backend == &neutron_backend_runner) {
        neutron_backend_runner_free(s->backend_ctx);
        s->backend_ctx = NULL;
    }
}

static void neutron_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = neutron_realize;
    dc->unrealize = neutron_unrealize;
    device_class_set_legacy_reset(dc, neutron_reset);
    device_class_set_props(dc, neutron_props);
    {
        /*
         * Delegate the runner-backend property registration to the runner
         * backend (real backend registers them; stub registers nothing so a
         * null-only build has no runner properties on its QOM surface).
         */
        const NeutronRunnerPropOffsets off = {
            .runner_path_off  = offsetof(IMX95NeutronState, runner_path),
            .fixtures_off     = offsetof(IMX95NeutronState, runner_fixtures),
            .scratch_dir_off  = offsetof(IMX95NeutronState, runner_scratch_dir),
            .timeout_ms_off   = offsetof(IMX95NeutronState, runner_timeout_ms),
            .keep_scratch_off = offsetof(IMX95NeutronState, keep_scratch),
        };
        neutron_backend_runner_add_props(dc, &off);
    }
    dc->desc = "NXP i.MX 95 Neutron NPU (bring-up)";
}

static const TypeInfo imx95_neutron_info = {
    .name           = TYPE_IMX95_NEUTRON,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(IMX95NeutronState),
    .instance_init  = neutron_init,
    .class_init     = neutron_class_init,
};

static void imx95_neutron_register_types(void)
{
    type_register_static(&imx95_neutron_info);
}

type_init(imx95_neutron_register_types)
