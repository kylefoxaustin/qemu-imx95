/*
 * NXP i.MX 95 SAI (Synchronous Audio Interface)
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The i.MX 95 has three SAI instances (sai1/2/3), each an I2S transmit/receive
 * front-end whose FIFOs are drained/filled by eDMA3. This model carries the
 * register file the fsl-sai driver needs to probe and the ASoC card to
 * register: a correct VERID/PARAM (so version/FIFO-depth detection succeeds)
 * plus self-clearing software-/FIFO-reset bits. Actual sample movement is left
 * to the eDMA datapath; no audio backend is attached.
 */

#ifndef IMX95_SAI_H
#define IMX95_SAI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX95_SAI "imx95.sai"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95SaiState, IMX95_SAI)

#define IMX95_SAI_SIZE   0x10000
/* Registers run from VERID (0x00) to MDIV (0x104); cover a little past that. */
#define IMX95_SAI_REGS   (0x108 / 4)

struct IMX95SaiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[IMX95_SAI_REGS];
};

#endif /* IMX95_SAI_H */
