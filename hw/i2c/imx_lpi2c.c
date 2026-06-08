/*
 * NXP i.MX LPI2C master controller
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the master side of the i.MX LPI2C used by the System Manager
 * firmware to talk to the board PMIC / IO-expander. Only the polled
 * blocking-transfer path the SM driver uses is modelled:
 *
 *   - MTDR is a command FIFO: the agent writes START (with address),
 *     TX-data, RX-data (count) and STOP commands.
 *   - Each command is executed synchronously against the QEMU I2C bus
 *     (i2c_start_transfer / i2c_send / i2c_recv / i2c_end_transfer).
 *   - MSR exposes the status flags the driver polls: TDF (tx ready,
 *     always set since we drain the FIFO immediately), RDF (rx data
 *     available), SDF (stop detected), NDF (NACK), MBF/BBF (busy).
 *   - MRDR pops received bytes (RXEMPTY when the rx FIFO is empty).
 *
 * No interrupts, DMA, slave mode, clocking or timing are modelled.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

#define TYPE_IMX_LPI2C "imx.lpi2c"
OBJECT_DECLARE_SIMPLE_TYPE(IMXLPI2CState, IMX_LPI2C)

#define IMX_LPI2C_REG_SIZE      0x10000

/* Register offsets. */
#define LPI2C_VERID             0x00
#define LPI2C_PARAM             0x04
#define LPI2C_MCR               0x10
#define LPI2C_MSR               0x14
#define LPI2C_MIER              0x18
#define LPI2C_MCFGR1            0x24
#define LPI2C_MFSR              0x5C
#define LPI2C_MTDR              0x60
#define LPI2C_MRDR              0x70

/* MCR bits. */
#define MCR_MEN                 (1u << 0)
#define MCR_RST                 (1u << 1)
#define MCR_RTF                 (1u << 8)   /* reset tx FIFO (self-clearing) */
#define MCR_RRF                 (1u << 9)   /* reset rx FIFO (self-clearing) */

/* MSR bits. */
#define MSR_TDF                 (1u << 0)
#define MSR_RDF                 (1u << 1)
#define MSR_EPF                 (1u << 8)
#define MSR_SDF                 (1u << 9)
#define MSR_NDF                 (1u << 10)
#define MSR_MBF                 (1u << 24)
#define MSR_BBF                 (1u << 25)
/* Write-1-to-clear status bits. */
#define MSR_W1C                 (MSR_EPF | MSR_SDF | MSR_NDF | (0xfu << 11))

/* MTDR command field [10:8]. */
#define MTDR_CMD_TXDATA         0x0
#define MTDR_CMD_RXDATA         0x1
#define MTDR_CMD_STOP           0x2
#define MTDR_CMD_START          0x4

/* MRDR bits. */
#define MRDR_RXEMPTY            (1u << 14)

#define RXFIFO_LEN              256

struct IMXLPI2CState {
    SysBusDevice    parent_obj;

    MemoryRegion    iomem;
    qemu_irq        irq;
    I2CBus         *bus;
    char           *bus_name;   /* qbus name for -device foo,bus=<name> */

    uint32_t        mcr;
    uint32_t        msr;       /* sticky status bits (SDF/NDF/MBF/BBF/...) */
    uint32_t        mier;      /* interrupt enables (TDIE/RDIE/SDIE/NDIE) */
    uint32_t        mcfgr1;

    uint8_t         rxfifo[RXFIFO_LEN];
    unsigned        rx_head;
    unsigned        rx_count;
};

static void imx_lpi2c_rx_push(IMXLPI2CState *s, uint8_t b)
{
    if (s->rx_count < RXFIFO_LEN) {
        unsigned tail = (s->rx_head + s->rx_count) % RXFIFO_LEN;
        s->rxfifo[tail] = b;
        s->rx_count++;
    }
}

/*
 * Effective MSR for interrupt purposes: the sticky bits plus the two
 * level-derived flags (TDF is always ready since we drain the tx command
 * FIFO immediately; RDF tracks the rx FIFO). The MIER enable bits sit at the
 * same positions as their MSR flags (TDIE/TDF=0, RDIE/RDF=1, SDIE/SDF=9,
 * NDIE/NDF=10), so a plain AND yields the active, enabled sources.
 */
static void imx_lpi2c_update_irq(IMXLPI2CState *s)
{
    uint32_t eff = s->msr | MSR_TDF | (s->rx_count ? MSR_RDF : 0);

    qemu_set_irq(s->irq, (eff & s->mier) != 0);
}

static void imx_lpi2c_do_command(IMXLPI2CState *s, uint32_t val)
{
    uint32_t cmd = (val >> 8) & 0x7;
    uint8_t data = val & 0xff;

    switch (cmd) {
    case MTDR_CMD_START: {
        /* DATA = (addr << 1) | rw. */
        uint8_t addr = data >> 1;
        bool recv = data & 1;
        if (i2c_start_transfer(s->bus, addr, recv) != 0) {
            s->msr |= MSR_NDF;     /* no slave acked the address */
        } else {
            s->msr |= MSR_MBF | MSR_BBF;
        }
        break;
    }
    case MTDR_CMD_TXDATA:
        if (i2c_send(s->bus, data) != 0) {
            s->msr |= MSR_NDF;
        }
        break;
    case MTDR_CMD_RXDATA: {
        /* Receive (DATA + 1) bytes. */
        unsigned n = (unsigned)data + 1;
        for (unsigned i = 0; i < n; i++) {
            imx_lpi2c_rx_push(s, i2c_recv(s->bus));
        }
        break;
    }
    case MTDR_CMD_STOP:
        i2c_end_transfer(s->bus);
        s->msr |= MSR_SDF | MSR_EPF;
        s->msr &= ~(MSR_MBF | MSR_BBF);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled MTDR cmd %u\n", __func__, cmd);
        break;
    }
}

