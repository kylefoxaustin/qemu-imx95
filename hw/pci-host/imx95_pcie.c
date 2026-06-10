/*
 * NXP i.MX 95 PCIe Root Complex (DesignWare, unrolled iATU)
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A from-scratch model of the imx95 general-purpose PCIe RC. It reuses the
 * DesignWare root-port / outbound-region machinery (cf. designware.c) with the
 * additions the imx95 bring-up needs:
 *
 *   - link bring-up runs through the "app" SERDES/GPR glue (@0x4c340000): the
 *     driver waits for the PHY MPLL to lock, then sets LTSSM_EN; the model
 *     reports the MPLL locked and gates the dbi link-up bit on LTSSM_EN;
 *   - the iATU is driven through the legacy VIEWPORT registers in dbi (the
 *     imx95 core also has an unrolled iATU region @0x4c360000, but
 *     dw_pcie_iatu_detect() settles on the viewport interface against this
 *     model; the unrolled region is also accepted for completeness);
 *   - the config window is at a fixed CPU address (0x60100000), reached through
 *     an outbound CFG iATU region, and the LIMIT register is widened to 64 bits
 *     so the >4 GiB MEM window works.
 *
 * MSI is delivered through the GICv3 ITS (the dtb msi-map targets &its), as for
 * the NETC host - the DWC internal MSI controller is not used.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/bitops.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pcie_port.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/core/irq.h"
#include "hw/pci-host/imx95_pcie.h"

/* DWC core registers within the dbi/root-port config space. */
#define PCIE_PORT_LINK_CONTROL          0x710
#define PCIE_PORT_DEBUG1                0x72c
#define PCIE_PORT_DEBUG1_LINK_UP        BIT(4)
#define PCIE_PORT_DEBUG1_LINK_IN_TRAIN  BIT(29)
#define PCIE_LINK_WIDTH_SPEED_CONTROL   0x80c
#define PCIE_PORT_LOGIC_SPEED_CHANGE    BIT(17)

/*
 * Legacy VIEWPORT iATU registers (in dbi). Although the imx95 core has an
 * unrolled iATU region, dw_pcie_iatu_detect() settles on the viewport interface
 * against this model, so the driver programs the iATU through these dbi
 * offsets; we honour them (the unrolled region below is also accepted).
 */
#define PCIE_ATU_VIEWPORT               0x900
#define PCIE_ATU_REGION_INBOUND         BIT(31)
#define PCIE_ATU_CR1                    0x904
#define PCIE_ATU_CR2                    0x908
#define PCIE_ATU_LOWER_BASE             0x90c
#define PCIE_ATU_UPPER_BASE             0x910
#define PCIE_ATU_LIMIT                  0x914
#define PCIE_ATU_LOWER_TARGET           0x918
#define PCIE_ATU_UPPER_TARGET           0x91c

/* "app" SERDES/GPR glue registers (driver's iomuxc_gpr mmio regmap). */
#define APP_PHY_MPLLA_CTRL              0x10
#define APP_PHY_MPLL_STATE              BIT(30)     /* PLL locked (read-only) */
#define APP_PE0_GEN_CTRL_3              0x1058
#define APP_PCIE_LTSSM_EN               BIT(0)
/*
 * Requester-ID -> stream-ID LUT (used for MSI/SMMU). The driver adds one entry
 * per enumerated requester-ID: it arms an index by writing ACSCTRL (low 5 bits
 * = index, RWA bit just selects read mode), reads DATA1 and skips the entry if
 * VLD (BIT31) is set, else fills DATA1/DATA2 and commits with ACSCTRL=index.
 * A flat register window makes every index read back the last DATA1 written
 * (VLD set), so the driver sees the whole 32-entry table as used after the
 * first device and bails ("All lut already used") - leaving every endpoint
 * without a stream-ID, so no MSI and no driver binds. Model it as an indexed
 * table: ACSCTRL latches the index, DATA1/DATA2 read/write that entry.
 */
