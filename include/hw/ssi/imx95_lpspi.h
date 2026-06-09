/*
 * NXP i.MX 95 Low Power SPI (LPSPI) master
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_IMX95_LPSPI_H
#define HW_SSI_IMX95_LPSPI_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qemu/fifo32.h"
#include "qom/object.h"

#define TYPE_IMX95_LPSPI "imx95.lpspi"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95LpspiState, IMX95_LPSPI)

#define IMX95_LPSPI_SIZE 0x10000

struct IMX95LpspiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    SSIBus *bus;
    char *bus_name;     /* optional unique SSI bus name for -device bus= */

    uint32_t cr;        /* control (enable/reset) */
    uint32_t sr;        /* status (latched TCF/FCF bits) */
    uint32_t ier;       /* interrupt enable */
    uint32_t cfgr1;     /* config */
    uint32_t tcr;       /* transmit command (frame size, CS, continuous) */
    Fifo32 rx;          /* received data words */
};

#endif /* HW_SSI_IMX95_LPSPI_H */