static uint64_t imx_lpi2c_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXLPI2CState *s = opaque;

    switch (offset) {
    case LPI2C_VERID:
        return 0x01000003;     /* plausible LPI2C version */
    case LPI2C_PARAM:
        return 0x00000303;     /* 8-deep tx/rx FIFOs (not load-bearing) */
    case LPI2C_MCR:
        return s->mcr;
    case LPI2C_MSR: {
        /*
         * TDF is always ready (we drain the tx command immediately);
         * RDF reflects the rx FIFO.
         */
        uint32_t v = s->msr | MSR_TDF;
        if (s->rx_count) {
            v |= MSR_RDF;
        }
        return v;
    }
    case LPI2C_MIER:
        return s->mier;
    case LPI2C_MCFGR1:
        return s->mcfgr1;
    case LPI2C_MFSR:
        /* rx FIFO count in [22:16]; tx always empty. */
        return (s->rx_count & 0x7f) << 16;
    case LPI2C_MRDR:
        if (s->rx_count == 0) {
            return MRDR_RXEMPTY;
        } else {
            uint8_t b = s->rxfifo[s->rx_head];
            s->rx_head = (s->rx_head + 1) % RXFIFO_LEN;
            s->rx_count--;
            imx_lpi2c_update_irq(s);
            return b;
        }
    default:
        return 0;
    }
}

static void imx_lpi2c_write(void *opaque, hwaddr offset,
                            uint64_t value, unsigned size)
{
    IMXLPI2CState *s = opaque;

    switch (offset) {
    case LPI2C_MCR:
        if (value & MCR_RRF) {
            s->rx_head = s->rx_count = 0;
        }
        /* RST / RTF / RRF are momentary; don't persist them. */
        s->mcr = value & ~(MCR_RST | MCR_RTF | MCR_RRF);
        break;
    case LPI2C_MSR:
        /* Write-1-to-clear the status flags. */
        s->msr &= ~((uint32_t)value & MSR_W1C);
        imx_lpi2c_update_irq(s);
        break;
    case LPI2C_MIER:
        s->mier = value;
        imx_lpi2c_update_irq(s);
        break;
    case LPI2C_MCFGR1:
        s->mcfgr1 = value;
        break;
    case LPI2C_MTDR:
        if (s->mcr & MCR_MEN) {
            imx_lpi2c_do_command(s, value);
            imx_lpi2c_update_irq(s);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps imx_lpi2c_ops = {
    .read = imx_lpi2c_read,
    .write = imx_lpi2c_write,
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

static void imx_lpi2c_reset(DeviceState *dev)
{
    IMXLPI2CState *s = IMX_LPI2C(dev);

    s->mcr = 0;
    s->msr = 0;
    s->mier = 0;
    s->mcfgr1 = 0;
    s->rx_head = s->rx_count = 0;
    qemu_set_irq(s->irq, 0);
}

static void imx_lpi2c_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    IMXLPI2CState *s = IMX_LPI2C(obj);

    memory_region_init_io(&s->iomem, obj, &imx_lpi2c_ops, s,
                          TYPE_IMX_LPI2C, IMX_LPI2C_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

/*
 * The I2C bus is created at realize (not instance_init) so it can adopt the
 * "bus-name" property: a unique, predictable qbus name (e.g. "lpi2c4") lets a
 * user hang slaves on it from the command line - `-device tmp105,bus=lpi2c4`.
 * Defaults to "i2c" when unset so a bare instance keeps the old child-bus name.
 */
static void imx_lpi2c_realize(DeviceState *dev, Error **errp)
{
    IMXLPI2CState *s = IMX_LPI2C(dev);

    s->bus = i2c_init_bus(dev, s->bus_name ? s->bus_name : "i2c");
}

static const Property imx_lpi2c_properties[] = {
    DEFINE_PROP_STRING("bus-name", IMXLPI2CState, bus_name),
};

static const VMStateDescription vmstate_imx_lpi2c = {
    .name = TYPE_IMX_LPI2C,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mcr, IMXLPI2CState),
        VMSTATE_UINT32(msr, IMXLPI2CState),
        VMSTATE_UINT32(mier, IMXLPI2CState),
        VMSTATE_UINT32(mcfgr1, IMXLPI2CState),
        VMSTATE_UINT8_ARRAY(rxfifo, IMXLPI2CState, RXFIFO_LEN),
        VMSTATE_UINT32(rx_head, IMXLPI2CState),
        VMSTATE_UINT32(rx_count, IMXLPI2CState),
        VMSTATE_END_OF_LIST()
    },
};

static void imx_lpi2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx_lpi2c_realize;
    device_class_set_props(dc, imx_lpi2c_properties);
    dc->vmsd = &vmstate_imx_lpi2c;
    device_class_set_legacy_reset(dc, imx_lpi2c_reset);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "NXP i.MX LPI2C master";
}

static const TypeInfo imx_lpi2c_info = {
    .name           = TYPE_IMX_LPI2C,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(IMXLPI2CState),
    .instance_init  = imx_lpi2c_init,
    .class_init     = imx_lpi2c_class_init,
};

static void imx_lpi2c_register_types(void)
{
    type_register_static(&imx_lpi2c_info);
}

type_init(imx_lpi2c_register_types)
