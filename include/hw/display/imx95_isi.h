/*
 * NXP i.MX 95 ISI (Image Sensing Interface) - capture channels
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_IMX95_ISI_H
#define HW_DISPLAY_IMX95_ISI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/timer.h"

#define TYPE_IMX95_ISI "imx95.isi"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95IsiState, IMX95_ISI)

/* 8 capture channels (pipes), each a 0x10000 register block. */
#define IMX95_ISI_NUM_CHANNELS  8
#define IMX95_ISI_CHANNEL_STRIDE 0x10000
#define IMX95_ISI_REG_SIZE \
    (IMX95_ISI_NUM_CHANNELS * IMX95_ISI_CHANNEL_STRIDE)
#define IMX95_ISI_NUM_REGS      (IMX95_ISI_REG_SIZE / 4)

typedef struct IMX95IsiState IMX95IsiState;

/* Per-channel frame-delivery context (the timer needs to know its channel). */
typedef struct IMX95IsiChan {
    IMX95IsiState *isi;
    int            index;
    QEMUTimer     *frame_timer;
    uint32_t       frame;
} IMX95IsiChan;

struct IMX95IsiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq     irq[IMX95_ISI_NUM_CHANNELS];
    uint32_t     regs[IMX95_ISI_NUM_REGS];
    IMX95IsiChan chan[IMX95_ISI_NUM_CHANNELS];
};

#endif /* HW_DISPLAY_IMX95_ISI_H */
