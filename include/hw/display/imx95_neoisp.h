/*
 * i.MX 95 NeoISP - camera image signal processor
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DISPLAY_IMX95_NEOISP_H
#define HW_DISPLAY_IMX95_NEOISP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX95_NEOISP "imx95.neoisp"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95NeoIspState, IMX95_NEOISP)

#define IMX95_NEOISP_REGS_SIZE  0x10000
#define IMX95_NEOISP_STATS_SIZE 0x10000

struct IMX95NeoIspState {
    SysBusDevice parent_obj;

    MemoryRegion regs_mr;
    MemoryRegion stats_mr;
    qemu_irq irq;

    /* Register file. The block has thousands of tuning registers; they are
     * kept verbatim so reads return what was written, and only the handful
     * that actually start work are interpreted. */
    uint32_t regs[IMX95_NEOISP_REGS_SIZE / 4];
    uint8_t  stats[IMX95_NEOISP_STATS_SIZE];

    /* v1 (imx95-a0) and v2 (imx95-b0) put the interrupt registers at
     * different offsets; the EVK is b0. */
    bool v2;
    uint64_t frames;            /* frames developed, for the qtest */
    bool warned_stages;         /* already said which stages are unmodelled */
};

#endif
