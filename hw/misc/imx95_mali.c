/*
 * NXP i.MX 95 Mali GPU — identify-tier model (Linux sees it, fails honestly)
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The i.MX 95 integrates an Arm Mali-G310 (Valhall, CSF) GPU at gpu@4d900000.
 * Linux's Arm "kbase" DDK (drivers/gpu/arm/midgard, CONFIG_MALI_MIDGARD) probes
 * the "arm,mali-valhall" node. The 3D compute is fixed-function Arm IP QEMU
 * cannot execute, and the userspace is Arm's proprietary libmali blob (not
 * Mesa), so a fully-bound GPU still could not render. Per the board-farm
 * standard the honest outcome is: Linux *sees and identifies* the GPU, the
 * driver attempts bring-up, and it fails CLEANLY - no hang, no silent
 * /dev/mali0 that cannot work.
 *
 * With the old reads-as-0 stub kbase read GPU_ID = 0, got arch 0, and rejected
 * the GPU ("Unknown GPU Product ID 0", -EINVAL) before identifying it. This
 * model reports the real Mali-G310 GPU_ID, so kbase identifies it ("GPU
 * identified as 0x4 arch 10.12.7"), reads its property registers, then fails
 * the CSF bring-up with -EIO ("Miscellaneous device initialization failed")
 * when the un-modelled reset/power/firmware handshake times out - a clean,
 * honest probe failure that still reaches userspace. Modelling the full CSF
 * reset/power/MCU-firmware handshake (to actually bind) is out of scope: the
 * proprietary userspace cannot render anyway.
 * See docs/validation/fidelity-audit.md.
 *
 * GPU_ID (new "ID2" format, uapi mali_kbase_gpu_id.h):
 *   arch_major[31:28] arch_minor[27:24] arch_rev[23:20] product_major[19:16]
 *   version_major[15:12] version_minor[11:4] version_status[3:0]
 * Mali-G310 = product model TVAX = MODEL_MAKE(arch_major=10, product_major=4)
 * (the G310/G510 share the TVAX product code, differing only in core count).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX95_MALI "imx95.mali"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95MaliState, IMX95_MALI)

/* DT reg window for gpu@4d900000 (the driver ioremaps the whole thing). */
#define MALI_MMIO_SIZE   0x480000

/* GPU control register offsets the kbase probe touches. */
#define MALI_GPU_ID      0x0

/*
 * Mali-G310 (Valhall, CSF) GPU_ID = product TVAX = arch_major=10,
 * arch_minor=12, arch_rev=7, product_major=4, version_status=1. Read back
 * from the GPU_ID register of a real i.MX 95 (FRDM-IMX95-PRO), where kbase
 * reports "GPU identified as 0x4 arch 10.12.7 r0p0".
 * (arch_minor MUST be 12: kbase builds its register-map LUT from
 * arch_major.minor.rev, and a wrong minor selects an incomplete map -> gpuprops
 * dereferences a NULL regmap entry and oopses.)
 */
#define MALI_GPU_ID_G310 0xAC740001u

struct IMX95MaliState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t gpu_id;
};

static uint64_t imx95_mali_read(void *opaque, hwaddr off, unsigned size)
{
    IMX95MaliState *s = opaque;

    switch (off) {
    case MALI_GPU_ID:
        return s->gpu_id;
    default:
        return 0;
    }
}

static void imx95_mali_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    /* Registration tier: writes are accepted and dropped (no compute). */
}

static const MemoryRegionOps imx95_mali_ops = {
    .read = imx95_mali_read,
    .write = imx95_mali_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx95_mali_realize(DeviceState *dev, Error **errp)
{
    IMX95MaliState *s = IMX95_MALI(dev);

    s->gpu_id = MALI_GPU_ID_G310;
    memory_region_init_io(&s->iomem, OBJECT(dev), &imx95_mali_ops, s,
                          TYPE_IMX95_MALI, MALI_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void imx95_mali_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx95_mali_realize;
    dc->desc = "NXP i.MX 95 Mali GPU (bring-up)";
}

static const TypeInfo imx95_mali_info = {
    .name           = TYPE_IMX95_MALI,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(IMX95MaliState),
    .class_init     = imx95_mali_class_init,
};

static void imx95_mali_register_types(void)
{
    type_register_static(&imx95_mali_info);
}

type_init(imx95_mali_register_types)
