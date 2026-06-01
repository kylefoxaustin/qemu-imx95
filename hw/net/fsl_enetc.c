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
#define ENETC_PSIPMAR0(a)   ((a) * 0x80 + 0x2000)  /* per-SI primary MAC lo */
#define ENETC_PSIPMAR1(a)   ((a) * 0x80 + 0x2004)  /* per-SI primary MAC hi */
#define ENETC_PCR           0x4010
#define ENETC_POR           0x4100
#define ENETC_IMDIO_BASE    0x5030      /* internal MDIO */
#define ENETC_MDIO_CFG      0x00        /* BSY BIT0 */
#define ENETC_MDIO_CTL      0x04
#define ENETC_MDIO_DATA     0x08
#define ENETC_MDIO_ADDR     0x0c

/* SI per-ring MSI-X vector tables (which MSI-X entry a ring fires). */
#define ENETC_SIMSITRV(n)   (0x0b00 + (n) * 4)  /* TX ring n -> vector */
#define ENETC_SIMSIRRV(n)   (0x0b80 + (n) * 4)  /* RX ring n -> vector */

/* TX/RX BD ring index-register masks (indices, not byte offsets). */
#define ENETC_TBCIR_IDX_MASK 0xffff
#define ENETC_RBCIR_IDX_MASK 0xffff

/* --- Global block --- */
#define ENETC_G_EIPBRR0     0x0bf8      /* IP block rev: low16 = 0x0401 */

/*
 * Buffer descriptors are 16 bytes. The TX BD the driver posts is
 *   addr[8] buf_len[2] frm_len[2] lstatus[4]   (flags in lstatus[31:24]).
 * The RX BD is posted as { addr[8], resv[8] } and written back by HW as
 *   { ..., buf_len@offset 12 (__le16), lstatus@offset 12 (__le32) }; the
 * driver polls lstatus != 0 and reads buf_len from the same word region.
 */
#define ENETC_BD_SIZE        16
#define ENETC_TXBD_FLAGS_F   (1u << 7)   /* final BD of a frame */
#define ENETC_TXBD_FLAGS_OFF 24          /* flags live in lstatus[31:24] */
#define ENETC_RXBD_LSTATUS_F (1u << 31)  /* RX writeback: final */
#define ENETC_RXBD_LSTATUS_R (1u << 30)  /* RX writeback: ready/ring-full */

/* Max Ethernet frame we gather/scatter (with VLAN + FCS headroom). */
#define ENETC_FRAME_MAX      2048

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

/* Raise the MSI-X vector a ring is routed to (SIMSITRV/SIMSIRRV entry). */
static void enetc_ring_irq(FslEnetcState *s, hwaddr msivec_reg)
{
    PCIDevice *pci = PCI_DEVICE(s);
    uint32_t vec = enetc_reg(s, ENETC_SI_BASE + msivec_reg);

    if (msix_enabled(pci)) {
        msix_notify(pci, vec);
    }
}

/* Base GPA of a BD ring from its BDxBAR0/BDxBAR1 register pair. */
static uint64_t enetc_ring_base(FslEnetcState *s, hwaddr bar0_off)
{
    return enetc_reg(s, bar0_off) |
           ((uint64_t)enetc_reg(s, bar0_off + 4) << 32);
}

/*
 * TX kick. The driver writes its producer index to TBPIR; walk the BD ring
 * from the consumer index (TBCIR) up to it, transmit each frame, then snap
 * TBCIR to TBPIR and raise the ring's TX interrupt. BDs are 16 bytes and the
 * ring length (in BDs) is TBLENR. Multi-BD frames are gathered until the F
 * (final) flag.
 */
