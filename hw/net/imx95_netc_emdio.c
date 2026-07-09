/*
 * NXP i.MX 95 NETC EMDIO - external MDIO controller (PCI endpoint 1131:ee00)
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The NETC block exposes its external MDIO as a PCI function on the bus-1 ECAM
 * (netc-blk-ctrl@4cde0000/pcie@4cb00000/mdio@0,0). The enetc_pci_mdio driver
 * binds PCI 1131:ee00 and maps a clause-22/45 MDIO register file at BAR0 +
 * ENETC_EMDIO_BASE (0x1c00): CFG(0x0, BSY/RD_ER/ENC45), CTL(0x4, devad[4:0] |
 * port[9:5] | READ bit15), DATA(0x8), ADDR(0xc).
 *
 * The 19x19 EVK wires an Aquantia c45 10G PHY at MDIO addr 8 (ethernet-phy@8,
 * the ethernet@10,0 phy-handle). We embed a minimal c45 PHY at addr 8 so the
 * mdio bus registers and that phy-handle resolves (the 10G port's LINK itself
 * comes from the port's internal xPCS via managed = "in-band-status", modelled
 * separately). Every other MDIO address reads back "no device" (RD_ER).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"

#define TYPE_IMX95_NETC_EMDIO "imx95-netc-emdio"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95NetcEmdio, IMX95_NETC_EMDIO)

#define EMDIO_VENDOR_ID     0x1131      /* PCI_VENDOR_ID_NXP2 (Philips) */
#define EMDIO_DEVICE_ID     0xee00      /* PCI_DEVICE_ID_NXP2_NETC_EMDIO */

#define EMDIO_BAR0_SIZE     0x10000   /* 64K; fits the 4cb00000 window */
#define EMDIO_MDIO_BASE     0x1c00      /* ENETC_EMDIO_BASE */

/* MDIO register file (relative to EMDIO_MDIO_BASE). */
#define MDIO_CFG            0x0
#define MDIO_CTL            0x4
#define MDIO_DATA           0x8
#define MDIO_ADDR           0xc

#define MDIO_CFG_BSY        (1u << 0)
#define MDIO_CFG_RD_ER      (1u << 1)
#define MDIO_CFG_ENC45      (1u << 6)

#define MDIO_CTL_DEV_ADDR(x)  ((x) & 0x1f)
#define MDIO_CTL_PORT_ADDR(x) (((x) >> 5) & 0x1f)
#define MDIO_CTL_READ         (1u << 15)

#define AQR_PHY_ADDR        8

/* c45 status/id register numbers (per MMD). */
#define C45_STAT1           0x0001      /* MDIO_STAT1 (LSTATUS bit 2) */
#define C45_PHYSID1         0x0002
#define C45_PHYSID2         0x0003
#define C45_DEVS2           0x0005      /* devices-in-package, MMD 16..31 */
#define C45_DEVS1           0x0006      /* devices-in-package, MMD 1..15 */
#define C45_STAT2           0x0008      /* DEVPRST bits [15:14] */

#define MDIO_STAT1_LSTATUS  (1u << 2)
#define MDIO_STAT2_DEVPRST  0x8000      /* present: bit15 set, 14 clear */

/* c45 MMD (device address) numbers. */
#define MDIO_MMD_PMAPMD     1
#define MDIO_MMD_PCS        3
#define MDIO_MMD_PHYXS      4
#define MDIO_MMD_AN         7
#define MDIO_MMD_VEND1      30

/* PMA/PMD (MMD 1) speed-ability registers genphy_c45 reads for features. The
 * AQR113C is a multi-gig BASE-T PHY, so it reports copper 10G/5G/2.5G via the
 * extended-ability tables (must match the xPCS mx95 10G features: 10000baseT +
 * multi-gig T, not fibre SR/LR - an empty intersection fails validate). */
#define C45_PMA_STAT2_EXTABLE 0x0200
#define C45_PMA_EXTABLE       0x000b
#define C45_PMA_EXTABLE_10GBT 0x0004
#define C45_PMA_EXTABLE_NBT   0x4000    /* 2.5/5GBASE-T -> NG_EXTABLE */
#define C45_PMA_NG_EXTABLE    0x0015
#define C45_PMA_NG_EXTABLE_2_5G 0x0001
#define C45_PMA_NG_EXTABLE_5G   0x0002