#define APP_PE0_LUT_ACSCTRL             0x1008
#define APP_PE0_LUT_ENLOC              0x1f        /* index, bits [4:0] */
#define APP_PE0_LUT_DATA1               0x100c
#define APP_PE0_LUT_DATA2               0x1010

/* Unrolled iATU region register offsets (within a (dir,index) sub-block). */
#define ATU_UNR_REGION_CTRL1            0x00
#define ATU_UNR_REGION_CTRL2            0x04
#define ATU_UNR_LOWER_BASE             0x08
#define ATU_UNR_UPPER_BASE             0x0c
#define ATU_UNR_LOWER_LIMIT            0x10
#define ATU_UNR_LOWER_TARGET          0x14
#define ATU_UNR_UPPER_TARGET          0x18
#define ATU_UNR_UPPER_LIMIT           0x20
#define ATU_ENABLE                     BIT(31)
#define ATU_TYPE_MEM                   0x0
#define ATU_TYPE_IO                    0x2
#define ATU_TYPE_CFG0                  0x4
#define ATU_TYPE_CFG1                  0x5

#define ATU_BUS(x)                     (((x) >> 24) & 0xff)
#define ATU_DEVFN(x)                   (((x) >> 16) & 0xff)

#define VIEWPORT_OUTBOUND  0
#define VIEWPORT_INBOUND   1

static IMX95PCIEHost *imx95_pcie_root_to_host(IMX95PCIERoot *root)
{
    BusState *bus = qdev_get_parent_bus(DEVICE(root));
    return IMX95_PCIE_HOST(bus->parent);
}

static void imx95_pcie_update_viewport(IMX95PCIERoot *root,
                                       IMX95PCIEViewport *viewport);

static IMX95PCIEViewport *
imx95_pcie_current_viewport(IMX95PCIERoot *root)
{
    unsigned idx = root->atu_viewport & (IMX95_PCIE_NUM_VIEWPORTS - 1);
    unsigned dir = !!(root->atu_viewport & PCIE_ATU_REGION_INBOUND);

    return &root->viewports[dir][idx];
}

/* ---- root-port (dbi) config space ------------------------------------- */

static uint32_t imx95_pcie_root_config_read(PCIDevice *d, uint32_t address,
                                            int len)
{
    IMX95PCIERoot *root = IMX95_PCIE_ROOT(d);
    IMX95PCIEHost *host = imx95_pcie_root_to_host(root);
    IMX95PCIEViewport *vp = imx95_pcie_current_viewport(root);

    switch (address) {
    case PCIE_PORT_LINK_CONTROL:
        return 0xdeadbeef;          /* lane config; value ignored by Linux */
    case PCIE_LINK_WIDTH_SPEED_CONTROL:
        return PCIE_PORT_LOGIC_SPEED_CHANGE;   /* never block speed-change */
    case PCIE_PORT_DEBUG1:
        /* Link is up once the driver has enabled the LTSSM. */
        return host->ltssm_en ? PCIE_PORT_DEBUG1_LINK_UP : 0;
    case PCIE_ATU_VIEWPORT:
        return root->atu_viewport;
    case PCIE_ATU_CR1:
        return vp->cr[0];
    case PCIE_ATU_CR2:
        return vp->cr[1];
    case PCIE_ATU_LOWER_BASE:
        return (uint32_t)vp->base;
    case PCIE_ATU_UPPER_BASE:
        return (uint32_t)(vp->base >> 32);
    case PCIE_ATU_LIMIT:
        return (uint32_t)vp->limit;
    case PCIE_ATU_LOWER_TARGET:
        return (uint32_t)vp->target;
    case PCIE_ATU_UPPER_TARGET:
        return (uint32_t)(vp->target >> 32);
    default:
        return pci_default_read_config(d, address, len);
    }
}

