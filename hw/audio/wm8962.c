/*
 * Wolfson/Cirrus WM8962 audio codec (I2C, register-file model)
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The WM8962 is the i.MX93 EVK's headphone/speaker/mic codec, on the SAI3
 * audio link. This is a read-what-you-write register-file model: enough for
 * the Linux wm8962 driver to probe and the fsl-asoc-card "wm8962-audio" card
 * to register. The only value that must be exact is the device-ID register
 * (SOFTWARE_RESET == 0x6243); the driver's FLL/DC-servo handshakes happen at
 * stream time, which this model does not drive (no audio backend).
 *
 * Unlike the trivial SMBus regdev, the WM8962 uses regmap-i2c with 16-bit
 * register addresses and 16-bit big-endian values, so the wire format is:
 *   write: [addr_hi][addr_lo] [val_hi][val_lo] ...   (auto-incrementing)
 *   read:  (write [addr_hi][addr_lo]) then repeated-start read [val_hi][val_lo]
 */

#include "qemu/osdep.h"
#include "hw/audio/wm8962.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define WM8962_SOFTWARE_RESET   0x0f
#define WM8962_DEVICE_ID        0x6243
#define WM8962_NUM_REGS         0x5294  /* WM8962_MAX_REGISTER + 1 */

OBJECT_DECLARE_SIMPLE_TYPE(Wm8962State, WM8962)

struct Wm8962State {
    I2CSlave parent_obj;

    uint16_t regs[WM8962_NUM_REGS];
    uint16_t ptr;           /* current register pointer */
    int      wphase;        /* bytes seen since I2C_START_SEND */
    int      rphase;        /* 0: next read byte is value high, 1: low */
    uint8_t  addr_hi;       /* first address byte (pending) */
    uint8_t  val_hi;        /* first value byte (pending) */
};

static int wm8962_event(I2CSlave *i2c, enum i2c_event event)
{
    Wm8962State *s = WM8962(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->wphase = 0;      /* address bytes come first */
        break;
    case I2C_START_RECV:
        s->rphase = 0;      /* read uses the pointer the preceding write set */
        break;
    default:
        break;
    }
    return 0;
}

static int wm8962_send(I2CSlave *i2c, uint8_t data)
{
    Wm8962State *s = WM8962(i2c);

    if (s->wphase == 0) {
        s->addr_hi = data;
    } else if (s->wphase == 1) {
        s->ptr = (s->addr_hi << 8) | data;
    } else if (((s->wphase - 2) & 1) == 0) {
        s->val_hi = data;
    } else {
        if (s->ptr < WM8962_NUM_REGS) {
            s->regs[s->ptr] = (s->val_hi << 8) | data;
        }
        s->ptr++;
    }
    s->wphase++;
    return 0;
}

static uint8_t wm8962_recv(I2CSlave *i2c)
{
    Wm8962State *s = WM8962(i2c);
    uint16_t val = s->ptr < WM8962_NUM_REGS ? s->regs[s->ptr] : 0;

    if (s->rphase == 0) {
        s->rphase = 1;
        return val >> 8;
    }
    s->rphase = 0;
    s->ptr++;
    return val & 0xff;
}

static void wm8962_reset(DeviceState *dev)
{
    Wm8962State *s = WM8962(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[WM8962_SOFTWARE_RESET] = WM8962_DEVICE_ID;
    s->ptr = 0;
    s->wphase = 0;
    s->rphase = 0;
}

static const VMStateDescription vmstate_wm8962 = {
    .name = TYPE_WM8962,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, Wm8962State),
        VMSTATE_UINT16_ARRAY(regs, Wm8962State, WM8962_NUM_REGS),
        VMSTATE_UINT16(ptr, Wm8962State),
        VMSTATE_INT32(wphase, Wm8962State),
        VMSTATE_INT32(rphase, Wm8962State),
        VMSTATE_UINT8(addr_hi, Wm8962State),
        VMSTATE_UINT8(val_hi, Wm8962State),
        VMSTATE_END_OF_LIST()
    },
};

static void wm8962_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    dc->desc = "Wolfson WM8962 audio codec";
    dc->vmsd = &vmstate_wm8962;
    device_class_set_legacy_reset(dc, wm8962_reset);
    sc->event = wm8962_event;
    sc->recv = wm8962_recv;
    sc->send = wm8962_send;
}

static const TypeInfo wm8962_types[] = {
    {
        .name          = TYPE_WM8962,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(Wm8962State),
        .class_init    = wm8962_class_init,
    },
};

DEFINE_TYPES(wm8962_types)
