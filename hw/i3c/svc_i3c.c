/*
 * Silvaco (Cadence) I3C master controller - "silvaco,i3c-master-v1"
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Enough of the i.MX 9 I3C master for the Linux svc-i3c-master driver to bring
 * the bus up and talk to a legacy I2C target on it (the imx91-...-i3c DTB moves
 * the wm8962 codec onto the I3C bus as an I2C device). The driver drives a
 * transfer by writing MCTRL (START + type + address + dir), polling MSTATUS for
 * MCTRLDONE / COMPLETE, pushing bytes through MWDATAB/MWDATABE and pulling them
 * from MRDATAB (with MDATACTRL.RXCOUNT), then emitting a STOP. We bridge those
 * register ops to the QEMU I3C bus: legacy-I2C transfers go to its built-in I2C
 * bus, where the codec sits. Dynamic Address Assignment finds no I3C targets
 * (only the legacy I2C device is present), which the driver accepts.
 */

#include "qemu/osdep.h"
#include "hw/i3c/svc_i3c.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/log.h"

#define MCONFIG     0x000
#define MCONFIG_MASTER_EN   (1u << 0)
#define MCTRL       0x084
#define MSTATUS     0x088
#define MINTSET     0x090
#define MINTCLR     0x094
#define MINTMASKED  0x098
#define MERRWARN    0x09c
#define MDMACTRL    0x0a0
#define MDATACTRL   0x0ac
#define MWDATAB     0x0b0
#define MWDATABE    0x0b4
#define MWDATAH     0x0b8
#define MWDATAHE    0x0bc
#define MRDATAB     0x0c0
#define MRDATAH     0x0c8
#define MDYNADDR    0x0e4

/* MCTRL fields. */
#define MCTRL_REQUEST_MASK      0x7
#define MCTRL_REQUEST_NONE      0
#define MCTRL_REQUEST_START_ADDR 1
#define MCTRL_REQUEST_STOP      2
#define MCTRL_REQUEST_PROC_DAA  4
#define MCTRL_TYPE_I2C          (1u << 4)
#define MCTRL_DIR               (1u << 8)
#define MCTRL_ADDR(v)           (((v) >> 9) & 0x7f)
#define MCTRL_RDTERM(v)         (((v) >> 16) & 0xff)

/* MSTATUS / MINT bits. */
#define MSTATUS_STATE_IDLE      0
#define MINT_SLVSTART           (1u << 8)
#define MINT_MCTRLDONE          (1u << 9)
#define MINT_COMPLETE           (1u << 10)
#define MINT_RXPEND             (1u << 11)
#define MINT_TXNOTFULL          (1u << 12)
#define MINT_ERRWARN            (1u << 15)
#define MSTATUS_NACKED          (1u << 5)

/* MERRWARN bits. */
#define MERRWARN_NACK           (1u << 2)

/* MDATACTRL bits. */
#define MDATACTRL_FLUSHTB       (1u << 0)
#define MDATACTRL_FLUSHRB       (1u << 1)
#define MDATACTRL_RXCOUNT_SHIFT 24
#define MDATACTRL_TXFULL        (1u << 30)
#define MDATACTRL_RXEMPTY       (1u << 31)

static void svc_i3c_update_irq(SvcI3cState *s)
{
    uint32_t active = (s->mstatus | (s->merrwarn ? MINT_ERRWARN : 0)) &
                      s->mintset;

    qemu_set_irq(s->irq, !!active);
}

static void svc_i3c_start(SvcI3cState *s, uint32_t mctrl)
{
    bool is_i2c = mctrl & MCTRL_TYPE_I2C;
    bool recv = mctrl & MCTRL_DIR;
    uint8_t addr = MCTRL_ADDR(mctrl);
    uint32_t rdterm = MCTRL_RDTERM(mctrl);
    int ret;

    s->cur_i2c = is_i2c;
    ret = is_i2c ? legacy_i2c_start_transfer(s->bus, addr, recv)
                 : i3c_start_transfer(s->bus, addr, recv);

    s->mstatus |= MINT_MCTRLDONE;
    if (ret) {                          /* target did not ACK */
        s->merrwarn |= MERRWARN_NACK;
        s->mstatus |= MSTATUS_NACKED;
        return;
    }

    if (recv) {                         /* auto-read RDTERM bytes into FIFO */
        uint32_t i;

        s->rx_head = 0;
        s->rx_count = 0;
        for (i = 0; i < rdterm && i < SVC_I3C_RXFIFO; i++) {
            uint8_t b;

            if (is_i2c) {
                b = legacy_i2c_recv(s->bus);
            } else if (i3c_recv_byte(s->bus, &b)) {
                break;
            }
            s->rx[s->rx_count++] = b;
        }
        s->mstatus |= MINT_COMPLETE;
    }
}

