/*
 * NXP ENETC v4 (NETC) Ethernet PF - PCI endpoint function
 *
 * Models the i.MX 95 ENETC station-interface PF as it appears on the NETC
 * integrated ECAM bus (PCI 1131:e101). Enough of the BAR0 register file,
 * the command BD-ring (CBDR) auto-complete and the TX/RX BD-ring DMA engine
 * to bring up Linux's enetc4_pf driver to a working netdev over a QEMU
 * -netdev backend.
 *
 * Copyright (c) 2026 Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NET_FSL_ENETC_H
#define HW_NET_FSL_ENETC_H

#include "hw/pci/pci_device.h"
#include "hw/pci/msix.h"
#include "net/net.h"
#include "qom/object.h"

#define TYPE_FSL_ENETC "fsl-enetc"
OBJECT_DECLARE_SIMPLE_TYPE(FslEnetcState, FSL_ENETC)

/* PCI identity (NETC v4 PF on i.MX 95) */
#define FSL_ENETC_VENDOR_ID     0x1131  /* PCI_VENDOR_ID_NXP2 */
#define FSL_ENETC_PF_DEVICE_ID  0xe101

/* BAR0 is a 256 KiB register window split into sub-blocks. */
#define FSL_ENETC_BAR0_SIZE     0x40000

/* MSI-X: advertise enough vectors for 1 + bdr_int_num rings. */
#define FSL_ENETC_MSIX_VECTORS   32
#define FSL_ENETC_MSIX_TABLE_BAR 2
#define FSL_ENETC_MSIX_TABLE_OFF 0x0
#define FSL_ENETC_MSIX_PBA_OFF   0x1000
#define FSL_ENETC_MSIX_BAR_SIZE  0x2000

/* One TX and one RX ring (matches SICAPR0 = 0x00010001 advertised). */
#define FSL_ENETC_NUM_TX_RINGS  1
#define FSL_ENETC_NUM_RX_RINGS  1

typedef struct FslEnetcRing {
    uint64_t base;       /* ring base GPA (BAR0 BDxBAR0/1) */
    uint32_t len;        /* ring length in BDs (BDxLENR & ~7) */
    uint32_t pir;        /* producer index */
    uint32_t cir;        /* consumer index */
    uint32_t mr;         /* mode register (enable etc.) */
    uint32_t ier;        /* interrupt enable */
    uint32_t idr;        /* interrupt status (W1C) */
    uint32_t msix_vec;   /* SIMSITRV/SIMSIRRV routed vector */
    uint32_t bsr;        /* RX buffer size (RBBSR), RX only */
} FslEnetcRing;

struct FslEnetcState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    MemoryRegion bar0;
    MemoryRegion msix_bar;

    NICState *nic;
    NICConf conf;

    /*
     * Backing store for the whole BAR0 window (word granular). Plain
     * control/config registers store-and-replay here; read-only, reset
     * and side-effecting registers are special-cased in the I/O handlers.
     */
    uint32_t *regs;

    /* TX/RX BD-ring bookkeeping (indices/base mirror BAR0 ring regs). */
    FslEnetcRing tx[FSL_ENETC_NUM_TX_RINGS];
    FslEnetcRing rx[FSL_ENETC_NUM_RX_RINGS];
};

#endif /* HW_NET_FSL_ENETC_H */