static void imx95_pcie_root_config_write(PCIDevice *d, uint32_t address,
                                         uint32_t val, int len)
{
    IMX95PCIERoot *root = IMX95_PCIE_ROOT(d);
    IMX95PCIEViewport *vp = imx95_pcie_current_viewport(root);

    switch (address) {
    case PCIE_PORT_LINK_CONTROL:
    case PCIE_LINK_WIDTH_SPEED_CONTROL:
    case PCIE_PORT_DEBUG1:
        break;                      /* no-op */
    case PCIE_ATU_VIEWPORT:
        root->atu_viewport = val;
        break;
    case PCIE_ATU_CR1:
        vp->cr[0] = val;
        break;
    case PCIE_ATU_CR2:
        vp->cr[1] = val;
        if (!vp->inbound) {
            imx95_pcie_update_viewport(root, vp);
        }
        break;
    case PCIE_ATU_LOWER_BASE:
        vp->base = deposit64(vp->base, 0, 32, val);
        break;
    case PCIE_ATU_UPPER_BASE:
        vp->base = deposit64(vp->base, 32, 32, val);
        break;
    case PCIE_ATU_LIMIT:
        /*
         * The viewport LIMIT register is only 32 bits, but the imx95 MEM
         * window lives above 4 GiB. A window never crosses a 4 GiB boundary,
         * so reconstruct the 64-bit limit from the base's upper bits.
         */
        vp->limit = (vp->base & ~0xffffffffULL) | (val & 0xffffffff);
        break;
    case PCIE_ATU_LOWER_TARGET:
        vp->target = deposit64(vp->target, 0, 32, val);
        break;
    case PCIE_ATU_UPPER_TARGET:
        vp->target = deposit64(vp->target, 32, 32, val);
        break;
    default:
        pci_bridge_write_config(d, address, val, len);
        /*
         * The DWC RC root port always bridges downstream: it decodes/forwards
         * memory + I/O (so the CPU reaches the endpoint BARs) and forwards
         * upstream bus-master DMA (so the endpoint can reach guest memory + the
         * MSI doorbell). Linux only sets the root-port COMMAND when its port
         * driver enables it, which may not happen here - leaving MEM/IO/MASTER
         * clear, so the bridge neither forwards the BARs (CPU faults on BAR
         * MMIO) nor the device DMA (the virtqueue stalls). Keep all three
         * asserted and re-run the bridge window update so the forwarding
         * apertures are always present.
         */
        if (ranges_overlap(address, len, PCI_COMMAND, 2)) {
            uint16_t cmd = pci_get_word(d->config + PCI_COMMAND);
            uint16_t want = cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_IO |
                            PCI_COMMAND_MASTER;

            if (cmd != want) {
                pci_set_word(d->config + PCI_COMMAND, want);
                pci_bridge_update_mappings(PCI_BRIDGE(d));
            }
        }
        break;
    }
}

/* ---- downstream config access (config window via CFG iATU) ------------ */

static uint64_t imx95_pcie_cfg_access(void *opaque, hwaddr addr,
                                      uint64_t *val, unsigned len)
{
    IMX95PCIEViewport *viewport = opaque;
    IMX95PCIERoot *root = viewport->root;
    const uint8_t busnum = ATU_BUS(viewport->target);
    const uint8_t devfn  = ATU_DEVFN(viewport->target);
    PCIBus *pcibus = pci_get_bus(PCI_DEVICE(root));
    PCIDevice *pcidev = pci_find_device(pcibus, busnum, devfn);

    if (pcidev) {
        addr &= pci_config_size(pcidev) - 1;
        if (val) {
            pci_host_config_write_common(pcidev, addr,
                                         pci_config_size(pcidev), *val, len);
        } else {
            return pci_host_config_read_common(pcidev, addr,
                                               pci_config_size(pcidev), len);
        }
    }
    return UINT64_MAX;
}

static uint64_t imx95_pcie_cfg_read(void *opaque, hwaddr addr, unsigned len)
{
    return imx95_pcie_cfg_access(opaque, addr, NULL, len);
}

static void imx95_pcie_cfg_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned len)
{
    imx95_pcie_cfg_access(opaque, addr, &val, len);
}

