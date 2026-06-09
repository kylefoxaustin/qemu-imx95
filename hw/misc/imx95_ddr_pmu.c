/*
 * NXP i.MX 95 DDR performance monitor (register compat)
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The ddr-pmu@4e090dc0 ("fsl,imx95-ddr-pmu") is registered by the Linux
 * fsl_imx9_ddr_perf driver as a perf PMU, so userspace can open the events
 * (/sys/bus/event_source/devices/imx9_ddr0, `perf stat -e imx9_ddr0/.../`).
 *
 * IMPORTANT: this is a register/perf-interface COMPATIBILITY model, not a
 * measurement. QEMU's DRAM is plain host memory: guest CPU loads/stores reach
 * it through the TCG softmmu TLB, never crossing a memory controller we could
 * count, and there is no cache model, so real DDR bandwidth (post-cache-miss
 * traffic) has no meaning here. The block is read-what-you-write: the driver
 * configures and clears the counters to 0, nothing increments them, so every
 * count reads back 0 - honestly "no traffic measured" rather than a fiction.
 * The overflow interrupt is wired but never asserted (the counters never roll).
 */

#include "qemu/osdep.h"
#include "hw/misc/imx95_ddr_pmu.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

static uint64_t imx95_ddr_pmu_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX95DdrPmuState *s = opaque;

    if ((offset >> 2) >= IMX95_DDR_PMU_NUM_REGS) {
        return 0;
    }
    return s->regs[offset >> 2];
}

static void imx95_ddr_pmu_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    IMX95DdrPmuState *s = opaque;

    if ((offset >> 2) < IMX95_DDR_PMU_NUM_REGS) {
        s->regs[offset >> 2] = value;
    }
}

static const MemoryRegionOps imx95_ddr_pmu_ops = {
    .read = imx95_ddr_pmu_read,
    .write = imx95_ddr_pmu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx95_ddr_pmu_reset(DeviceState *dev)
{
    IMX95DdrPmuState *s = IMX95_DDR_PMU(dev);

    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void imx95_ddr_pmu_realize(DeviceState *dev, Error **errp)
{
    IMX95DdrPmuState *s = IMX95_DDR_PMU(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &imx95_ddr_pmu_ops, s,
                          TYPE_IMX95_DDR_PMU, IMX95_DDR_PMU_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_imx95_ddr_pmu = {
    .name = TYPE_IMX95_DDR_PMU,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX95DdrPmuState, IMX95_DDR_PMU_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx95_ddr_pmu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx95_ddr_pmu_realize;
    dc->vmsd = &vmstate_imx95_ddr_pmu;
    device_class_set_legacy_reset(dc, imx95_ddr_pmu_reset);
    dc->desc = "i.MX 95 DDR performance monitor (register compat)";
}

static const TypeInfo imx95_ddr_pmu_types[] = {
    {
        .name          = TYPE_IMX95_DDR_PMU,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX95DdrPmuState),
        .class_init    = imx95_ddr_pmu_class_init,
    },
};

DEFINE_TYPES(imx95_ddr_pmu_types)
