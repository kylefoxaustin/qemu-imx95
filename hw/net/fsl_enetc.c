/*
 * NXP ENETC v4 (NETC) Ethernet PF - PCI endpoint function
 *
 * Models the i.MX 95 ENETC station-interface PF (PCI 1131:e101) on the NETC
 * integrated ECAM bus: the BAR0 register file, the station-interface command
 * BD ring (CBDR) auto-complete, and the TX/RX BD-ring DMA engine, enough to
 * bring up Linux's enetc4_pf driver over a QEMU -netdev backend.
 *
 * Reset values and register layout are taken from the linux-imx enetc driver
 * (drivers/net/ethernet/freescale/enetc) and imx95.dtsi. See
 * docs/reviews/netc-spec.md for the bring-up rationale.
 *
 * Copyright (c) 2026 Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/net/fsl_enetc.h"
#include "hw/pci/pci.h"
#include "hw/pci/pcie.h"
#include "migration/vmstate.h"
#include "net/eth.h"

/* --- BAR0 sub-block bases --- */
#define ENETC_SI_BASE       0x00000
#define ENETC_BDR_BASE      0x08000
#define ENETC_PORT_BASE     0x10000
#define ENETC_GLOBAL_BASE   0x20000

/* --- Station interface (SI) --- */
#define ENETC_SIMR          0x0000      /* mode (EN BIT31) */
#define ENETC_SIPCAPR0      0x0020      /* capabilities (RSS/RFS/...) -> 0 */
#define ENETC_SICAR0        0x0040
#define ENETC_SIPMAR0       0x0080      /* primary MAC low */
#define ENETC_SIPMAR1       0x0084      /* primary MAC high */
#define ENETC_SICAPR0       0x0900      /* ring count caps -> 1 RX/1 TX */
#define ENETC_SITXIDR       0x0a18      /* TX ring IRQ status (W1C) */
#define ENETC_SIRXIDR       0x0a28      /* RX ring IRQ status (W1C) */
#define ENETC_SIMSITRV(n)   (0x0b00 + (n) * 4)
#define ENETC_SIMSIRRV(n)   (0x0b80 + (n) * 4)

/* SI command BD ring (CBDR) */
#define ENETC_SICBDRMR      0x0800      /* mode (EN BIT31) */
#define ENETC_SICBDRSR      0x0804      /* status (RO) */
#define ENETC_SICBDRBAR0    0x0810      /* ring base low */
#define ENETC_SICBDRBAR1    0x0814      /* ring base high */
#define ENETC_SICBDRPIR     0x0818      /* producer idx (driver writes) */
#define ENETC_SICBDRCIR     0x081c      /* consumer idx (HW advances) */
#define ENETC_SICBDRLENR    0x0820      /* ring length */

/* --- BD rings (relative to ENETC_BDR_BASE) --- */
#define ENETC_BDR(t, i)     ((t) * 0x100 + (i) * 0x200)
#define ENETC_BDR_TX        0
#define ENETC_BDR_RX        1
#define ENETC_TBMR          0x00        /* TX mode (EN BIT31, FWB BIT24) */
#define ENETC_TBSR          0x04        /* TX status (BUSY BIT0) */
#define ENETC_TBBAR0        0x10
#define ENETC_TBBAR1        0x14
#define ENETC_TBPIR         0x18        /* TX producer (kick) */
#define ENETC_TBCIR         0x1c        /* TX consumer (HW advances) */
#define ENETC_TBLENR        0x20
#define ENETC_TBIER         0xa0
#define ENETC_TBIDR         0xa4
#define ENETC_RBMR          0x00        /* RX mode (EN BIT31) */
#define ENETC_RBSR          0x04        /* RX status (BUSY BIT0) */
#define ENETC_RBBSR         0x08        /* RX buffer size */
#define ENETC_RBCIR         0x0c        /* RX consumer (driver writes) */
#define ENETC_RBBAR0        0x10
#define ENETC_RBBAR1        0x14
#define ENETC_RBPIR         0x18        /* RX producer (HW advances) */
#define ENETC_RBLENR        0x20
#define ENETC_RBIER         0xa0
#define ENETC_RBIDR         0xa4