static const MemoryRegionOps imx95_pcie_cfg_ops = {
    .read = imx95_pcie_cfg_read,
    .write = imx95_pcie_cfg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---- iATU outbound region -> CPU memory alias / config window --------- */

static void imx95_pcie_update_viewport(IMX95PCIERoot *root,
                                       IMX95PCIEViewport *viewport)
{
    const uint64_t base = viewport->base;
    const uint64_t size = viewport->limit - base + 1;
    const bool enabled  = viewport->cr[1] & ATU_ENABLE;
    const uint32_t type = viewport->cr[0] & 0x1f;
    MemoryRegion *current, *other;

    if (type == ATU_TYPE_CFG0 || type == ATU_TYPE_CFG1) {
        current = &viewport->cfg;
        other   = &viewport->mem;
    } else {
        current = &viewport->mem;
        other   = &viewport->cfg;
        memory_region_set_alias_offset(current, viewport->target);
    }

    memory_region_set_enabled(other, false);
    if (enabled) {
        memory_region_set_size(current, size);
        memory_region_set_address(current, base);
    }
    memory_region_set_enabled(current, enabled);
}

static uint64_t imx95_pcie_atu_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX95PCIEHost *host = opaque;
    unsigned dir = (offset >> 8) & 1;
    unsigned idx = (offset >> 9) & (IMX95_PCIE_NUM_VIEWPORTS - 1);
    IMX95PCIEViewport *vp = &host->root.viewports[dir][idx];

    switch (offset & 0xff) {
    case ATU_UNR_REGION_CTRL1:
        return vp->cr[0];
    case ATU_UNR_REGION_CTRL2:
        return vp->cr[1];
    case ATU_UNR_LOWER_BASE:
        return (uint32_t)vp->base;
    case ATU_UNR_UPPER_BASE:
        return (uint32_t)(vp->base >> 32);
    case ATU_UNR_LOWER_LIMIT:
        return (uint32_t)vp->limit;
    case ATU_UNR_UPPER_LIMIT:
        return (uint32_t)(vp->limit >> 32);
    case ATU_UNR_LOWER_TARGET:
        return (uint32_t)vp->target;
    case ATU_UNR_UPPER_TARGET:
        return (uint32_t)(vp->target >> 32);
    default:
        return 0;
    }
}

