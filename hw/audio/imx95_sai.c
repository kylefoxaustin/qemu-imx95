/*
 * NXP i.MX 95 SAI (Synchronous Audio Interface)
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/audio/imx95_sai.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

/* Register map (reg_offset = 8: VERID/PARAM precede the control regs). */
#define SAI_VERID       0x00    /* Version ID (read-only)   */
#define SAI_PARAM       0x04    /* Parameter   (read-only)  */
#define SAI_TCSR        0x08    /* Transmit Control/Status  */
#define SAI_RCSR        0x88    /* Receive Control/Status   */

/* xCSR self-clearing bits */
#define SAI_CSR_FR      (1u << 25)  /* FIFO reset (self-clearing)     */
#define SAI_CSR_SR      (1u << 24)  /* Software reset (self-clearing) */

/*
 * VERID: major 3, minor 3, feature 0. A zero feature word keeps the timestamp
 * (TSTMP_EN) path out of the driver's probe, which is all we need here.
 */
#define SAI_VERID_VALUE 0x03030000
/*
 * PARAM: SPF (max slots/frame) = 5 -> 32 slots, WPF (FIFO depth) = 7 -> 128
 * words, DLN (datalines) = 4. Matches the imx95 soc_data the driver assumes.
 */
#define SAI_PARAM_VALUE 0x00050704

static uint64_t imx95_sai_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX95SaiState *s = opaque;

    switch (offset) {
    case SAI_VERID:
        return SAI_VERID_VALUE;
    case SAI_PARAM:
        return SAI_PARAM_VALUE;
    default:
        if ((offset >> 2) >= IMX95_SAI_REGS) {
            return 0;
        }
        return s->regs[offset >> 2];
    }
}

static void imx95_sai_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    IMX95SaiState *s = opaque;

    switch (offset) {
    case SAI_VERID:
    case SAI_PARAM:
        /* Read-only identification registers. */
        return;
    case SAI_TCSR:
    case SAI_RCSR:
        /* FIFO/software reset are momentary: never latch them. */
        value &= ~(SAI_CSR_FR | SAI_CSR_SR);
        break;
    default:
        break;
    }

    if ((offset >> 2) < IMX95_SAI_REGS) {
        s->regs[offset >> 2] = value;
    }
}

static const MemoryRegionOps imx95_sai_ops = {
    .read = imx95_sai_read,
    .write = imx95_sai_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx95_sai_reset(DeviceState *dev)
{
    IMX95SaiState *s = IMX95_SAI(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void imx95_sai_realize(DeviceState *dev, Error **errp)
{
    IMX95SaiState *s = IMX95_SAI(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &imx95_sai_ops, s,
                          TYPE_IMX95_SAI, IMX95_SAI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_imx95_sai = {
    .name = TYPE_IMX95_SAI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX95SaiState, IMX95_SAI_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx95_sai_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx95_sai_realize;
    dc->vmsd = &vmstate_imx95_sai;
    device_class_set_legacy_reset(dc, imx95_sai_reset);
    dc->desc = "i.MX 95 Synchronous Audio Interface";
}

static const TypeInfo imx95_sai_types[] = {
    {
        .name = TYPE_IMX95_SAI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX95SaiState),
        .class_init = imx95_sai_class_init,
    },
};

DEFINE_TYPES(imx95_sai_types)