static void svc_i3c_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    SvcI3cState *s = opaque;
    uint32_t v = value;

    switch (offset) {
    case MCTRL: {
        uint32_t req = v & MCTRL_REQUEST_MASK;

        s->regs[MCTRL >> 2] = v;
        switch (req) {
        case MCTRL_REQUEST_START_ADDR:
            svc_i3c_start(s, v);
            break;
        case MCTRL_REQUEST_STOP:
            if (s->cur_i2c) {
                legacy_i2c_end_transfer(s->bus);
            } else {
                i3c_end_transfer(s->bus);
            }
            s->mstatus |= MINT_MCTRLDONE;
            break;
        case MCTRL_REQUEST_PROC_DAA:
            /* No I3C targets on this bus: DAA completes with none found. */
            s->mstatus |= MINT_MCTRLDONE | MINT_COMPLETE;
            break;
        default:
            s->mstatus |= MINT_MCTRLDONE;
            break;
        }
        break;
    }
    case MWDATAB:
    case MWDATAH:
        if (s->cur_i2c) {
            legacy_i2c_send(s->bus, v & 0xff);
        } else {
            i3c_send_byte(s->bus, v & 0xff);
        }
        break;
    case MWDATABE:
    case MWDATAHE:
        if (s->cur_i2c) {
            legacy_i2c_send(s->bus, v & 0xff);
        } else {
            i3c_send_byte(s->bus, v & 0xff);
        }
        s->mstatus |= MINT_COMPLETE;    /* "end" byte completes the message */
        break;
    case MSTATUS:
        s->mstatus &= ~v;               /* write-1-to-clear */
        break;
    case MERRWARN:
        s->merrwarn &= ~v;              /* write-1-to-clear */
        break;
    case MINTSET:
        s->mintset |= v;
        break;
    case MINTCLR:
        s->mintset &= ~v;
        break;
    case MDATACTRL:
        if (v & MDATACTRL_FLUSHRB) {
            s->rx_count = 0;
            s->rx_head = 0;
        }
        s->regs[MDATACTRL >> 2] = v & ~(MDATACTRL_FLUSHTB | MDATACTRL_FLUSHRB);
        break;
    default:
        if ((offset >> 2) < SVC_I3C_NUM_REGS) {
            s->regs[offset >> 2] = v;
        }
        break;
    }
    svc_i3c_update_irq(s);
}

static uint64_t svc_i3c_read(void *opaque, hwaddr offset, unsigned size)
{
    SvcI3cState *s = opaque;
    uint64_t r;

    switch (offset) {
    case MSTATUS:
        /* STATE is always IDLE here; surface the sticky bits + live FIFO. */
        r = s->mstatus | MINT_TXNOTFULL |
            (s->rx_count ? MINT_RXPEND : 0);
        return r;
    case MERRWARN:
        return s->merrwarn;
    case MINTMASKED:
        return (s->mstatus | (s->merrwarn ? MINT_ERRWARN : 0)) & s->mintset;
    case MDATACTRL:
        return (s->rx_count << MDATACTRL_RXCOUNT_SHIFT) |
               (s->rx_count ? 0 : MDATACTRL_RXEMPTY);
    case MRDATAB:
    case MRDATAH:
        if (s->rx_count) {
            uint8_t b = s->rx[s->rx_head++];

            s->rx_count--;
            return b;
        }
        return 0;
    default:
        if ((offset >> 2) >= SVC_I3C_NUM_REGS) {
            return 0;
        }
        return s->regs[offset >> 2];
    }
}

static const MemoryRegionOps svc_i3c_ops = {
    .read = svc_i3c_read,
    .write = svc_i3c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void svc_i3c_reset(DeviceState *dev)
{
    SvcI3cState *s = SVC_I3C(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->mstatus = 0;
    s->merrwarn = 0;
    s->mintset = 0;
    s->rx_count = 0;
    s->rx_head = 0;
    s->cur_i2c = false;
    qemu_set_irq(s->irq, 0);
}

static void svc_i3c_realize(DeviceState *dev, Error **errp)
{
    SvcI3cState *s = SVC_I3C(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &svc_i3c_ops, s,
                          TYPE_SVC_I3C, 0x10000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->bus = i3c_init_bus(dev, "i3c");
}

static const VMStateDescription vmstate_svc_i3c = {
    .name = TYPE_SVC_I3C,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, SvcI3cState, SVC_I3C_NUM_REGS),
        VMSTATE_UINT32(mstatus, SvcI3cState),
        VMSTATE_UINT32(merrwarn, SvcI3cState),
        VMSTATE_UINT32(mintset, SvcI3cState),
        VMSTATE_UINT8_ARRAY(rx, SvcI3cState, SVC_I3C_RXFIFO),
        VMSTATE_UINT32(rx_count, SvcI3cState),
        VMSTATE_UINT32(rx_head, SvcI3cState),
        VMSTATE_BOOL(cur_i2c, SvcI3cState),
        VMSTATE_END_OF_LIST()
    },
};

static void svc_i3c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = svc_i3c_realize;
    dc->vmsd = &vmstate_svc_i3c;
    device_class_set_legacy_reset(dc, svc_i3c_reset);
    dc->desc = "Silvaco I3C master controller";
}

static const TypeInfo svc_i3c_types[] = {
    {
        .name          = TYPE_SVC_I3C,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SvcI3cState),
        .class_init    = svc_i3c_class_init,
    },
};

DEFINE_TYPES(svc_i3c_types)
