/*
 * NXP i.MX 95 DDR performance monitor (register compat)
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_IMX95_DDR_PMU_H
#define HW_MISC_IMX95_DDR_PMU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX95_DDR_PMU "imx95.ddr-pmu"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95DdrPmuState, IMX95_DDR_PMU)

#define IMX95_DDR_PMU_SIZE      0x200
#define IMX95_DDR_PMU_NUM_REGS  (IMX95_DDR_PMU_SIZE / 4)

struct IMX95DdrPmuState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[IMX95_DDR_PMU_NUM_REGS];
};

#endif /* HW_MISC_IMX95_DDR_PMU_H */