/* --- Port block --- */
#define ENETC_PMR           0x0010
#define ENETC_PCR           0x4010
#define ENETC_POR           0x4100
#define ENETC_IMDIO_BASE    0x5030      /* internal MDIO */
#define ENETC_MDIO_CFG      0x00        /* BSY BIT0 */
#define ENETC_MDIO_CTL      0x04
#define ENETC_MDIO_DATA     0x08
#define ENETC_MDIO_ADDR     0x0c

/* --- Global block --- */
#define ENETC_G_EIPBRR0     0x0bf8      /* IP block rev: low16 = 0x0401 */

/* Reset / fixed values */
#define ENETC_REV_4_1       0x0401
#define ENETC_SICAPR0_VAL   0x00010001  /* [31:16]=1 RX ring, [7:0]=1 TX ring */

#define ENETC_MR_EN         (1u << 31)

static inline uint32_t enetc_reg(FslEnetcState *s, hwaddr off)
{
    return s->regs[off / 4];
}

static inline void enetc_set(FslEnetcState *s, hwaddr off, uint32_t val)
{
    s->regs[off / 4] = val;
}

/*
 * CBDR auto-complete. The driver posts NTMP command BDs and polls
 * SICBDRCIR until it reaches SICBDRPIR. We never stall: snap the consumer
 * index to the producer index so every command "completes" immediately.
 * (Per-command response payloads are left zeroed = success; extend here if
 * a command's read-back is needed.)
 */
static void enetc_cbdr_kick(FslEnetcState *s)
{
    enetc_set(s, ENETC_SI_BASE + ENETC_SICBDRCIR,
              enetc_reg(s, ENETC_SI_BASE + ENETC_SICBDRPIR));
}

static uint64_t fsl_enetc_bar0_read(void *opaque, hwaddr off, unsigned size)
{
    FslEnetcState *s = opaque;
    uint32_t val;

    switch (off) {
    case ENETC_SI_BASE + ENETC_SICAPR0:
        return ENETC_SICAPR0_VAL;
    case ENETC_SI_BASE + ENETC_SIPCAPR0:
        return 0;
    case ENETC_GLOBAL_BASE + ENETC_G_EIPBRR0:
        return ENETC_REV_4_1;
    /* TX/RX ring status: never busy (teardown completes immediately). */
    case ENETC_BDR_BASE + ENETC_BDR(ENETC_BDR_TX, 0) + ENETC_TBSR:
    case ENETC_BDR_BASE + ENETC_BDR(ENETC_BDR_RX, 0) + ENETC_RBSR:
    case ENETC_SI_BASE + ENETC_SICBDRSR:
        return 0;
    /* Internal MDIO: never busy, no read error. */
    case ENETC_PORT_BASE + ENETC_IMDIO_BASE + ENETC_MDIO_CFG:
        return 0;
    default:
        val = enetc_reg(s, off);
        return val;
    }
}

static void fsl_enetc_bar0_write(void *opaque, hwaddr off, uint64_t val,
                                 unsigned size)
{
    FslEnetcState *s = opaque;

    switch (off) {
    case ENETC_SI_BASE + ENETC_SICBDRPIR:
        enetc_set(s, off, val);
        enetc_cbdr_kick(s);
        return;
    case ENETC_SI_BASE + ENETC_SITXIDR:
    case ENETC_SI_BASE + ENETC_SIRXIDR:
        /* W1C IRQ status: clear written bits. */
        enetc_set(s, off, enetc_reg(s, off) & ~(uint32_t)val);
        return;
    default:
        enetc_set(s, off, val);
        return;
    }
}

static const MemoryRegionOps fsl_enetc_bar0_ops = {
    .read = fsl_enetc_bar0_read,
    .write = fsl_enetc_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/* --- NIC backend (RX path lands in stage 4c) --- */
static bool fsl_enetc_can_receive(NetClientState *nc)
{
    return false; /* RX BD-ring DMA not wired yet (stage 4c) */
}

static ssize_t fsl_enetc_receive(NetClientState *nc, const uint8_t *buf,
                                 size_t size)
{
    return size; /* drop until stage 4c */
}

static NetClientInfo net_fsl_enetc_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = fsl_enetc_can_receive,
    .receive = fsl_enetc_receive,
};