/* Aquantia AQR113C vendor (MMD 30) registers the aqr driver probe touches. */
#define AQR_VEND1_FW_ID       0x0020    /* non-zero => fw loaded */
#define AQR_VEND1_GEN_STAT2   0xc831    /* bit15 OP_IN_PROG; return clear */
#define AQR_VEND1_CFG_10M     0x031a    /* media-speed serdes cfg regs */
#define AQR_VEND1_CFG_10G     0x031f    /* .. to 10G (SERDES_MODE [2:0]) */
#define AQR_CFG_SERDES_SGMII  0x0003    /* non-zero; low speeds report SGMII */
#define AQR_CFG_SERDES_XFI    0x0000    /* 10G reports XFI => 10GBASE-R */

/* Aquantia AQR113C: MII PHY id 0x31c31c12 (OUI 0x03a1b4, model/rev in id2). */
#define AQR_PHYSID1         0x31c3
#define AQR_PHYSID2         0x1c12
/* devices-in-package: PMA/PMD(1), PCS(3), PHY XS(4), AN(7) + vend 30/31 */
#define AQR_DEVS1           ((1u << 1) | (1u << 3) | (1u << 4) | (1u << 7))
#define AQR_DEVS2           ((1u << (30 - 16)) | (1u << (31 - 16)))

struct IMX95NetcEmdio {
    PCIDevice parent_obj;
    MemoryRegion bar0;
    uint32_t cfg;
    uint32_t ctl;
    uint32_t data;
    uint32_t addr;
};

/* Model the embedded Aquantia AQR113C c45 PHY at MDIO addr 8. */
static uint16_t aqr_c45_read(int devad, int reg)
{
    /* Aquantia vendor MMD: let the aqr driver's probe/config_init succeed. */
    if (devad == MDIO_MMD_VEND1) {
        if (reg == AQR_VEND1_FW_ID) {
            return 0x0501;              /* fw v5.1 present => skip fw load */
        }
        if (reg == AQR_VEND1_GEN_STAT2) {
            return 0;                   /* OP_IN_PROG clear (not busy) */
        }
        if (reg >= AQR_VEND1_CFG_10M && reg < AQR_VEND1_CFG_10G) {
            return AQR_CFG_SERDES_SGMII; /* low media speeds: SGMII, non-zero */
        }
        if (reg == AQR_VEND1_CFG_10G) {
            return AQR_CFG_SERDES_XFI;   /* 10G: XFI serdes => 10GBASE-R host */
        }
        return 0;
    }

    switch (reg) {
    case C45_DEVS1:
        return AQR_DEVS1;
    case C45_DEVS2:
        return AQR_DEVS2;
    case C45_PHYSID1:
        return AQR_PHYSID1;
    case C45_PHYSID2:
        return AQR_PHYSID2;
    case C45_STAT2:
        /*
         * PMA/PMD STAT2 doubles as the c45 device-present marker (bits[15:14])
         * and a speed-ability register. Set EXTABLE so genphy_c45 reads the
         * extended tables below for the copper 10G/multi-gig abilities.
         */
        if (devad == MDIO_MMD_PMAPMD) {
            return MDIO_STAT2_DEVPRST | C45_PMA_STAT2_EXTABLE;
        }
        return MDIO_STAT2_DEVPRST;
    case C45_PMA_EXTABLE:
        /* 10GBASE-T + the 2.5/5G marker (NG_EXTABLE has the detail). */
        if (devad == MDIO_MMD_PMAPMD) {
            return C45_PMA_EXTABLE_10GBT | C45_PMA_EXTABLE_NBT;
        }
        return 0;
    case C45_PMA_NG_EXTABLE:
        if (devad == MDIO_MMD_PMAPMD) {
            return C45_PMA_NG_EXTABLE_2_5G | C45_PMA_NG_EXTABLE_5G;
        }
        return 0;
    case C45_STAT1:
        /* PMA/PMD + PCS report link up: the phy sees a live 10G PHY. */
        if (devad == MDIO_MMD_PMAPMD || devad == MDIO_MMD_PCS ||
            devad == MDIO_MMD_PHYXS || devad == MDIO_MMD_AN) {
            return MDIO_STAT1_LSTATUS;
        }
        return 0;
    default:
        return 0;
    }
}

