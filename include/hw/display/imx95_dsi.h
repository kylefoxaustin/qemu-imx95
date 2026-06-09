/*
 * NXP i.MX 95 MIPI-DSI host (Synopsys DesignWare DSI)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_IMX95_DSI_H
#define HW_DISPLAY_IMX95_DSI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX95_DSI "imx95.dsi"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95DsiState, IMX95_DSI)

#define IMX95_DSI_REG_SIZE  0x10000
#define IMX95_DSI_NUM_REGS  (IMX95_DSI_REG_SIZE / 4)

struct IMX95DsiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq     irq;
    uint32_t     regs[IMX95_DSI_NUM_REGS];
};

#endif /* HW_DISPLAY_IMX95_DSI_H */
