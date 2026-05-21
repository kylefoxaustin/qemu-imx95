/*
 * NXP i.MX 95 SRC (System Reset Controller) mix-slice stub model
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Covers the SRC block at 0x44460000: SRC_GEN at offset 0, then the
 * per-power-domain "mix slice" register blocks (SRC_XSPR) starting at
 * offset 0x400, one every 0x400 (ANAMIX, AONMIX, ..., CCMSRCGPCMIX, ...).
 *
 * The System Manager powers a mix down by setting SLICE_SW_CTRL.PDN_SOFT
 * (bit 31, slice offset 0x20) and powers it up by clearing it, then polls
 * the read-only FUNC_STAT (slice offset 0xb4) until the power state
 * settles. QEMU has no power switches, so the transition is instantaneous:
 * FUNC_STAT is derived from the slice's SLICE_SW_CTRL.PDN_SOFT -
 *   powered up   -> RST_STAT released                  (0x00000004)
 *   powered down -> PSW off, ISO on, handshakes done    (0x00005511)
 * matching PWR_MIX_FUNC_STAT_PUP / _PDN in the SM's fsl_power.h. Everything
 * else is plain storage. Same status-mirrors-control idea as the GPC model.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"

#define TYPE_IMX95_SRC "imx95.src"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95SRCState, IMX95_SRC)

#define IMX95_SRC_REG_SIZE      0x10000
#define IMX95_SRC_NUM_WORDS     (IMX95_SRC_REG_SIZE / 4)

/* Mix-slice (SRC_XSPR) layout. SRC_GEN occupies the first slice-sized block. */
#define SRC_SLICE_STRIDE        0x400
#define SRC_SLICE_SW_CTRL       0x20    /* PDN_SOFT in bit 31 */
#define SRC_FUNC_STAT           0xb4    /* read-only power/reset status */
#define SRC_SLICE_SW_CTRL_PDN_SOFT  0x80000000u

/* FUNC_STAT values for the fully-up / fully-down states (SM fsl_power.h). */
#define SRC_FUNC_STAT_PUP       0x00000004u
#define SRC_FUNC_STAT_PDN       0x00005511u

struct IMX95SRCState {
    SysBusDevice    parent_obj;
    MemoryRegion    iomem;
    uint32_t        regs[IMX95_SRC_NUM_WORDS];
};

static uint64_t imx95_src_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX95SRCState *s = opaque;

    /* FUNC_STAT in a mix slice mirrors that slice's SLICE_SW_CTRL.PDN_SOFT. */
    if (offset >= SRC_SLICE_STRIDE &&
        (offset & (SRC_SLICE_STRIDE - 1)) == SRC_FUNC_STAT) {
        hwaddr slice = offset & ~(hwaddr)(SRC_SLICE_STRIDE - 1);
        uint32_t ctrl = s->regs[(slice + SRC_SLICE_SW_CTRL) / 4];

        return (ctrl & SRC_SLICE_SW_CTRL_PDN_SOFT) ? SRC_FUNC_STAT_PDN
                                                   : SRC_FUNC_STAT_PUP;
    }
    return s->regs[offset / 4];
}

static void imx95_src_write(void *opaque, hwaddr offset,
                            uint64_t value, unsigned size)
{
    IMX95SRCState *s = opaque;

    s->regs[offset / 4] = value;
}

static const MemoryRegionOps imx95_src_ops = {
    .read = imx95_src_read,
    .write = imx95_src_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void imx95_src_reset(DeviceState *dev)
{
    IMX95SRCState *s = IMX95_SRC(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void imx95_src_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    IMX95SRCState *s = IMX95_SRC(obj);

    memory_region_init_io(&s->iomem, obj, &imx95_src_ops, s,
                          TYPE_IMX95_SRC, IMX95_SRC_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_imx95_src = {
    .name = TYPE_IMX95_SRC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX95SRCState, IMX95_SRC_NUM_WORDS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx95_src_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_imx95_src;
    device_class_set_legacy_reset(dc, imx95_src_reset);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "NXP i.MX 95 SRC (stub)";
}

static const TypeInfo imx95_src_info = {
    .name           = TYPE_IMX95_SRC,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(IMX95SRCState),
    .instance_init  = imx95_src_init,
    .class_init     = imx95_src_class_init,
};

static void imx95_src_register_types(void)
{
    type_register_static(&imx95_src_info);
}

type_init(imx95_src_register_types)
