/*
 * NXP i.MX 95 Neutron NPU — bring-up model (mailbox responder + remoteproc)
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The i.MX 95 Neutron NPU is a firmware-driven accelerator: Linux's staging
 * driver (drivers/staging/neutron) loads NeutronFirmware.elf onto the NPU's own
 * core via a remoteproc, then drives inference over a register mailbox. The NPU
 * core ISA, the firmware, and the compiled-model command stream are all NXP-
 * proprietary (unlike the i.MX 93's open Arm Ethos-U), so the *compute* cannot
 * be modelled from public material. What CAN be modelled (and what this does)
 * is the whole software bring-up path, so the driver binds and the TFLite /
 * LiteRT Neutron delegate initialises and runs inferences to completion:
 *
 *   - RESETCTRL (the remoteproc's 4-byte window @0x4ab00000) is the clock gate;
 *     once the driver turns the ZENV clock on we report the NPU "started" by
 *     setting APPCTRL[31:16] = 0xF807 (the firmware's "I'm up" handshake the
 *     driver polls for).
 *   - The device window (@0x4ab00004) carries the mailbox: the driver writes a
 *     command to MBOX3 (+args) and rings the doorbell (APPCTRL bit2); we reply
 *     DONE (0xAD0) in MBOX0, set APPSTATUS INFDONE|MBOX, and raise the
 *     completion IRQ (SPI 318). The driver ISR reads MBOX0, sees DONE, and the
 *     inference completes. The output buffer is NOT computed.
 *
 * So inferences run end to end but return uncomputed output — a "brings up"
 * model, not a functional NPU. See docs/imx95/known-limitations.md.
 *
 * Because the output is uncomputed, a guest that trusts NPU results would get a
 * silent wrong answer - the worst fidelity bug class. Completion gates only on
 * MBOX0 == DONE, but the driver also reads an error_code from MBOX1 and copies
 * it to userspace (NEUTRON_IOCTL_INFERENCE_STATE). So the model faults honestly
 * by default: the "neutron-uncomputed-errcode" property defaults to a non-zero
 * value (0x95E0), making the model return that recognisable error_code in MBOX1
 * for every uncomputed inference - a guest-visible fault, without hanging the
 * driver (MBOX0 stays DONE). Set the property to 0 to opt back into silicon-
 * faithful happy-path success. See docs/validation/fidelity-audit.md.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qom/object.h"

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
 * Nominal servicing latency between the RUN_ACK and DONE phases (see
 * neutron_doorbell). This is NOT modelled inference time - the compute is
 * proprietary and unmodelled - just a small non-zero gap so the driver observes
 * the two-phase RUN_ACK -> DONE transition it sees on silicon, rather than a
 * single synchronous DONE. Well within the driver's patient result wait (a
 * 100 us poll hrtimer, or the completion IRQ).
 */
#define NEUTRON_RUN_NS        1000

/* mailbox commands in MBOX3 (driver: RUN = run inference, RESET = re-arm). */
#define N_CMD_RUN             0x269
#define N_CMD_RESET           0x23637

/*
 * Default guest-visible error_code for an uncomputed inference. Non-zero by
 * default: the NPU compute is proprietary and unmodelled, so an inference that
 * "completes" returns uncomputed output - an honest fault to the guest beats a
 * silent wrong answer. 0x95E0 reads as "95, Error, 0" and is recognisable in a
 * guest's NEUTRON_IOCTL_INFERENCE_STATE. Set the property to 0 to opt back into
 * happy-path (silicon-faithful success) for a guest that tolerates uncomputed
 * output (e.g. a delegate that offloads 0 nodes and falls back to CPU).
 */
#define NEUTRON_UNCOMPUTED_ERRCODE_DEFAULT  0x95E0u

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

    /*
     * Two-phase mailbox: the doorbell posts RUN_ACK synchronously and arms this
     * timer, which posts DONE (the completion) once the nominal servicing delay
     * elapses. pending_cmd carries the command word across the two phases.
     */
    QEMUTimer *done_timer;
    uint32_t pending_cmd;

    /*
     * Honesty/telemetry: the NPU acks inferences without computing them (the
     * compute is proprietary firmware). This counts RUN commands so an operator
     * can query, via QMP qom-get, how many inferences "completed" with
     * uncomputed output.
     */
    uint64_t acked_uncomputed;

    /*
     * Operator opt-in honest-fault to the GUEST (not just the operator). The
     * driver completes an inference purely on MBOX0 == DONE; it then reads an
     * error_code from MBOX1 and surfaces it to userspace via the
     * NEUTRON_IOCTL_INFERENCE_STATE ioctl (struct neutron_uapi_result_status
     * .error_code). So a non-zero MBOX1 alongside MBOX0 == DONE signals "this
     * inference did not really compute" to a guest app WITHOUT hanging the
     * driver (completion does not depend on MBOX1).
     *
     * Default NEUTRON_UNCOMPUTED_ERRCODE_DEFAULT (0x95E0) = honest fault: every
     * uncomputed RUN emits that recognisable error_code, so a guest that trusts
     * NPU output learns it did not really compute rather than getting a silent
     * wrong answer. Set to 0 at runtime
     * (qom-set /machine/soc/neutron neutron-uncomputed-errcode 0) to opt back
     * into silicon-faithful happy-path success for a guest that tolerates
     * uncomputed output. Preserved across reset (operator config, not state).
     */
    uint32_t uncomputed_errcode;
};

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

