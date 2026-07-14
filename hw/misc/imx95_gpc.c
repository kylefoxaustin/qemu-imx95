/*
 * NXP i.MX 95 GPC (General Power Controller) stub model
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Covers the GPC block at 0x44470000: the per-domain GPC_CPU_CTRL
 * instances (CM33 @+0x0000, CM7 @+0x0800, CA55_0..5 @+0x1000..+0x3800,
 * CA55_CLUSTER @+0x4000) and GPC_GLOBAL @+0x4800.
 *
 * The System Manager drives CPU power modes by writing CMC_MODE_CTRL
 * (CPU_MODE_TARGET, offset 0x10 in a CPU_CTRL block) and polling
 * CMC_MODE_STAT (CPU_MODE_CURRENT, offset 0x14) until the mode settles.
 * It also uses the CMC_SLEEP_*_CTRL / *_STAT handshake pairs (a *_STAT
 * at *_CTRL + 4). QEMU has no power hardware, so transitions are
 * instantaneous: every status register reflects the value last written
 * to its paired control register. Everything else is plain storage.
 *
 * This is the register-class triage from v0.7's XCACHE applied to a
 * control/status block: control bits persist (RAM), status registers
 * mirror their control so "request X, wait for X" loops converge.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"

#define TYPE_IMX95_GPC "imx95.gpc"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95GPCState, IMX95_GPC)

#define IMX95_GPC_REG_SIZE      0x10000
#define IMX95_GPC_NUM_WORDS     (IMX95_GPC_REG_SIZE / 4)

/* GPC_CPU_CTRL per-domain block layout (the GLOBAL block lives at +0x4800). */
#define GPC_GLOBAL_OFF          0x4800
#define CPU_CTRL_BLOCK_SIZE     0x800
#define CMC_MODE_CTRL           0x10    /* CPU_MODE_TARGET in [1:0] */
#define CMC_MODE_STAT           0x14    /* CPU_MODE_CURRENT in [1:0] */

struct IMX95GPCState {
    SysBusDevice    parent_obj;
    MemoryRegion    iomem;
    uint32_t        regs[IMX95_GPC_NUM_WORDS];
};

/*
 * Is `reg` (offset within a CPU_CTRL block) a status register that mirrors
 * the control register 4 bytes below it? Covers CMC_MODE_STAT (0x14) and
 * the CMC_SLEEP_*_STAT handshake registers (0x204, 0x20c, ... 0x24c), all
 * of which sit at their paired *_CTRL + 4.
 */
static bool gpc_is_mirror_stat(uint32_t reg)
{
    if (reg == CMC_MODE_STAT) {
        return true;
    }
    /* SLEEP/WAKEUP handshake STATs: odd-word in the 0x200..0x2ff range. */
    if (reg >= 0x204 && reg <= 0x2fc && ((reg & 0x4) != 0)) {
        return true;
    }
    return false;
}

static uint64_t imx95_gpc_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX95GPCState *s = opaque;

    if (offset < GPC_GLOBAL_OFF) {
        uint32_t reg = offset & (CPU_CTRL_BLOCK_SIZE - 1);
        if (gpc_is_mirror_stat(reg)) {
            /* Status mirrors the paired control (transition is instant). */
            return s->regs[(offset - 4) / 4];
        }
    }
    return s->regs[offset / 4];
}

static void imx95_gpc_write(void *opaque, hwaddr offset,
                            uint64_t value, unsigned size)
{
    IMX95GPCState *s = opaque;

    s->regs[offset / 4] = value;
}

static const MemoryRegionOps imx95_gpc_ops = {
    .read = imx95_gpc_read,
    .write = imx95_gpc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/*
 * RM reset column, per CPU_CTRL (CMC) block. We memset the whole region, which
 * is a claim that every reset bit here is zero; silicon says otherwise. The
 * consumer is the M33 System Manager firmware, so this is BOOT-CRITICAL and is
 * A/B'd against a real SM boot rather than reasoned about.
 *
 * The SLEEP/WAKEUP *_STAT registers MIRROR the *_CTRL four bytes below them in
 * this model (a "request X, wait for X" handshake completes instantly), so
 * seeding a CTRL to its reset value moves its STAT with it - which is what
 * silicon does too, and is why the SM's wait loops still converge.
 */
#define CMC_AUTHEN_CTRL             0x004
#define CMC_AUTHEN_CTRL_RESET       0xffff0000u
#define CMC_MISC                    0x00c
#define CMC_MISC_RESET              0x0000000eu
#define CMC_NON_IRQ_WAKEUP_MASK     0x140
#define CMC_NON_IRQ_WAKEUP_MASK_RST 0x00000001u
#define CMC_SLEEP_A55_HDSK_CTRL     0x200
#define CMC_SLEEP_SSAR_CTRL         0x208
#define CMC_SLEEP_RESET_CTRL        0x230
#define CMC_SLEEP_SYSMAN_CTRL       0x248
#define CMC_WAKEUP_POWER_CTRL       0x290
#define CMC_HDSK_CTRL_RESET         0x00000004u

static void imx95_gpc_reset(DeviceState *dev)
{
    IMX95GPCState *s = IMX95_GPC(dev);
    hwaddr blk;

    memset(s->regs, 0, sizeof(s->regs));

    for (blk = 0; blk < GPC_GLOBAL_OFF; blk += CPU_CTRL_BLOCK_SIZE) {
        s->regs[(blk + CMC_AUTHEN_CTRL) / 4] = CMC_AUTHEN_CTRL_RESET;
        s->regs[(blk + CMC_MISC) / 4] = CMC_MISC_RESET;
        s->regs[(blk + CMC_NON_IRQ_WAKEUP_MASK) / 4] =
            CMC_NON_IRQ_WAKEUP_MASK_RST;
        s->regs[(blk + CMC_SLEEP_A55_HDSK_CTRL) / 4] = CMC_HDSK_CTRL_RESET;
        s->regs[(blk + CMC_SLEEP_SSAR_CTRL) / 4] = CMC_HDSK_CTRL_RESET;
        s->regs[(blk + CMC_SLEEP_RESET_CTRL) / 4] = CMC_HDSK_CTRL_RESET;
        s->regs[(blk + CMC_SLEEP_SYSMAN_CTRL) / 4] = CMC_HDSK_CTRL_RESET;
        s->regs[(blk + CMC_WAKEUP_POWER_CTRL) / 4] = CMC_HDSK_CTRL_RESET;
    }
}

static void imx95_gpc_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    IMX95GPCState *s = IMX95_GPC(obj);

    memory_region_init_io(&s->iomem, obj, &imx95_gpc_ops, s,
                          TYPE_IMX95_GPC, IMX95_GPC_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_imx95_gpc = {
    .name = TYPE_IMX95_GPC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX95GPCState, IMX95_GPC_NUM_WORDS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx95_gpc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_imx95_gpc;
    device_class_set_legacy_reset(dc, imx95_gpc_reset);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "NXP i.MX 95 GPC (stub)";
}

static const TypeInfo imx95_gpc_info = {
    .name           = TYPE_IMX95_GPC,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(IMX95GPCState),
    .instance_init  = imx95_gpc_init,
    .class_init     = imx95_gpc_class_init,
};

static void imx95_gpc_register_types(void)
{
    type_register_static(&imx95_gpc_info);
}

type_init(imx95_gpc_register_types)