static void enetc_tx_kick(FslEnetcState *s)
{
    hwaddr tbase = ENETC_BDR_BASE + ENETC_BDR(ENETC_BDR_TX, 0);
    uint64_t ring = enetc_ring_base(s, tbase + ENETC_TBBAR0);
    uint32_t len = enetc_reg(s, tbase + ENETC_TBLENR) & ENETC_TBCIR_IDX_MASK;
    uint32_t pi = enetc_reg(s, tbase + ENETC_TBPIR) & ENETC_TBCIR_IDX_MASK;
    uint32_t ci = enetc_reg(s, tbase + ENETC_TBCIR) & ENETC_TBCIR_IDX_MASK;
    PCIDevice *pci = PCI_DEVICE(s);
    uint8_t frame[ENETC_FRAME_MAX];
    uint32_t frame_len = 0;

    if (!len) {
        return;
    }

    while (ci != pi) {
        uint8_t bd[ENETC_BD_SIZE];
        uint64_t addr;
        uint16_t buf_len;
        uint32_t lstatus;

        pci_dma_read(pci, ring + (uint64_t)ci * ENETC_BD_SIZE, bd, sizeof(bd));
        addr = ldq_le_p(bd);
        buf_len = lduw_le_p(bd + 8);
        lstatus = ldl_le_p(bd + 12);

        if (frame_len + buf_len <= sizeof(frame)) {
            pci_dma_read(pci, addr, frame + frame_len, buf_len);
            frame_len += buf_len;
        }

        if (lstatus & (ENETC_TXBD_FLAGS_F << ENETC_TXBD_FLAGS_OFF)) {
            if (frame_len) {
                qemu_send_packet(qemu_get_queue(s->nic), frame, frame_len);
            }
            frame_len = 0;
        }

        if (++ci == len) {
            ci = 0;
        }
    }

    enetc_set(s, tbase + ENETC_TBCIR, ci);
    enetc_ring_irq(s, ENETC_SIMSITRV(0));
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
    case ENETC_BDR_BASE + ENETC_BDR(ENETC_BDR_TX, 0) + ENETC_TBPIR:
        enetc_set(s, off, val);
        enetc_tx_kick(s);
        return;
    case ENETC_BDR_BASE + ENETC_BDR(ENETC_BDR_RX, 0) + ENETC_RBCIR:
        /* Driver posted more empty RX buffers; try to flush pending RX. */
        enetc_set(s, off, val);
        if (s->nic && s->rx_pending) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
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
    /*
     * The driver uses sub-word MMIO (e.g. __raw_readw of PSIPMAR1 for the
     * MAC high half); reject nothing 1..4 bytes or QEMU raises an external
     * abort. impl stays word-wide so the handlers only ever see 32-bit
     * accesses (QEMU extracts/merges the narrow ones).
     */
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/* --- NIC backend --- */
static bool fsl_enetc_can_receive(NetClientState *nc)
{
    FslEnetcState *s = qemu_get_nic_opaque(nc);
    hwaddr rbase = ENETC_BDR_BASE + ENETC_BDR(ENETC_BDR_RX, 0);
    uint32_t len = enetc_reg(s, rbase + ENETC_RBLENR) & ENETC_RBCIR_IDX_MASK;
    uint32_t ci;

    /* The RX ring must be enabled and have at least one posted (empty) BD. */
    if (!len || !(enetc_reg(s, rbase + ENETC_RBMR) & ENETC_MR_EN)) {
        return false;
    }
    /* RBCIR (driver's next_to_use) is one past the last posted buffer. */
    ci = enetc_reg(s, rbase + ENETC_RBCIR) & ENETC_RBCIR_IDX_MASK;
    return s->rx_pi != ci;
}

static ssize_t fsl_enetc_receive(NetClientState *nc, const uint8_t *buf,
                                 size_t size)
{
    FslEnetcState *s = qemu_get_nic_opaque(nc);
    PCIDevice *pci = PCI_DEVICE(s);
    hwaddr rbase = ENETC_BDR_BASE + ENETC_BDR(ENETC_BDR_RX, 0);
    uint64_t ring = enetc_ring_base(s, rbase + ENETC_RBBAR0);
    uint32_t len = enetc_reg(s, rbase + ENETC_RBLENR) & ENETC_RBCIR_IDX_MASK;
    uint64_t bd_addr;
    uint8_t bd[ENETC_BD_SIZE];
    uint64_t buf_addr;

    if (!fsl_enetc_can_receive(nc)) {
        s->rx_pending = true;
        return 0; /* back-pressure: retried on the next RBCIR post */
    }
    if (size > ENETC_FRAME_MAX) {
        return size; /* drop oversized; reported as received */
    }

    /* Read the posted BD at the producer index, DMA the frame to its buffer. */
    bd_addr = ring + (uint64_t)s->rx_pi * ENETC_BD_SIZE;
    pci_dma_read(pci, bd_addr, bd, sizeof(bd));
    buf_addr = ldq_le_p(bd);
    pci_dma_write(pci, buf_addr, buf, size);

    /*
     * Write back the RX BD in the HW (union enetc_rx_bd.r) layout the driver
     * reads: buf_len is a __le16 at offset 8, and lstatus is a __le32 at
     * offset 12 (overlapping flags[15:0]/error[31:16]). The driver takes the
     * frame length from buf_len and treats lstatus != 0 as "BD ready", with
     * LSTATUS_F (bit 31) marking the final BD. Clear the rest of the BD so
     * stale csum/hash fields do not confuse offload parsing.
     */
    memset(bd, 0, sizeof(bd));
    stw_le_p(bd + 8, (uint16_t)size);                 /* r.buf_len */
    stl_le_p(bd + 12, ENETC_RXBD_LSTATUS_F);          /* final BD, no error */
    pci_dma_write(pci, bd_addr, bd, sizeof(bd));

    if (++s->rx_pi == len) {
        s->rx_pi = 0;
    }
    enetc_set(s, rbase + ENETC_RBPIR, s->rx_pi);
    s->rx_pending = false;
    enetc_ring_irq(s, ENETC_SIMSIRRV(0));

    return size;
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
    int ret, i;

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

    /*
     * Mark every MSI-X vector as in-use so msix_notify() delivers it. Like
     * other fully-emulated MSI-X NICs (e1000e, igb, vmxnet3) we do not use
     * vector-use notifiers; without this, msix_notify() early-returns on
     * !msix_entry_used and the ring interrupts never reach the guest.
     */
    for (i = 0; i < FSL_ENETC_MSIX_VECTORS; i++) {
        msix_vector_use(pci_dev, i);
    }

    /*
     * Modelled as a conventional PCI endpoint (the real ENETC PF is an RCiEP,
     * but enetc4_pf treats pcie_flr() at probe as best-effort, so the PCIe
     * capability is not required). A PCIe-cap init here would assert unless
     * the device is plugged into an express slot.
     */

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_fsl_enetc_info, &s->conf,
                          object_get_typename(OBJECT(s)), pci_dev->qdev.id,
                          &pci_dev->qdev.mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);

    /*
     * Seed the SI primary MAC the driver reads back at probe
     * (enetc4_pf_get_si_primary_mac reads PORT PSIPMAR0/1 for SI 0).
     * PSIPMAR0 = MAC[0..3] little-endian, PSIPMAR1 = MAC[4..5].
     */
    {
        const uint8_t *m = s->conf.macaddr.a;

        enetc_set(s, ENETC_PORT_BASE + ENETC_PSIPMAR0(0),
                  m[0] | m[1] << 8 | m[2] << 16 | (uint32_t)m[3] << 24);
        enetc_set(s, ENETC_PORT_BASE + ENETC_PSIPMAR1(0), m[4] | m[5] << 8);
    }
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
    s->rx_pi = 0;
    s->rx_pending = false;
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
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void fsl_enetc_register_types(void)
{
    type_register_static(&fsl_enetc_info);
}

type_init(fsl_enetc_register_types)
