/*
 * NXP i.MX 95 eDMA v3 (fsl,imx95-edma3) controller
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the management block plus per-channel pages of the eDMA engine
 * well enough to execute the TCD-described transfers the Linux fsl-edma
 * driver programs. Channel N's register page is at base + (N+1) * 0x10000;
 * each page holds the channel control registers (CH_CSR/ES/INT/SBR/...) and
 * the 32-byte TCD at offset 0x20. A transfer runs when CH_CSR.ERQ is set.
 */

#ifndef IMX95_EDMA_H
#define IMX95_EDMA_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX95_EDMA "imx95.edma3"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95EdmaState, IMX95_EDMA)

#define IMX95_EDMA_MAX_CHANNELS     64
#define IMX95_EDMA_CHAN_OFFSET      0x10000     /* first channel page */
#define IMX95_EDMA_CHAN_STRIDE      0x10000     /* default page size (edma3) */
#define IMX95_EDMA_CHAN_REGS_SZ     0x40        /* control regs + TCD */
#define IMX95_EDMA_MGMT_REGS        0x40        /* mgmt words 0x0..0xff */

typedef struct IMX95EdmaChan {
    uint8_t  regs[IMX95_EDMA_CHAN_REGS_SZ];     /* byte-addressable page head */
    bool     armed;             /* dev->mem deferred until data */
} IMX95EdmaChan;

struct IMX95EdmaState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t     num_channels;
    uint32_t     chan_stride;   /* page size: edma3 0x10000, edma4 0x8000 */

    uint32_t     mgmt[IMX95_EDMA_MGMT_REGS];
    IMX95EdmaChan chan[IMX95_EDMA_MAX_CHANNELS];
    qemu_irq     irq[IMX95_EDMA_MAX_CHANNELS];
};

#endif /* IMX95_EDMA_H */
