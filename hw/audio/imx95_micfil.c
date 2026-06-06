/*
 * NXP i.MX 95 MICFIL (PDM microphone interface)
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/audio/imx95_micfil.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

/* Register map */
#define MICFIL_CTRL1    0x00    /* Control 1                */
#define MICFIL_STAT     0x08    /* Status (read-only here)  */
#define MICFIL_VERID    0x84    /* Version ID (read-only)   */
#define MICFIL_PARAM    0x88    /* Parameter   (read-only)  */

#define MICFIL_CTRL1_SRES   (1u << 27)  /* Software reset (self-clearing) */

/* VERID: major 1, minor 0, feature 0. */
#define MICFIL_VERID_VALUE  0x01000000
/*
 * PARAM: FIFO_PTRWID = 3 (FIFO depth 8) and NPAIR = 4 (eight mic inputs). No
 * HWVAD reported, which keeps the voice-activity-detect path out of probe.
 */
#define MICFIL_PARAM_VALUE  0x00000034

static uint64_t imx95_micfil_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX95MicfilState *s = opaque;

    switch (offset) {
    case MICFIL_VERID:
        return MICFIL_VERID_VALUE;
    case MICFIL_PARAM:
        return MICFIL_PARAM_VALUE;
    case MICFIL_STAT:
        /* Not busy, no channel/error flags pending. */
        return 0;
    default:
        if ((offset >> 2) >= IMX95_MICFIL_REGS) {
            return 0;
        }
        return s->regs[offset >> 2];
    }
}

static void imx95_micfil_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    IMX95MicfilState *s = opaque;

    switch (offset) {
    case MICFIL_VERID:
    case MICFIL_PARAM:
        /* Read-only identification registers. */
        return;
    case MICFIL_CTRL1:
        /* Software reset is momentary: never latch it. */
        value &= ~MICFIL_CTRL1_SRES;
        break;
    default:
        break;
    }

    if ((offset >> 2) < IMX95_MICFIL_REGS) {
        s->regs[offset >> 2] = value;
    }
}

static const MemoryRegionOps imx95_micfil_ops = {
    .read = imx95_micfil_read,
    .write = imx95_micfil_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx95_micfil_reset(DeviceState *dev)
{
    IMX95MicfilState *s = IMX95_MICFIL(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void imx95_micfil_realize(DeviceState *dev, Error **errp)
{
    IMX95MicfilState *s = IMX95_MICFIL(dev);
    int i;

    memory_region_init_io(&s->iomem, OBJECT(dev), &imx95_micfil_ops, s,
                          TYPE_IMX95_MICFIL, IMX95_MICFIL_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    for (i = 0; i < IMX95_MICFIL_IRQS; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[i]);
    }
}

static const VMStateDescription vmstate_imx95_micfil = {
    .name = TYPE_IMX95_MICFIL,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX95MicfilState, IMX95_MICFIL_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx95_micfil_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx95_micfil_realize;
    dc->vmsd = &vmstate_imx95_micfil;
    device_class_set_legacy_reset(dc, imx95_micfil_reset);
    dc->desc = "i.MX 95 PDM microphone interface";
}

static const TypeInfo imx95_micfil_types[] = {
    {
        .name = TYPE_IMX95_MICFIL,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX95MicfilState),
        .class_init = imx95_micfil_class_init,
    },
};

DEFINE_TYPES(imx95_micfil_types)
