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
#include "hw/core/irq.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define WM8962_SOFTWARE_RESET   0x0f
#define WM8962_DEVICE_ID        0x6243
#define WM8962_NUM_REGS         0x5294  /* WM8962_MAX_REGISTER + 1 */

/*
 * THE CODEC IS THE CLOCK, SO THE CODEC KNOWS THE RATE - AND NOTHING ELSE DOES.
 *
 * On this board the SAI is a bit-clock SLAVE: the wm8962 drives BCLK and
 * LRCLK, and its rate is programmed here, over I2C. The SAI's own divider
 * registers are never even touched - they are byte-identical at 48 kHz and at
 * 16 kHz - so the rate is NOT DERIVABLE FROM THE SAI. Computing it from SAI
 * dividers would be correct for a bit-clock master, would agree with itself at
 * the one rate anyone tests, and would be a fabrication with a reference-manual
 * citation attached.
 *
 * This model was a pure register store: it accepted the rate and told nobody.
 * So the SAI clocked every stream at a hardcoded 48 kHz, and a 16 kHz stream
 * played three times too fast while the whole datapath looked perfectly
 * healthy.
 *
 * Read out of the driver (sound/soc/codecs/wm8962.c :: wm8962_hw_params), NOT
 * out of a description of it:
 *
 *   adctl3 = sr_vals[i].reg;                     // R27 (0x1B) bits [2:0]
 *   if (lrclk % 8000 == 0) adctl3 |= INT_MODE;   // R27 (0x1B) bit 4
 *
 *   sr code:  0 -> 48000 | 44100      3 -> 16000
 *             1 -> 32000             4 -> 11025 | 12000
 *             2 -> 24000 | 22050     5 ->  8000
 *                                    6 -> 96000 | 88200
 *
 * INT_MODE (set iff rate % 8000 == 0) disambiguates every pair EXCEPT code 4:
 * 11025 % 8000 and 12000 % 8000 are both non-zero, so both clear it. THAT PAIR
 * IS GENUINELY NOT DISTINGUISHABLE FROM THIS REGISTER - the information is not
 * in the device. We say so and pick the integral one, rather than inventing a
 * certainty the hardware does not have.
 */
#define WM8962_ADDITIONAL_CONTROL_3 0x1b
#define WM8962_SAMPLE_RATE_MASK     0x0007
#define WM8962_SAMPLE_RATE_INT_MODE 0x0010

OBJECT_DECLARE_SIMPLE_TYPE(Wm8962State, WM8962)

struct Wm8962State {
    I2CSlave parent_obj;

    uint16_t regs[WM8962_NUM_REGS];
    uint16_t ptr;           /* current register pointer */
    int      wphase;        /* bytes seen since I2C_START_SEND */
    int      rphase;        /* 0: next read byte is value high, 1: low */
    uint8_t  addr_hi;       /* first address byte (pending) */
    uint8_t  val_hi;        /* first value byte (pending) */

    /*
     * Carries the configured sample rate, in Hz, to whatever the codec is
     * clocking - on this board, the SAI's "codec-rate" input. It is a wire from
     * the bit-clock MASTER to its SLAVE, which is exactly what it is on
     * silicon.
     */
    qemu_irq rate_out;
    uint32_t rate;
};

/* Decode R27 (0x1B) into a sample rate, or 0 if it does not name one. */
static uint32_t wm8962_decode_rate(uint16_t adctl3)
{
    bool int_mode = adctl3 & WM8962_SAMPLE_RATE_INT_MODE;

    switch (adctl3 & WM8962_SAMPLE_RATE_MASK) {
    case 0:
        return int_mode ? 48000 : 44100;
    case 1:
        return 32000;               /* only 32000 maps here */
    case 2:
        return int_mode ? 24000 : 22050;
    case 3:
        return 16000;               /* only 16000 maps here */
    case 4:
        /* AMBIGUOUS with 11025: that bit is not in the register. */
        return 12000;
    case 5:
        return 8000;                /* only 8000 maps here */
    case 6:
        return int_mode ? 96000 : 88200;
    default:
        return 0;
    }
}

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
            uint16_t val = (s->val_hi << 8) | data;

            s->regs[s->ptr] = val;

            /*
             * The driver just told us what rate it is clocking at. Tell
             * whoever we are clocking: we are the bit-clock master, and
             * this is the only place in the machine where that number
             * exists.
             */
            if (s->ptr == WM8962_ADDITIONAL_CONTROL_3) {
                uint32_t rate = wm8962_decode_rate(val);

                if (rate && rate != s->rate) {
                    s->rate = rate;
                    qemu_set_irq(s->rate_out, rate);
                }
            }
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

static void wm8962_realize(DeviceState *dev, Error **errp)
{
    Wm8962State *s = WM8962(dev);

    /* The wire on which the bit-clock master tells its slave the rate. */
    qdev_init_gpio_out_named(dev, &s->rate_out, "rate", 1);
}

static void wm8962_reset(DeviceState *dev)
{
    Wm8962State *s = WM8962(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[WM8962_SOFTWARE_RESET] = WM8962_DEVICE_ID;
    s->ptr = 0;
    s->wphase = 0;
    s->rphase = 0;
    s->rate = 0;
}

static const VMStateDescription vmstate_wm8962 = {
    .name = TYPE_WM8962,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, Wm8962State),
        VMSTATE_UINT16_ARRAY(regs, Wm8962State, WM8962_NUM_REGS),
        VMSTATE_UINT16(ptr, Wm8962State),
        VMSTATE_INT32(wphase, Wm8962State),
        VMSTATE_INT32(rphase, Wm8962State),
        VMSTATE_UINT8(addr_hi, Wm8962State),
        VMSTATE_UINT8(val_hi, Wm8962State),
        VMSTATE_UINT32(rate, Wm8962State),
        VMSTATE_END_OF_LIST()
    },
};

static void wm8962_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    dc->desc = "Wolfson WM8962 audio codec";
    dc->realize = wm8962_realize;
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
