/*
 * NXP i.MX 95 Arm SMMU-v3 — declared stub that announces itself
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The i.MX 95 integrates an Arm SMMU-v3 at iommu@490d0000.  It is NOT
 * modelled, and this window exists so that the guests which merely *disable*
 * it still get through:
 *
 *   - U-Boot SPL calls disable_smmuv3(), which polls GBPA.UPDATE clear, writes
 *     GBPA then CR0 = 0, and polls CR0_ACK == 0.  Reads-as-zero satisfies all
 *     three polls, so the SMMU is left disabled exactly as it is on silicon
 *     after SPL.
 *   - Linux's arm-smmu-v3 driver reads IDR0, finds neither S1P nor S2P, prints
 *     "no translation support!" and fails probe with -ENXIO.
 *
 * That second outcome is deliberate, but its consequence is easy to miss, so
 * this device says it out loud.  Because no IOMMU ever registers, every DMA
 * master the device tree places behind the SMMU falls back to direct DMA ops:
 * `iommus`/`iommu-map` are *optional* fw_devlink supplier bindings, so a
 * consumer is not blocked - it takes -EPROBE_DEFER from iommu_fwspec_init,
 * waits out driver_deferred_probe_timeout (10 s), warns "deferred probe
 * timeout, ignoring dependency", and binds identity-mapped.  On real silicon
 * the BSP runs those same masters in a *translated*, strict-mode domain
 * (iommu groups 0-12 on a FRDM-IMX95-PRO), where device addresses are IOVAs
 * and never equal physical addresses.
 *
 * So a whole class of DMA-addressing bug - handing hardware a CPU physical
 * address instead of the dma_addr_t from dma_map_*(), touching a buffer after
 * dma_unmap, an under-sized mapping, a new master with no `iommus` entry -
 * works here and faults on the real part.  A one-shot warning on the guest's
 * first IDR0 read is cheap and turns that silent gap into a visible one.
 *
 * The warning fires on IDR0 only, not on any access: SPL touches just
 * GBPA/CR0/CR0_ACK and must stay quiet, or every U-Boot boot would cry wolf.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX95_SMMU_STUB "imx95.smmu-stub"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95SMMUStubState, IMX95_SMMU_STUB)

/*
 * DT reg window for iommu@490d0000.  128 KiB covers both register pages the
 * Linux driver ioremaps (ARM_SMMU_REG_SZ at base, and again at +64 KiB).
 */
#define SMMU_STUB_MMIO_SIZE  (128 * KiB)

/* The only offset we need to recognise: Linux probes here, SPL never does. */
#define SMMU_IDR0            0x00

struct IMX95SMMUStubState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    bool announced;   /* host-side only; nothing guest-visible to migrate */
};

static uint64_t imx95_smmu_stub_read(void *opaque, hwaddr off, unsigned size)
{
    IMX95SMMUStubState *s = opaque;

    if (off == SMMU_IDR0 && !s->announced) {
        s->announced = true;
        warn_report("i.MX 95 SMMUv3 (iommu@490d0000) is not modelled: "
                    "the guest's arm-smmu-v3 probe will fail and every DMA "
                    "master behind the SMMU (USB2/USB3, Neutron NPU, uSDHC1-3, "
                    "eDMA3, PCIe and NETC endpoints) runs identity-mapped, "
                    "i.e. device address == physical address.");
        warn_report("Real i.MX 95 silicon translates those masters in a "
                    "strict-mode IOMMU domain, so DMA-addressing bugs - a "
                    "physical address used in place of a dma_addr_t, DMA after "
                    "dma_unmap, an under-sized mapping, a master with no "
                    "'iommus' entry - pass here and fault on hardware.");
    }

    /*
     * Reads-as-zero is load-bearing in both directions: IDR0 == 0 makes
     * Linux's probe fail deterministically (rather than half-bind an SMMU
     * whose stream table we would not honour), and CR0_ACK/GBPA == 0 lets
     * SPL's disable_smmuv3() poll loops complete.
     */
    return 0;
}

static void imx95_smmu_stub_write(void *opaque, hwaddr off, uint64_t val,
                                  unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: write 0x%" PRIx64 " to offset 0x%" HWADDR_PRIx
                  " ignored (SMMUv3 not modelled)\n",
                  TYPE_IMX95_SMMU_STUB, val, off);
}

static const MemoryRegionOps imx95_smmu_stub_ops = {
    .read = imx95_smmu_stub_read,
    .write = imx95_smmu_stub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx95_smmu_stub_realize(DeviceState *dev, Error **errp)
{
    IMX95SMMUStubState *s = IMX95_SMMU_STUB(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &imx95_smmu_stub_ops, s,
                          TYPE_IMX95_SMMU_STUB, SMMU_STUB_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void imx95_smmu_stub_reset(DeviceState *dev)
{
    IMX95SMMUStubState *s = IMX95_SMMU_STUB(dev);

    /* Announce again after a reboot - the next guest deserves the warning. */
    s->announced = false;
}

static void imx95_smmu_stub_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx95_smmu_stub_realize;
    device_class_set_legacy_reset(dc, imx95_smmu_stub_reset);
    dc->desc = "NXP i.MX 95 SMMU-v3 (declared stub, no translation)";
}

static const TypeInfo imx95_smmu_stub_info = {
    .name           = TYPE_IMX95_SMMU_STUB,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(IMX95SMMUStubState),
    .class_init     = imx95_smmu_stub_class_init,
};

static void imx95_smmu_stub_register_types(void)
{
    type_register_static(&imx95_smmu_stub_info);
}

type_init(imx95_smmu_stub_register_types)
