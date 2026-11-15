/*
 * NXP i.MX 95 DDR controller (geometry reporting)
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_IMX95_DDRC_H
#define HW_MISC_IMX95_DDRC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX95_DDRC "imx95.ddrc"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95DdrcState, IMX95_DDRC)

/*
 * The block's window is 64 KiB so a stray read anywhere in it returns 0
 * instead of taking an external abort, but only the first 8 KiB is backed -
 * that covers every register any guest here reads, the highest being
 * ERR_EN at 0x1000.
 */
#define IMX95_DDRC_SIZE         0x10000
#define IMX95_DDRC_NUM_REGS     (0x2000 / 4)

struct IMX95DdrcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    /* DRAM the machine actually has, which is what the geometry reports. */
    uint64_t ram_size;

    uint32_t regs[IMX95_DDRC_NUM_REGS];
};

#endif /* HW_MISC_IMX95_DDRC_H */
