/*
 * NXP i.MX 95 Clock Control Module (CCM) - clock-root register model
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The CCM at 0x44450000 is owned by the M33 System Manager, which programs the
 * clock roots and then answers the A55's SCMI CLOCK_RATE_GET by reading them
 * back: DEV_SM_ClockRateGet() -> CCM_RootGetRate() computes
 *   rate = source_rate / (CLOCK_ROOT[n].CONTROL.DIV + 1)
 * and CCM_RootGetSourceRate() picks the parent from CONTROL.MUX. Both the SM's
 * rate-set and mux-set are read-modify-writes of CONTROL.RW. So when the CCM was
 * a create_unimplemented_device() stub (writes dropped, reads 0), every clock
 * root read back DIV=0/MUX=0 and the SM reported the *undivided* source rate for
 * every clock to Linux over SCMI - a silent-wrong.
 *
 * This model backs the register file so those read-modify-writes persist. The
 * only non-trivial semantics is the per-root CONTROL register, which hardware
 * exposes as an RW/SET/CLR/TOG alias quad (offsets 0x0/0x4/0x8/0xc) plus a
 * read-only STATUS0 shadow (0x20). All four aliases fold into the single live
 * CONTROL word; STATUS0 reflects it (DIV/MUX/OFF share the same bit positions)
 * with the live-only SLICE_BUSY/UPDATE_FORWARD bits masked off so a "not busy"
 * status is always reported (nothing in the SM firmware polls them). Everything
 * else is plain read-what-you-write, which is correct for the GPR/LPCG/CGC
 * config registers the SM and Linux touch.
 */

#include "qemu/osdep.h"
#include "hw/misc/imx95_ccm.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

static uint64_t imx95_ccm_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX95CcmState *s = opaque;
    uint32_t idx = offset >> 2;

    if (idx >= IMX95_CCM_NUM_REGS) {
        return 0;
    }

    if (offset < IMX95_CCM_ROOT_END) {
        uint32_t sub = offset & (IMX95_CCM_ROOT_STRIDE - 1);
        uint32_t ctrl = (offset & ~(hwaddr)(IMX95_CCM_ROOT_STRIDE - 1)) >> 2;

        /* CONTROL RW/SET/CLR/TOG all read back the live control word. */
        if (sub < 0x10) {
            return s->regs[ctrl];
        }
        /* STATUS0 shadows CONTROL (aligned DIV/MUX/OFF), never busy. */
        if (sub == IMX95_CCM_ROOT_STATUS0) {
            return s->regs[ctrl] & ~IMX95_CCM_STATUS0_LIVE;
        }
    }
    return s->regs[idx];
}

static void imx95_ccm_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    IMX95CcmState *s = opaque;
    uint32_t idx = offset >> 2;

    if (idx >= IMX95_CCM_NUM_REGS) {
        return;
    }

    if (offset < IMX95_CCM_ROOT_END) {
        uint32_t sub = offset & (IMX95_CCM_ROOT_STRIDE - 1);
        uint32_t ctrl = (offset & ~(hwaddr)(IMX95_CCM_ROOT_STRIDE - 1)) >> 2;

        switch (sub) {
        case 0x0:                                   /* CONTROL.RW  */
            s->regs[ctrl] = value;
            return;
        case 0x4:                                   /* CONTROL.SET */
            s->regs[ctrl] |= (uint32_t)value;
            return;
        case 0x8:                                   /* CONTROL.CLR */
            s->regs[ctrl] &= ~(uint32_t)value;
            return;
        case 0xc:                                   /* CONTROL.TOG */
            s->regs[ctrl] ^= (uint32_t)value;
            return;
        case IMX95_CCM_ROOT_STATUS0:                /* read-only status */
            return;
        default:
            break;                                  /* AUTHEN etc: plain RW */
        }
    }
    s->regs[idx] = value;
}

static const MemoryRegionOps imx95_ccm_ops = {
    .read = imx95_ccm_read,
    .write = imx95_ccm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx95_ccm_reset(DeviceState *dev)
{
    IMX95CcmState *s = IMX95_CCM(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void imx95_ccm_realize(DeviceState *dev, Error **errp)
{
    IMX95CcmState *s = IMX95_CCM(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &imx95_ccm_ops, s,
                          TYPE_IMX95_CCM, IMX95_CCM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_imx95_ccm = {
    .name = TYPE_IMX95_CCM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX95CcmState, IMX95_CCM_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx95_ccm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx95_ccm_realize;
    dc->vmsd = &vmstate_imx95_ccm;
    device_class_set_legacy_reset(dc, imx95_ccm_reset);
    dc->desc = "i.MX 95 Clock Control Module (clock-root register model)";
}

static const TypeInfo imx95_ccm_types[] = {
    {
        .name          = TYPE_IMX95_CCM,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX95CcmState),
        .class_init    = imx95_ccm_class_init,
    },
};

DEFINE_TYPES(imx95_ccm_types)
