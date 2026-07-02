/*
 * NXP i.MX 95 Clock Control Module (CCM) - clock-root register model
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_IMX95_CCM_H
#define HW_MISC_IMX95_CCM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX95_CCM "imx95.ccm"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95CcmState, IMX95_CCM)

#define IMX95_CCM_SIZE          (64 * 1024)
#define IMX95_CCM_NUM_REGS      (IMX95_CCM_SIZE / 4)

/* CLOCK_ROOT[123], each a 0x80 block: CONTROL (RW/SET/CLR/TOG @0x0..0xc),
 * STATUS0 @0x20, AUTHEN @0x30. */
#define IMX95_CCM_NUM_ROOT      123
#define IMX95_CCM_ROOT_STRIDE   0x80
#define IMX95_CCM_ROOT_END      (IMX95_CCM_NUM_ROOT * IMX95_CCM_ROOT_STRIDE)
#define IMX95_CCM_ROOT_STATUS0  0x20
/* Live-status-only bits that must not appear in the CONTROL->STATUS0 shadow. */
#define IMX95_CCM_STATUS0_LIVE  0x30000000u  /* SLICE_BUSY(28) | UPDATE_FWD(29) */

struct IMX95CcmState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[IMX95_CCM_NUM_REGS];
};

#endif /* HW_MISC_IMX95_CCM_H */
