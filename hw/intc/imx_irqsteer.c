/*
 * NXP i.MX IRQSTEER interrupt multiplexer ("fsl,imx-irqsteer")
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The IRQSTEER funnels a large bank of device interrupt inputs onto a small
 * number of parent (GIC) outputs. Inputs are grouped 64 per output; within the
 * register file they are addressed by 32-bit registers with a REVERSED index
 * (register idx = reg_num - input/32 - 1), matching the Linux irq-imx-irqsteer
 * driver. Each input has a mask bit (CHANMASK, 1 = enabled) and a software-set
 * bit (CHANSET); the read-only CHANSTATUS exposes (raw_input | sw_set) & mask,
 * and a parent output asserts whenever any of its group's CHANSTATUS bits are
 * set. The i.MX95 displaymix instance has 512 inputs (16 registers) and 8
 * outputs; the DPU's frame-complete / shadow-load interrupts ride inputs in
 * group 1 (inputs 64..127 -> output 1).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_IMX_IRQSTEER "imx.irqsteer"
OBJECT_DECLARE_SIMPLE_TYPE(IMXIRQSteerState, IMX_IRQSTEER)

#define IRQSTEER_NUM_INPUTS   512
#define IRQSTEER_REG_NUM      (IRQSTEER_NUM_INPUTS / 32)        /* 16 */
#define IRQSTEER_NUM_OUTPUTS  ((IRQSTEER_NUM_INPUTS + 63) / 64) /* 8 */
#define IRQSTEER_MMIO_SIZE    0x1000

/* Register map (reg_num = 16, so the per-block stride is 16 * 4 = 0x40). */
#define IRQSTEER_CHANCTRL     0x00
#define IRQSTEER_CHANMASK0    0x04   /* 16 regs: 0x04 .. 0x40 */
#define IRQSTEER_CHANSET0     0x44   /* 16 regs: 0x44 .. 0x80 */
#define IRQSTEER_CHANSTATUS0  0x84   /* 16 regs: 0x84 .. 0xc0 */
#define IRQSTEER_MINTDIS      0xc4
#define IRQSTEER_MASTRSTAT    0xc8

struct IMXIRQSteerState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    uint32_t chanctrl;
    uint32_t mask[IRQSTEER_REG_NUM];
    uint32_t set[IRQSTEER_REG_NUM];     /* software-set interrupts */
    uint32_t inlevel[IRQSTEER_REG_NUM]; /* raw input line levels */

    qemu_irq out[IRQSTEER_NUM_OUTPUTS];
};

/* Register index for input line L: reversed, per the HW/driver convention. */
static inline int irqsteer_reg_index(int line)
{
    return IRQSTEER_REG_NUM - line / 32 - 1;
}

static inline uint32_t irqsteer_status(IMXIRQSteerState *s, int idx)
{
    return (s->inlevel[idx] | s->set[idx]) & s->mask[idx];
}

/* Recompute the parent output levels: output g ORs its group's two registers. */
static void irqsteer_update(IMXIRQSteerState *s)
{
    int g;

    for (g = 0; g < IRQSTEER_NUM_OUTPUTS; g++) {
        /* Output g covers inputs [g*64, g*64+63] -> registers idx and idx-1. */
        int idx = irqsteer_reg_index(g * 64);
        uint32_t pending = irqsteer_status(s, idx);

        if (idx - 1 >= 0) {
            pending |= irqsteer_status(s, idx - 1);
        }
        qemu_set_irq(s->out[g], pending != 0);
    }
}

/* GPIO input: an upstream device drives one of the IRQSTEER input lines. */
static void irqsteer_set_input(void *opaque, int line, int level)
{
    IMXIRQSteerState *s = opaque;
    int idx = irqsteer_reg_index(line);
    uint32_t bit = 1u << (line % 32);

    if (level) {
        s->inlevel[idx] |= bit;
    } else {
        s->inlevel[idx] &= ~bit;
    }
    irqsteer_update(s);
}

