/*
 * Silvaco (Cadence) I3C master controller - "silvaco,i3c-master-v1"
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I3C_SVC_I3C_H
#define HW_I3C_SVC_I3C_H

#include "hw/core/sysbus.h"
#include "hw/i3c/i3c.h"
#include "qom/object.h"

#define TYPE_SVC_I3C "silvaco-i3c"
OBJECT_DECLARE_SIMPLE_TYPE(SvcI3cState, SVC_I3C)

#define SVC_I3C_NUM_REGS    (0x100 / 4)
#define SVC_I3C_RXFIFO      256

struct SvcI3cState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    I3CBus *bus;

    uint32_t regs[SVC_I3C_NUM_REGS];
    uint32_t mstatus;           /* sticky W1C status bits        */
    uint32_t merrwarn;          /* sticky W1C error bits         */
    uint32_t mintset;           /* interrupt enable mask         */

    uint8_t  rx[SVC_I3C_RXFIFO];
    uint32_t rx_count;
    uint32_t rx_head;
    bool     cur_i2c;           /* current transfer is legacy I2C */
};

#endif /* HW_I3C_SVC_I3C_H */
