/*
 * NXP i.MX 95 / i.MX 8ULP RGPIO controller ("fsl,imx95-gpio",
 * "fsl,imx8ulp-gpio")
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This is the Vybrid/8ULP-style GPIO the Linux gpio-vf610 driver binds, NOT
 * the legacy i.MX GPIO modelled by hw/gpio/imx_gpio.c. Within the 4 KiB window
 * there are two register sub-blocks:
 *
 *   - GPIO data block at +0x40: PDOR (output), PSOR/PCOR/PTOR (set/clear/
 *     toggle), PDIR (pad input readback), PDDR (direction, 1 = output).
 *   - PORT block at +0x80: PCR[0..31] (per-pin control; the IRQC field at
 *     bits [19:16] selects the interrupt trigger), ISFR (interrupt status,
 *     write-1-to-clear), and the digital-filter regs (DFER/DFCR/DFWR).
 *
 * 32 GPIO input lines (driven by board wiring), 32 output lines (to consumers
 * such as reset / regulator-enable pins), and the GIC interrupt outputs. The
 * gpio-vf610 driver requests only the first GIC line and its chained handler
 * walks the whole ISFR, so output 0 fires for any pending pin; output 1
 * mirrors the upper half to stay faithful to the two-IRQ HW split.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_IMX95_GPIO "imx95.gpio"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95GPIOState, IMX95_GPIO)

#define IMX95_GPIO_PINS     32
#define IMX95_GPIO_MMIO     0x1000

/* Identification (read-only; values are plausible, not load-bearing). */
#define RGPIO_VERID         0x00
#define RGPIO_PARAM         0x04

/* GPIO data block (base + 0x40). */
#define RGPIO_PDOR          0x40    /* output data       */
#define RGPIO_PSOR          0x44    /* set   (W1S)       */
#define RGPIO_PCOR          0x48    /* clear (W1C)       */
#define RGPIO_PTOR          0x4c    /* toggle (W1T)      */
#define RGPIO_PDIR          0x50    /* pad input readback */
#define RGPIO_PDDR          0x54    /* direction, 1 = out */

/* PORT block (base + 0x80). */
#define RGPIO_PCR0          0x80    /* per-pin control, 32 regs 0x80..0xfc */
#define RGPIO_ISFR          0x120   /* 0x80 + 0xa0, interrupt status (W1C) */
#define RGPIO_DFER          0x140   /* 0x80 + 0xc0, digital-filter enable  */
#define RGPIO_DFCR          0x144
#define RGPIO_DFWR          0x148

/* PCR interrupt-config field. */
#define PCR_IRQC_SHIFT      16
#define PCR_IRQC_MASK       0xf
#define IRQC_OFF            0x0
#define IRQC_LOGIC_ZERO     0x8
#define IRQC_RISING_EDGE    0x9
#define IRQC_FALLING_EDGE   0xa
#define IRQC_EITHER_EDGE    0xb
#define IRQC_LOGIC_ONE      0xc

struct IMX95GPIOState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    uint32_t pdor;                  /* output data            */
    uint32_t pddr;                  /* direction (1 = output) */
    uint32_t in_level;              /* external input levels  */
    uint32_t isfr;                  /* interrupt status flags */
    uint32_t pcr[IMX95_GPIO_PINS];  /* per-pin control        */
    uint32_t dfer, dfcr, dfwr;      /* digital filter         */

    qemu_irq out[IMX95_GPIO_PINS];  /* to consumers (reset / regulator pins) */
    qemu_irq irq[2];                /* to GIC (driver uses [0]) */
};

static inline uint32_t imx95_gpio_pin_irqc(IMX95GPIOState *s, int pin)
{
    return (s->pcr[pin] >> PCR_IRQC_SHIFT) & PCR_IRQC_MASK;
}

/* Pad readback: output pins report their driven value, inputs their level. */
static uint32_t imx95_gpio_pdir(IMX95GPIOState *s)
{
    return (s->pdor & s->pddr) | (s->in_level & ~s->pddr);
}

