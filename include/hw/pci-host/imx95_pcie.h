/*
 * NXP i.MX 95 PCIe Root Complex (DesignWare, unrolled iATU)
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The general-purpose PCIe controllers (pcie0/pcie1, "fsl,imx95-pcie") are a
 * newer Synopsys DesignWare core than QEMU's hw/pci-host/designware.c models:
 * the iATU is "unrolled" into a separate register region rather than the legacy
 * single VIEWPORT window inside dbi, and the imx95 link bring-up runs through a
 * syscon glue block (the hsio blk-ctl). This model reuses the DesignWare root
 * port / outbound-region machinery but drives it from the unrolled iATU region
 * and the glue. (Distinct from the NETC integrated-ECAM gpex host.)
 */

#ifndef IMX95_PCIE_H
#define IMX95_PCIE_H

#include "hw/core/sysbus.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci/pci_host.h"
#include "qom/object.h"

#define TYPE_IMX95_PCIE_ROOT_BUS "imx95-pcie-root-bus"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95PCIERootBus, IMX95_PCIE_ROOT_BUS)

#define TYPE_IMX95_PCIE_HOST "imx95-pcie-host"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95PCIEHost, IMX95_PCIE_HOST)

#define TYPE_IMX95_PCIE_ROOT "imx95-pcie-root"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95PCIERoot, IMX95_PCIE_ROOT)

#define IMX95_PCIE_NUM_VIEWPORTS  8      /* num-viewport = 8 */

struct IMX95PCIERootBus {
    PCIBus parent;
};

typedef struct IMX95PCIEViewport {
    IMX95PCIERoot *root;

    MemoryRegion cfg;
    MemoryRegion mem;

    uint64_t base;          /* CPU-side base (outbound)            */
    uint64_t target;        /* PCI target / bus|devfn for CFG      */
    uint64_t limit;         /* CPU-side limit (64-bit: incl upper) */
    uint32_t cr[2];         /* CTRL1 (type), CTRL2 (enable)        */

    bool inbound;
} IMX95PCIEViewport;

struct IMX95PCIERoot {
    PCIBridge parent_obj;

    /* Legacy VIEWPORT iATU selector (dbi 0x900); see imx95_pcie.c. */
    uint32_t atu_viewport;

    /* viewports[dir][index]; dir 0 = outbound, 1 = inbound. */
    IMX95PCIEViewport viewports[2][IMX95_PCIE_NUM_VIEWPORTS];
};

#define IMX95_PCIE_GLUE_REGS  (0x4000 / 4)  /* "app" SERDES/GPR window */
#define IMX95_PCIE_LUT_ENTRIES  32          /* requester-ID -> stream-ID LUT */

struct IMX95PCIEHost {
    PCIHostState parent_obj;

    IMX95PCIERoot root;

    struct {
        AddressSpace address_space;
        MemoryRegion address_space_root;
        MemoryRegion memory;
        MemoryRegion io;
        qemu_irq irqs[4];   /* INTA-D */
    } pci;

    MemoryRegion dbi;       /* 0x4c300000: DWC core / RP config       */
    MemoryRegion atu;       /* 0x4c360000: unrolled iATU              */
    MemoryRegion glue;      /* 0x4c340000: "app" SERDES/GPR glue regs */

    uint32_t glue_regs[IMX95_PCIE_GLUE_REGS];
    bool ltssm_en;          /* gates the dbi link-up bit              */

    /* Requester-ID -> stream-ID LUT (ACSCTRL latches the index). */
    uint32_t lut_data1[IMX95_PCIE_LUT_ENTRIES];
    uint32_t lut_data2[IMX95_PCIE_LUT_ENTRIES];
    uint32_t lut_index;
};

#endif /* IMX95_PCIE_H */