/* Run the MDIO transaction the driver just kicked (CTL write). */
static void emdio_do_ctl(IMX95NetcEmdio *s, uint32_t ctl)
{
    int port = MDIO_CTL_PORT_ADDR(ctl);
    int devad = MDIO_CTL_DEV_ADDR(ctl);

    s->ctl = ctl;
    s->cfg &= ~MDIO_CFG_RD_ER;
    if (!(ctl & MDIO_CTL_READ)) {
        return;                 /* address/port latch only */
    }
    if (port == AQR_PHY_ADDR) {
        s->data = aqr_c45_read(devad, s->addr & 0xffff);
    } else {
        s->cfg |= MDIO_CFG_RD_ER;   /* no device at this address */
        s->data = 0xffff;
    }
}

static uint64_t emdio_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX95NetcEmdio *s = opaque;

    if (offset < EMDIO_MDIO_BASE ||
        offset >= EMDIO_MDIO_BASE + 0x10) {
        return 0;
    }
    switch (offset - EMDIO_MDIO_BASE) {
    case MDIO_CFG:
        return s->cfg & ~MDIO_CFG_BSY;   /* never busy: instant completion */
    case MDIO_CTL:
        return s->ctl;
    case MDIO_DATA:
        return s->data;
    case MDIO_ADDR:
        return s->addr;
    default:
        return 0;
    }
}

static void emdio_write(void *opaque, hwaddr offset, uint64_t value,
                        unsigned size)
{
    IMX95NetcEmdio *s = opaque;

    if (offset < EMDIO_MDIO_BASE ||
        offset >= EMDIO_MDIO_BASE + 0x10) {
        return;
    }
    switch (offset - EMDIO_MDIO_BASE) {
    case MDIO_CFG:
        s->cfg = value;
        break;
    case MDIO_CTL:
        emdio_do_ctl(s, value);
        break;
    case MDIO_DATA:
        s->data = value;        /* c45 write payload (PHY writes are no-ops) */
        break;
    case MDIO_ADDR:
        s->addr = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps emdio_bar0_ops = {
    .read = emdio_read,
    .write = emdio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void emdio_realize(PCIDevice *pci_dev, Error **errp)
{
    IMX95NetcEmdio *s = IMX95_NETC_EMDIO(pci_dev);
    uint8_t *cfg = pci_dev->config;

    pci_set_word(cfg + PCI_VENDOR_ID, EMDIO_VENDOR_ID);
    pci_set_word(cfg + PCI_DEVICE_ID, EMDIO_DEVICE_ID);
    pci_set_word(cfg + PCI_CLASS_DEVICE, PCI_CLASS_SYSTEM_OTHER);
    pci_config_set_prog_interface(cfg, 0x01);
    pci_config_set_interrupt_pin(cfg, 0);

    memory_region_init_io(&s->bar0, OBJECT(s), &emdio_bar0_ops, s,
                          "emdio-bar0", EMDIO_BAR0_SIZE);
    pci_register_bar(pci_dev, 0,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64, &s->bar0);
}

static const VMStateDescription vmstate_emdio = {
    .name = TYPE_IMX95_NETC_EMDIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, IMX95NetcEmdio),
        VMSTATE_UINT32(cfg, IMX95NetcEmdio),
        VMSTATE_UINT32(ctl, IMX95NetcEmdio),
        VMSTATE_UINT32(data, IMX95NetcEmdio),
        VMSTATE_UINT32(addr, IMX95NetcEmdio),
        VMSTATE_END_OF_LIST()
    },
};

static void emdio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = emdio_realize;
    k->vendor_id = EMDIO_VENDOR_ID;
    k->device_id = EMDIO_DEVICE_ID;
    /*
     * Real i.MX 95 silicon presents EMDIO as a system peripheral (class 0880,
     * prog-if 01) at revision 4, not as an Ethernet controller. The kernel
     * binds enetc_pci_mdio by vendor/device ID, so this is identification
     * only, but lspci in the guest should match the board.
     */
    k->class_id = PCI_CLASS_SYSTEM_OTHER;
    k->revision = 4;
    dc->desc = "i.MX 95 NETC EMDIO (MDIO + Aquantia c45 PHY)";
    dc->vmsd = &vmstate_emdio;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo emdio_info = {
    .name          = TYPE_IMX95_NETC_EMDIO,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(IMX95NetcEmdio),
    .class_init    = emdio_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void emdio_register_types(void)
{
    type_register_static(&emdio_info);
}

type_init(emdio_register_types)
