/*
 * NXP i.MX 95 Low Power SPI (LPSPI) master
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A functional LPSPI master that bridges TDR writes onto a QEMU SSI bus (so SPI
 * peripherals can be attached) and returns shifted-in data via RDR, mirroring
 * how hw/i2c/imx_lpi2c bridges to the I2C bus. Models the registers the
 * spi-fsl-lpspi driver uses: VERID/PARAM (FIFO sizing), CR (enable/reset),
 * SR/IER (TDF/RDF/TCF/FCF + interrupts), FSR/RSR (FIFO levels), TCR (frame
 * size, chip-select, continuous) and TDR/RDR. Each TDR write performs the
 * transfer immediately, raising the frame/transfer-complete interrupt the
 * driver waits on.
 */

#include "qemu/osdep.h"
#include "hw/ssi/imx95_lpspi.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define LPSPI_VERID  0x00
#define LPSPI_PARAM  0x04
#define LPSPI_CR     0x10
#define LPSPI_SR     0x14
#define LPSPI_IER    0x18
#define LPSPI_CFGR1  0x24
#define LPSPI_FSR    0x5c
#define LPSPI_TCR    0x60
#define LPSPI_TDR    0x64
#define LPSPI_RSR    0x70
#define LPSPI_RDR    0x74

#define CR_MEN   (1u << 0)
#define CR_RST   (1u << 1)
#define CR_RTF   (1u << 8)   /* reset tx fifo */
#define CR_RRF   (1u << 9)   /* reset rx fifo */

#define SR_TDF   (1u << 0)
#define SR_RDF   (1u << 1)
#define SR_FCF   (1u << 9)
#define SR_TCF   (1u << 10)
#define SR_CLEAR_MASK 0x00003f00   /* W1C bits [13:8] */

#define RSR_RXEMPTY (1u << 1)

#define TCR_FRAMESZ_MASK 0xfff
#define TCR_CONT (1u << 21)

/* PARAM: tx/rx FIFO depth = 1 << nibble; report 16-entry FIFOs. */
#define LPSPI_PARAM_VALUE 0x00000404
#define LPSPI_VERID_VALUE 0x02000004
#define LPSPI_FIFO_DEPTH  16

static void lpspi_update_irq(IMX95LpspiState *s)
{
    uint32_t sr = s->sr | SR_TDF | (fifo32_is_empty(&s->rx) ? 0 : SR_RDF);

    qemu_set_irq(s->irq, !!(sr & s->ier & (SR_TDF | SR_RDF | SR_FCF | SR_TCF)));
}

static void lpspi_transfer(IMX95LpspiState *s, uint32_t tx)
{
    uint32_t bits = (s->tcr & TCR_FRAMESZ_MASK) + 1;
    uint32_t nbytes = (bits + 7) / 8;
    uint32_t rx = 0;
    int i;

    if (nbytes == 0 || nbytes > 4) {
        nbytes = 1;
    }
    /* Shift MSB byte first, as SPI controllers do. */
    for (i = nbytes - 1; i >= 0; i--) {
        uint8_t out = (tx >> (i * 8)) & 0xff;
        uint8_t in = ssi_transfer(s->bus, out);

        rx |= (uint32_t)in << (i * 8);
    }
    if (!fifo32_is_full(&s->rx)) {
        fifo32_push(&s->rx, rx);
    }
    s->sr |= SR_TCF;
    if (!(s->tcr & TCR_CONT)) {
        s->sr |= SR_FCF;        /* CS deasserts: frame complete */
    }
}

static uint64_t lpspi_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX95LpspiState *s = opaque;
    uint32_t val;

    switch (offset) {
    case LPSPI_VERID:
        return LPSPI_VERID_VALUE;
    case LPSPI_PARAM:
        return LPSPI_PARAM_VALUE;
    case LPSPI_CR:
        return s->cr;
    case LPSPI_SR:
        val = s->sr | SR_TDF;
        if (!fifo32_is_empty(&s->rx)) {
            val |= SR_RDF;
        }
        return val;
    case LPSPI_IER:
        return s->ier;
    case LPSPI_CFGR1:
        return s->cfgr1;
    case LPSPI_FSR:
        return fifo32_num_used(&s->rx) << 16;   /* tx=0, rx in [23:16] */
    case LPSPI_TCR:
        return s->tcr;
    case LPSPI_RSR:
        return fifo32_is_empty(&s->rx) ? RSR_RXEMPTY : 0;
    case LPSPI_RDR:
        val = fifo32_is_empty(&s->rx) ? 0 : fifo32_pop(&s->rx);
        lpspi_update_irq(s);
        return val;
    default:
        return 0;
    }
}

static void lpspi_write(void *opaque, hwaddr offset, uint64_t value,
                        unsigned size)
{
    IMX95LpspiState *s = opaque;

    switch (offset) {
    case LPSPI_CR:
        if (value & CR_RST) {
            s->sr = 0;
            fifo32_reset(&s->rx);
        }
        if (value & CR_RRF) {
            fifo32_reset(&s->rx);
        }
        s->cr = value & ~(CR_RST | CR_RRF | CR_RTF);
        lpspi_update_irq(s);
        break;
    case LPSPI_SR:
        s->sr &= ~(value & SR_CLEAR_MASK);
        lpspi_update_irq(s);
        break;
    case LPSPI_IER:
        s->ier = value;
        lpspi_update_irq(s);
        break;
    case LPSPI_CFGR1:
        s->cfgr1 = value;
        break;
    case LPSPI_TCR:
        s->tcr = value;
        break;
    case LPSPI_TDR:
        if (s->cr & CR_MEN) {
            lpspi_transfer(s, value);
            lpspi_update_irq(s);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps lpspi_ops = {
    .read = lpspi_read,
    .write = lpspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void lpspi_reset(DeviceState *dev)
{
    IMX95LpspiState *s = IMX95_LPSPI(dev);

    s->cr = 0;
    s->sr = 0;
    s->ier = 0;
    s->cfgr1 = 0;
    s->tcr = 0;
    fifo32_reset(&s->rx);
}

static void lpspi_realize(DeviceState *dev, Error **errp)
{
    IMX95LpspiState *s = IMX95_LPSPI(dev);

    s->bus = ssi_create_bus(dev, s->bus_name ? s->bus_name : "spi");
    fifo32_create(&s->rx, LPSPI_FIFO_DEPTH);
    memory_region_init_io(&s->iomem, OBJECT(dev), &lpspi_ops, s,
                          TYPE_IMX95_LPSPI, IMX95_LPSPI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_lpspi = {
    .name = TYPE_IMX95_LPSPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cr, IMX95LpspiState),
        VMSTATE_UINT32(sr, IMX95LpspiState),
        VMSTATE_UINT32(ier, IMX95LpspiState),
        VMSTATE_UINT32(cfgr1, IMX95LpspiState),
        VMSTATE_UINT32(tcr, IMX95LpspiState),
        VMSTATE_FIFO32(rx, IMX95LpspiState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property lpspi_properties[] = {
    DEFINE_PROP_STRING("bus-name", IMX95LpspiState, bus_name),
};

static void lpspi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = lpspi_realize;
    dc->vmsd = &vmstate_lpspi;
    device_class_set_legacy_reset(dc, lpspi_reset);
    device_class_set_props(dc, lpspi_properties);
    dc->desc = "i.MX 95 LPSPI master";
}

static const TypeInfo lpspi_types[] = {
    {
        .name = TYPE_IMX95_LPSPI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX95LpspiState),
        .class_init = lpspi_class_init,
    },
};

DEFINE_TYPES(lpspi_types)