/* Drive each output pin's consumer line; input pins read low (undriven). */
static void imx95_gpio_update_outputs(IMX95GPIOState *s)
{
    uint32_t driven = s->pdor & s->pddr;
    int i;

    for (i = 0; i < IMX95_GPIO_PINS; i++) {
        qemu_set_irq(s->out[i], (driven >> i) & 1);
    }
}

static void imx95_gpio_update_irq(IMX95GPIOState *s)
{
    qemu_set_irq(s->irq[0], s->isfr != 0);
    qemu_set_irq(s->irq[1], (s->isfr & 0xffff0000) != 0);
}

/*
 * Evaluate one pin's level-sensitive interrupt condition (edges are handled in
 * the input setter). Used after a PCR write so configuring a level trigger over
 * an already-matching input latches ISFR like the HW does.
 */
static void imx95_gpio_eval_level(IMX95GPIOState *s, int pin)
{
    uint32_t level = (s->in_level >> pin) & 1;

    switch (imx95_gpio_pin_irqc(s, pin)) {
    case IRQC_LOGIC_ZERO:
        if (!level) {
            s->isfr |= 1u << pin;
        }
        break;
    case IRQC_LOGIC_ONE:
        if (level) {
            s->isfr |= 1u << pin;
        }
        break;
    default:
        break;
    }
}

/* Board/device drives a GPIO input line. */
static void imx95_gpio_set_input(void *opaque, int line, int level)
{
    IMX95GPIOState *s = opaque;
    uint32_t bit = 1u << line;
    bool old = s->in_level & bit;
    bool new = level;

    if (level) {
        s->in_level |= bit;
    } else {
        s->in_level &= ~bit;
    }

    switch (imx95_gpio_pin_irqc(s, line)) {
    case IRQC_LOGIC_ZERO:
        if (!new) {
            s->isfr |= bit;
        }
        break;
    case IRQC_RISING_EDGE:
        if (!old && new) {
            s->isfr |= bit;
        }
        break;
    case IRQC_FALLING_EDGE:
        if (old && !new) {
            s->isfr |= bit;
        }
        break;
    case IRQC_EITHER_EDGE:
        if (old != new) {
            s->isfr |= bit;
        }
        break;
    case IRQC_LOGIC_ONE:
        if (new) {
            s->isfr |= bit;
        }
        break;
    default:
        break;
    }
    imx95_gpio_update_irq(s);
}

static uint64_t imx95_gpio_read(void *opaque, hwaddr off, unsigned size)
{
    IMX95GPIOState *s = opaque;

    if (off >= RGPIO_PCR0 && off < RGPIO_PCR0 + IMX95_GPIO_PINS * 4) {
        return s->pcr[(off - RGPIO_PCR0) / 4];
    }

    switch (off) {
    /*
     * Read back from every RGPIO instance of a real i.MX 95. PARAM is NOT a pin
     * count (silicon reports 2, not 32) - gpio-vf610 hardcodes 32 pins per port
     * and never reads either register, so these are identification only.
     */
    case RGPIO_VERID:
        return 0x02010001;
    case RGPIO_PARAM:
        return 0x00000002;
    case RGPIO_PDOR:
        return s->pdor;
    case RGPIO_PSOR:
    case RGPIO_PCOR:
    case RGPIO_PTOR:
        return 0;                    /* set/clear/toggle read as 0 */
    case RGPIO_PDIR:
        return imx95_gpio_pdir(s);
    case RGPIO_PDDR:
        return s->pddr;
    case RGPIO_ISFR:
        return s->isfr;
    case RGPIO_DFER:
        return s->dfer;
    case RGPIO_DFCR:
        return s->dfcr;
    case RGPIO_DFWR:
        return s->dfwr;
    default:
        return 0;
    }
}