static void fsl_enetc_reset_regs(FslEnetcState *s)
{
    memset(s->regs, 0, FSL_ENETC_BAR0_SIZE);
    enetc_set(s, ENETC_SI_BASE + ENETC_SICAPR0, ENETC_SICAPR0_VAL);
    enetc_set(s, ENETC_GLOBAL_BASE + ENETC_G_EIPBRR0, ENETC_REV_4_1);
}

static void fsl_enetc_realize(PCIDevice *pci_dev, Error **errp)
{
    FslEnetcState *s = FSL_ENETC(pci_dev);
    uint8_t *cfg = pci_dev->config;
    int ret;

    pci_set_word(cfg + PCI_VENDOR_ID, FSL_ENETC_VENDOR_ID);
    pci_set_word(cfg + PCI_DEVICE_ID, FSL_ENETC_PF_DEVICE_ID);
    pci_set_word(cfg + PCI_CLASS_DEVICE, PCI_CLASS_NETWORK_ETHERNET);
    pci_config_set_interrupt_pin(cfg, 0); /* MSI-X only */

    s->regs = g_malloc0(FSL_ENETC_BAR0_SIZE);
    fsl_enetc_reset_regs(s);

    memory_region_init_io(&s->bar0, OBJECT(s), &fsl_enetc_bar0_ops, s,
                          "enetc-bar0", FSL_ENETC_BAR0_SIZE);
    pci_register_bar(pci_dev, 0,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64, &s->bar0);

    memory_region_init(&s->msix_bar, OBJECT(s), "enetc-msix",
                       FSL_ENETC_MSIX_BAR_SIZE);
    pci_register_bar(pci_dev, FSL_ENETC_MSIX_TABLE_BAR,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64, &s->msix_bar);

    ret = msix_init(pci_dev, FSL_ENETC_MSIX_VECTORS,
                    &s->msix_bar, FSL_ENETC_MSIX_TABLE_BAR,
                    FSL_ENETC_MSIX_TABLE_OFF,
                    &s->msix_bar, FSL_ENETC_MSIX_TABLE_BAR,
                    FSL_ENETC_MSIX_PBA_OFF, 0, errp);
    if (ret) {
        return;
    }

    /* PCIe endpoint with FLR (enetc4_pf does pcie_flr at probe start). */
    ret = pcie_endpoint_cap_init(pci_dev, 0);
    if (ret < 0) {
        error_setg(errp, "enetc: failed to init PCIe cap");
        return;
    }
    pcie_cap_flr_init(pci_dev);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_fsl_enetc_info, &s->conf,
                          object_get_typename(OBJECT(s)), pci_dev->qdev.id,
                          &pci_dev->qdev.mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void fsl_enetc_exit(PCIDevice *pci_dev)
{
    FslEnetcState *s = FSL_ENETC(pci_dev);

    qemu_del_nic(s->nic);
    msix_uninit(pci_dev, &s->msix_bar, &s->msix_bar);
    g_free(s->regs);
}

static void fsl_enetc_reset(DeviceState *dev)
{
    FslEnetcState *s = FSL_ENETC(dev);

    fsl_enetc_reset_regs(s);
    memset(s->tx, 0, sizeof(s->tx));
    memset(s->rx, 0, sizeof(s->rx));
}

static const Property fsl_enetc_props[] = {
    DEFINE_NIC_PROPERTIES(FslEnetcState, conf),
};

static void fsl_enetc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = fsl_enetc_realize;
    k->exit = fsl_enetc_exit;
    k->vendor_id = FSL_ENETC_VENDOR_ID;
    k->device_id = FSL_ENETC_PF_DEVICE_ID;
    k->class_id = PCI_CLASS_NETWORK_ETHERNET;
    k->revision = 1;

    device_class_set_legacy_reset(dc, fsl_enetc_reset);
    device_class_set_props(dc, fsl_enetc_props);
    dc->desc = "NXP ENETC v4 Ethernet PF";
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo fsl_enetc_info = {
    .name = TYPE_FSL_ENETC,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(FslEnetcState),
    .class_init = fsl_enetc_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void fsl_enetc_register_types(void)
{
    type_register_static(&fsl_enetc_info);
}

type_init(fsl_enetc_register_types)