static void imx95_pcie_atu_write(void *opaque, hwaddr offset, uint64_t val,
                                 unsigned size)
{
    IMX95PCIEHost *host = opaque;
    unsigned dir = (offset >> 8) & 1;
    unsigned idx = (offset >> 9) & (IMX95_PCIE_NUM_VIEWPORTS - 1);
    IMX95PCIEViewport *vp = &host->root.viewports[dir][idx];

    switch (offset & 0xff) {
    case ATU_UNR_REGION_CTRL1:
        vp->cr[0] = val;
        break;
    case ATU_UNR_REGION_CTRL2:
        vp->cr[1] = val;
        if (dir == VIEWPORT_OUTBOUND) {
            imx95_pcie_update_viewport(&host->root, vp);
        }
        break;
    case ATU_UNR_LOWER_BASE:
        vp->base = deposit64(vp->base, 0, 32, val);
        break;
    case ATU_UNR_UPPER_BASE:
        vp->base = deposit64(vp->base, 32, 32, val);
        break;
    case ATU_UNR_LOWER_LIMIT:
        vp->limit = deposit64(vp->limit, 0, 32, val);
        break;
    case ATU_UNR_UPPER_LIMIT:
        vp->limit = deposit64(vp->limit, 32, 32, val);
        break;
    case ATU_UNR_LOWER_TARGET:
        vp->target = deposit64(vp->target, 0, 32, val);
        break;
    case ATU_UNR_UPPER_TARGET:
        vp->target = deposit64(vp->target, 32, 32, val);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps imx95_pcie_atu_ops = {
    .read = imx95_pcie_atu_read,
    .write = imx95_pcie_atu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---- dbi MMIO: routes to the root-port config space ------------------- */

static uint64_t imx95_pcie_dbi_read(void *opaque, hwaddr addr, unsigned size)
{
    PCIHostState *pci = PCI_HOST_BRIDGE(opaque);
    PCIDevice *dev = pci_find_device(pci->bus, 0, 0);

    if (addr >= pci_config_size(dev)) {
        return 0;       /* DWC regs the model doesn't implement -> 0 */
    }
    return pci_host_config_read_common(dev, addr, pci_config_size(dev), size);
}

static void imx95_pcie_dbi_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    PCIHostState *pci = PCI_HOST_BRIDGE(opaque);
    PCIDevice *dev = pci_find_device(pci->bus, 0, 0);

    if (addr >= pci_config_size(dev)) {
        return;
    }
    pci_host_config_write_common(dev, addr, pci_config_size(dev), val, size);
}

static const MemoryRegionOps imx95_pcie_dbi_ops = {
    .read = imx95_pcie_dbi_read,
    .write = imx95_pcie_dbi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---- "app" SERDES/GPR glue ------------------------------------------- */

static uint64_t imx95_pcie_glue_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX95PCIEHost *host = opaque;
    uint32_t idx = offset >> 2;
    uint32_t val = idx < IMX95_PCIE_GLUE_REGS ? host->glue_regs[idx] : 0;

    if (offset == APP_PHY_MPLLA_CTRL) {
        val |= APP_PHY_MPLL_STATE;      /* PHY PLL is always locked here */
    }
    /* LUT data registers read the currently-latched entry. */
    if (offset == APP_PE0_LUT_DATA1) {
        return host->lut_data1[host->lut_index];
    }
    if (offset == APP_PE0_LUT_DATA2) {
        return host->lut_data2[host->lut_index];
    }
    return val;
}

static void imx95_pcie_glue_write(void *opaque, hwaddr offset, uint64_t val,
                                  unsigned size)
{
    IMX95PCIEHost *host = opaque;
    uint32_t idx = offset >> 2;

    if (idx < IMX95_PCIE_GLUE_REGS) {
        host->glue_regs[idx] = val;
    }
    if (offset == APP_PE0_GEN_CTRL_3) {
        host->ltssm_en = !!(val & APP_PCIE_LTSSM_EN);
    }
    /* LUT: ACSCTRL latches the index; DATA1/DATA2 fill the latched entry. */
    if (offset == APP_PE0_LUT_ACSCTRL) {
        host->lut_index = val & APP_PE0_LUT_ENLOC;
    } else if (offset == APP_PE0_LUT_DATA1) {
        host->lut_data1[host->lut_index] = val;
    } else if (offset == APP_PE0_LUT_DATA2) {
        host->lut_data2[host->lut_index] = val;
    }
}

static const MemoryRegionOps imx95_pcie_glue_ops = {
    .read = imx95_pcie_glue_read,
    .write = imx95_pcie_glue_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---- realize ---------------------------------------------------------- */

static void imx95_pcie_root_realize(PCIDevice *dev, Error **errp)
{
    IMX95PCIERoot *root = IMX95_PCIE_ROOT(dev);
    IMX95PCIEHost *host = imx95_pcie_root_to_host(root);
    MemoryRegion *host_mem = get_system_memory();
    PCIBridge *br = PCI_BRIDGE(dev);
    const hwaddr dummy_offset = 0;
    const uint64_t dummy_size = 4;
    size_t i;

    br->bus_name = "imx95-pcie";
    pci_set_word(dev->config + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    pci_config_set_interrupt_pin(dev->config, 1);
    pci_bridge_initfn(dev, TYPE_PCIE_BUS);
    pcie_port_init_reg(dev);
    pcie_cap_init(dev, 0x70, PCI_EXP_TYPE_ROOT_PORT, 0, &error_fatal);

    for (i = 0; i < IMX95_PCIE_NUM_VIEWPORTS; i++) {
        IMX95PCIEViewport *vp;
        MemoryRegion *mem;
        char *name;

        /* Inbound: PCI -> CPU memory (default pass-through set below). */
        vp = &root->viewports[VIEWPORT_INBOUND][i];
        vp->root = root;
        vp->inbound = true;
        vp->limit = UINT32_MAX;
        mem = &vp->mem;
        name = g_strdup_printf("imx95-pcie inbound %zu", i);
        memory_region_init_alias(mem, OBJECT(root), name, host_mem,
                                 dummy_offset, dummy_size);
        /*
         * Higher priority than the PCI memory window (added at the default
         * priority 0 in host_realize): the device's DMA must reach guest RAM
         * and the ITS MSI doorbell through this inbound (PCI -> CPU) window, so
         * it has to win over the full-range PCI-mem region that would otherwise
         * absorb (and drop) every upstream access.
         */
        memory_region_add_subregion_overlap(&host->pci.address_space_root,
                                             dummy_offset, mem, 1);
        memory_region_set_enabled(mem, false);
        g_free(name);

        /* Outbound MEM: CPU -> PCI memory. */
        vp = &root->viewports[VIEWPORT_OUTBOUND][i];
        vp->root = root;
        vp->limit = UINT32_MAX;
        mem = &vp->mem;
        name = g_strdup_printf("imx95-pcie outbound mem %zu", i);
        memory_region_init_alias(mem, OBJECT(root), name, &host->pci.memory,
                                 dummy_offset, dummy_size);
        memory_region_add_subregion(host_mem, dummy_offset, mem);
        memory_region_set_enabled(mem, false);
        g_free(name);

        /* Outbound CFG: CPU -> downstream config. */
        mem = &vp->cfg;
        name = g_strdup_printf("imx95-pcie outbound cfg %zu", i);
        memory_region_init_io(mem, OBJECT(root), &imx95_pcie_cfg_ops, vp,
                              name, dummy_size);
        memory_region_add_subregion(host_mem, dummy_offset, mem);
        memory_region_set_enabled(mem, false);
        g_free(name);
    }

    /* HW default: with no inbound window programmed, inbound TLPs pass in. */
    root->viewports[VIEWPORT_INBOUND][0].cr[1] = ATU_ENABLE;
    imx95_pcie_update_viewport(root, &root->viewports[VIEWPORT_INBOUND][0]);
}

static void imx95_pcie_set_irq(void *opaque, int irq_num, int level)
{
    IMX95PCIEHost *host = IMX95_PCIE_HOST(opaque);

    qemu_set_irq(host->pci.irqs[irq_num], level);
}

static const char *imx95_pcie_root_bus_path(PCIHostState *host_bridge,
                                            PCIBus *rootbus)
{
    return "0000:00";
}

/*
 * Route every downstream device's DMA through the RC's own address space (which
 * carries the inbound iATU windows -> guest memory + the MSI doorbell), like
 * designware.c. Without this the device DMAs into the bare PCI-memory window
 * and never reaches RAM, so the virtqueue stalls.
 */
static AddressSpace *imx95_pcie_dma_iommu(PCIBus *bus, void *opaque, int devfn)
{
    IMX95PCIEHost *host = opaque;

    return &host->pci.address_space;
}

static const PCIIOMMUOps imx95_pcie_iommu_ops = {
    .get_address_space = imx95_pcie_dma_iommu,
};

static void imx95_pcie_host_realize(DeviceState *dev, Error **errp)
{
    PCIHostState *pci = PCI_HOST_BRIDGE(dev);
    IMX95PCIEHost *s = IMX95_PCIE_HOST(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    size_t i;

    for (i = 0; i < ARRAY_SIZE(s->pci.irqs); i++) {
        sysbus_init_irq(sbd, &s->pci.irqs[i]);
    }

    /* mmio 0: dbi (RP config), 1: unrolled iATU, 2: "app" glue. */
    memory_region_init_io(&s->dbi, OBJECT(s), &imx95_pcie_dbi_ops, s,
                          "imx95-pcie-dbi", 0x10000);
    sysbus_init_mmio(sbd, &s->dbi);
    memory_region_init_io(&s->atu, OBJECT(s), &imx95_pcie_atu_ops, s,
                          "imx95-pcie-atu", 0x10000);
    sysbus_init_mmio(sbd, &s->atu);
    memory_region_init_io(&s->glue, OBJECT(s), &imx95_pcie_glue_ops, s,
                          "imx95-pcie-app", 0x4000);
    sysbus_init_mmio(sbd, &s->glue);

    memory_region_init(&s->pci.io, OBJECT(s), "imx95-pcie-io", 64 * 1024);
    memory_region_init(&s->pci.memory, OBJECT(s), "imx95-pcie-mem",
                       UINT64_MAX);

    pci->bus = pci_register_root_bus(dev, "pcie", imx95_pcie_set_irq,
                                     pci_swizzle_map_irq_fn, s,
                                     &s->pci.memory, &s->pci.io,
                                     0, 4, TYPE_IMX95_PCIE_ROOT_BUS);
    pci->bus->flags |= PCI_BUS_EXTENDED_CONFIG_SPACE;

    memory_region_init(&s->pci.address_space_root, OBJECT(s),
                       "imx95-pcie-dma-root", UINT64_MAX);
    memory_region_add_subregion(&s->pci.address_space_root, 0x0,
                                &s->pci.memory);
    address_space_init(&s->pci.address_space, &s->pci.address_space_root,
                       "imx95-pcie-dma");
    pci_setup_iommu(pci->bus, &imx95_pcie_iommu_ops, s);

    qdev_realize(DEVICE(&s->root), BUS(pci->bus), &error_fatal);
}

static void imx95_pcie_host_init(Object *obj)
{
    IMX95PCIEHost *s = IMX95_PCIE_HOST(obj);

    object_initialize_child(obj, "root", &s->root, TYPE_IMX95_PCIE_ROOT);
    qdev_prop_set_int32(DEVICE(&s->root), "addr", PCI_DEVFN(0, 0));
    qdev_prop_set_bit(DEVICE(&s->root), "multifunction", false);
}

static void imx95_pcie_root_bus_class_init(ObjectClass *klass, const void *data)
{
    BusClass *k = BUS_CLASS(klass);

    k->max_dev = 1;     /* single root complex */
}

static void imx95_pcie_root_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->vendor_id = PCI_VENDOR_ID_SYNOPSYS;
    k->device_id = 0xabcd;
    k->class_id = PCI_CLASS_BRIDGE_PCI;
    k->exit = pci_bridge_exitfn;
    k->realize = imx95_pcie_root_realize;
    k->config_read = imx95_pcie_root_config_read;
    k->config_write = imx95_pcie_root_config_write;
    device_class_set_legacy_reset(dc, pci_bridge_reset);
    dc->user_creatable = false;
}

static void imx95_pcie_host_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(klass);

    hc->root_bus_path = imx95_pcie_root_bus_path;
    dc->realize = imx95_pcie_host_realize;
    dc->fw_name = "pci";
}

static const TypeInfo imx95_pcie_types[] = {
    {
        .name          = TYPE_IMX95_PCIE_ROOT_BUS,
        .parent        = TYPE_PCIE_BUS,
        .instance_size = sizeof(IMX95PCIERootBus),
        .class_init    = imx95_pcie_root_bus_class_init,
    }, {
        .name          = TYPE_IMX95_PCIE_HOST,
        .parent        = TYPE_PCI_HOST_BRIDGE,
        .instance_size = sizeof(IMX95PCIEHost),
        .instance_init = imx95_pcie_host_init,
        .class_init    = imx95_pcie_host_class_init,
    }, {
        .name          = TYPE_IMX95_PCIE_ROOT,
        .parent        = TYPE_PCI_BRIDGE,
        .instance_size = sizeof(IMX95PCIERoot),
        .class_init    = imx95_pcie_root_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_PCIE_DEVICE },
            { }
        },
    },
};

DEFINE_TYPES(imx95_pcie_types)