static uint64_t irqsteer_read(void *opaque, hwaddr off, unsigned size)
{
    IMXIRQSteerState *s = opaque;

    if (off == IRQSTEER_CHANCTRL) {
        return s->chanctrl;
    } else if (off >= IRQSTEER_CHANMASK0 && off < IRQSTEER_CHANSET0) {
        return s->mask[(off - IRQSTEER_CHANMASK0) / 4];
    } else if (off >= IRQSTEER_CHANSET0 && off < IRQSTEER_CHANSTATUS0) {
        return s->set[(off - IRQSTEER_CHANSET0) / 4];
    } else if (off >= IRQSTEER_CHANSTATUS0 && off < IRQSTEER_MINTDIS) {
        return irqsteer_status(s, (off - IRQSTEER_CHANSTATUS0) / 4);
    }
    /* MINTDIS / MASTRSTAT and anything else read as 0. */
    return 0;
}

static void irqsteer_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size)
{
    IMXIRQSteerState *s = opaque;

    if (off == IRQSTEER_CHANCTRL) {
        s->chanctrl = val;
    } else if (off >= IRQSTEER_CHANMASK0 && off < IRQSTEER_CHANSET0) {
        s->mask[(off - IRQSTEER_CHANMASK0) / 4] = val;
        irqsteer_update(s);
    } else if (off >= IRQSTEER_CHANSET0 && off < IRQSTEER_CHANSTATUS0) {
        s->set[(off - IRQSTEER_CHANSET0) / 4] = val;
        irqsteer_update(s);
    }
    /* CHANSTATUS is read-only; MINTDIS/MASTRSTAT unmodelled. */
}

static const MemoryRegionOps irqsteer_ops = {
    .read = irqsteer_read,
    .write = irqsteer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void irqsteer_reset(DeviceState *dev)
{
    IMXIRQSteerState *s = IMX_IRQSTEER(dev);
    int i;

    s->chanctrl = 0;
    for (i = 0; i < IRQSTEER_REG_NUM; i++) {
        s->mask[i] = 0;
        s->set[i] = 0;
        s->inlevel[i] = 0;
    }
    irqsteer_update(s);
}

static void irqsteer_init(Object *obj)
{
    IMXIRQSteerState *s = IMX_IRQSTEER(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &irqsteer_ops, s,
                          TYPE_IMX_IRQSTEER, IRQSTEER_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    qdev_init_gpio_in(DEVICE(obj), irqsteer_set_input, IRQSTEER_NUM_INPUTS);
    for (int i = 0; i < IRQSTEER_NUM_OUTPUTS; i++) {
        sysbus_init_irq(sbd, &s->out[i]);
    }
}

static const VMStateDescription vmstate_irqsteer = {
    .name = TYPE_IMX_IRQSTEER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(chanctrl, IMXIRQSteerState),
        VMSTATE_UINT32_ARRAY(mask, IMXIRQSteerState, IRQSTEER_REG_NUM),
        VMSTATE_UINT32_ARRAY(set, IMXIRQSteerState, IRQSTEER_REG_NUM),
        VMSTATE_UINT32_ARRAY(inlevel, IMXIRQSteerState, IRQSTEER_REG_NUM),
        VMSTATE_END_OF_LIST()
    },
};

static void irqsteer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_irqsteer;
    device_class_set_legacy_reset(dc, irqsteer_reset);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "NXP i.MX IRQSTEER";
}

static const TypeInfo imx_irqsteer_info = {
    .name          = TYPE_IMX_IRQSTEER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXIRQSteerState),
    .instance_init = irqsteer_init,
    .class_init    = irqsteer_class_init,
};

static void imx_irqsteer_register_types(void)
{
    type_register_static(&imx_irqsteer_info);
}

type_init(imx_irqsteer_register_types)