static void imx95_gpio_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    IMX95GPIOState *s = opaque;
    uint32_t v = val;

    if (off >= RGPIO_PCR0 && off < RGPIO_PCR0 + IMX95_GPIO_PINS * 4) {
        int pin = (off - RGPIO_PCR0) / 4;

        s->pcr[pin] = v;
        imx95_gpio_eval_level(s, pin);
        imx95_gpio_update_irq(s);
        return;
    }

    switch (off) {
    case RGPIO_PDOR:
        s->pdor = v;
        imx95_gpio_update_outputs(s);
        break;
    case RGPIO_PSOR:
        s->pdor |= v;
        imx95_gpio_update_outputs(s);
        break;
    case RGPIO_PCOR:
        s->pdor &= ~v;
        imx95_gpio_update_outputs(s);
        break;
    case RGPIO_PTOR:
        s->pdor ^= v;
        imx95_gpio_update_outputs(s);
        break;
    case RGPIO_PDDR:
        s->pddr = v;
        imx95_gpio_update_outputs(s);
        break;
    case RGPIO_ISFR:
        s->isfr &= ~v;               /* write 1 to clear */
        imx95_gpio_update_irq(s);
        break;
    case RGPIO_DFER:
        s->dfer = v;
        break;
    case RGPIO_DFCR:
        s->dfcr = v;
        break;
    case RGPIO_DFWR:
        s->dfwr = v;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps imx95_gpio_ops = {
    .read = imx95_gpio_read,
    .write = imx95_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx95_gpio_reset(DeviceState *dev)
{
    IMX95GPIOState *s = IMX95_GPIO(dev);

    s->pdor = 0;
    s->pddr = 0;
    s->isfr = 0;
    s->dfer = s->dfcr = s->dfwr = 0;
    memset(s->pcr, 0, sizeof(s->pcr));
    /* in_level is owned by the board wiring; leave it. */
    imx95_gpio_update_outputs(s);
    imx95_gpio_update_irq(s);
}

static void imx95_gpio_init(Object *obj)
{
    IMX95GPIOState *s = IMX95_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &imx95_gpio_ops, s,
                          TYPE_IMX95_GPIO, IMX95_GPIO_MMIO);
    sysbus_init_mmio(sbd, &s->iomem);
    /*
     * Undriven inputs idle HIGH: on-board interrupt lines (i2c-expander INT
     * pins, buttons) are active-low with pull-ups, so an unwired pin must read
     * deasserted - otherwise a driver that configures it level-low (e.g.
     * gpio-pca953x for an expander INT) takes a spurious, unending interrupt.
     */
    s->in_level = 0xffffffff;
    qdev_init_gpio_in(DEVICE(obj), imx95_gpio_set_input, IMX95_GPIO_PINS);
    qdev_init_gpio_out(DEVICE(obj), s->out, IMX95_GPIO_PINS);
    sysbus_init_irq(sbd, &s->irq[0]);
    sysbus_init_irq(sbd, &s->irq[1]);
}

static const VMStateDescription vmstate_imx95_gpio = {
    .name = TYPE_IMX95_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(pdor, IMX95GPIOState),
        VMSTATE_UINT32(pddr, IMX95GPIOState),
        VMSTATE_UINT32(in_level, IMX95GPIOState),
        VMSTATE_UINT32(isfr, IMX95GPIOState),
        VMSTATE_UINT32_ARRAY(pcr, IMX95GPIOState, IMX95_GPIO_PINS),
        VMSTATE_UINT32(dfer, IMX95GPIOState),
        VMSTATE_UINT32(dfcr, IMX95GPIOState),
        VMSTATE_UINT32(dfwr, IMX95GPIOState),
        VMSTATE_END_OF_LIST()
    },
};

static void imx95_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_imx95_gpio;
    device_class_set_legacy_reset(dc, imx95_gpio_reset);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "NXP i.MX95 RGPIO controller";
}

static const TypeInfo imx95_gpio_info = {
    .name          = TYPE_IMX95_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMX95GPIOState),
    .instance_init = imx95_gpio_init,
    .class_init    = imx95_gpio_class_init,
};

static void imx95_gpio_register_types(void)
{
    type_register_static(&imx95_gpio_info);
}

type_init(imx95_gpio_register_types)