/*
 * DONE phase (deferred): the "inference" has finished. Post the completion
 * retcode, raise the event flags + IRQ, and (for RUN) the honest-fault
 * error_code. The proprietary compute is not modelled, so the output buffer at
 * BASEINOUT is left untouched.
 */
static void neutron_done(void *opaque)
{
    IMX95NeutronState *s = opaque;
    uint32_t cmd = s->pending_cmd;

    ndev_w(s, N_MBOX0, N_RET_DONE);    /* the retcode the driver waits on */
    ndev_w(s, N_APPSTATUS,
           ndev_r(s, N_APPSTATUS) | N_APPSTATUS_INFDONE | N_APPSTATUS_MBOX);
    s->irq_pending = true;
    neutron_update_irq(s);

    if (cmd == N_CMD_RUN) {
        s->acked_uncomputed++;         /* an inference "completed" uncomputed */
        /*
         * Surface the honest-fault error_code to the guest in MBOX1. Non-zero
         * by default so a guest app's NEUTRON_IOCTL_INFERENCE_STATE sees an
         * error for this uncomputed inference rather than a silent wrong
         * answer; set the property to 0 to opt back into happy-path success.
         * MBOX0 stays DONE either way, so the driver never hangs.
         */
        ndev_w(s, N_MBOX1, s->uncomputed_errcode);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "imx95-neutron: inference #%" PRIu64 " acked (DONE) but "
                      "NOT computed - output is uncomputed (proprietary NPU "
                      "firmware not modelled). compute-modelled = false; "
                      "guest error_code (MBOX1) = 0x%x%s\n",
                      s->acked_uncomputed, s->uncomputed_errcode,
                      s->uncomputed_errcode ? "" :
                      " (happy-path opt-out: neutron-uncomputed-errcode = 0, "
                      "guest sees success despite uncomputed output)");
    } else {
        qemu_log_mask(LOG_UNIMP,
                      "imx95-neutron: cmd 0x%x acked (DONE)\n", cmd);
    }
}

/*
 * Service a mailbox command. On silicon the NPU is a two-phase mailbox: the
 * doorbell is acknowledged IMMEDIATELY with RUN_ACK (the driver's
 * mbox_send_data polls MBOX0 != 0 within ~20 us and returns -ETIME otherwise),
 * and the completion DONE lands later, once the engine finishes. We split it
 * the same way - RUN_ACK synchronously, DONE from neutron_done after a nominal
 * delay - so the driver observes the RUN_ACK -> DONE transition it does on
 * hardware instead of a single synchronous DONE. (The RESET command, handled in
 * neutron_dev_write, is not a doorbell and stays synchronous.)
 */
static void neutron_doorbell(IMX95NeutronState *s)
{
    s->pending_cmd = ndev_r(s, N_MBOX3);
    ndev_w(s, N_MBOX0, N_RET_RUN_ACK);
    timer_mod(s->done_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NEUTRON_RUN_NS);
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
         * RESET is the one command the driver does NOT ring the doorbell for:
         * mbox_send_reset() writes it and then polls MBOX0 for RESET_VAL,
         * relying on the running firmware to pick it up. Re-arm the mailbox
         * here, or the driver declares "failed to reset neutron state" and
         * hw-resets + reloads firmware before every subsequent inference.
         */
        if (val == N_CMD_RESET) {
            /*
             * Re-arm: abandon any inference whose DONE has not fired yet, or
             * its timer would clobber MBOX0 to DONE after we set RESET here.
             */
            timer_del(s->done_timer);
            ndev_w(s, N_MBOX0, N_RET_RESET);
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

static void neutron_reset(DeviceState *dev)
{
    IMX95NeutronState *s = IMX95_NEUTRON(dev);

    s->resetctrl = 0;
    memset(s->regs, 0, sizeof(s->regs));
    s->started = false;
    s->irq_pending = false;
    s->acked_uncomputed = 0;
    s->pending_cmd = 0;
    if (s->done_timer) {
        timer_del(s->done_timer);
    }
    /*
     * uncomputed_errcode is operator config (property), not device state - do
     * not clear it on reset, so a -global / qom-set value survives a guest
     * reboot.
     */
    qemu_set_irq(s->irq, 0);
}

/* Honest fidelity flag: this NPU does not model the compute. Always false. */
static bool neutron_get_compute_modelled(Object *obj, Error **errp)
{
    return false;
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
     * success. Settable at runtime via qom-set, like the ADC's operator-driven
     * conversion values.
     */
    object_property_add_uint32_ptr(OBJECT(dev), "neutron-uncomputed-errcode",
                                   &s->uncomputed_errcode,
                                   OBJ_PROP_FLAG_READWRITE);

    s->done_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, neutron_done, s);

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
     * operator configuration: a -global or qom-set override survives, and it is
     * not clobbered on a guest reboot. reset deliberately leaves it untouched.
     */
    s->uncomputed_errcode = NEUTRON_UNCOMPUTED_ERRCODE_DEFAULT;
}

static void neutron_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = neutron_realize;
    device_class_set_legacy_reset(dc, neutron_reset);
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
