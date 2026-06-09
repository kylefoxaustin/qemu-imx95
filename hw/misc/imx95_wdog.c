/*
 * NXP i.MX 95 ULP Watchdog
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the i.MX7ULP-style WDOG (compatible "fsl,imx93-wdt") the i.MX 95 uses
 * for WDOG2..5. Two layers:
 *
 *   - U-Boot SPL's arch_cpu_init() disables WDG3/4/5 before the console: its
 *     disable_wdog() reads CS, early-exits if the enable bit is clear, else
 *     unlocks (UNLOCK -> CNT, sets CS.ULK) and waits for CS.RCS. Defaults
 *     disabled, so the first CS read returns no EN and disable_wdog() returns.
 *
 *   - When the "functional" property is set, the full Linux datapath: the
 *     imx7ulp_wdt driver unlocks via CNT, reconfigures via CS (RCS acked),
 *     pings by writing REFRESH to CNT, and when enabled (CS.EN) and not
 *     refreshed within the timeout the QEMU watchdog action fires (reset). This
 *     is enabled only for WDOG3 (the watchdog Linux owns). WDOG2 is the M33
 *     SM's own watchdog: it stays non-functional so the SM can configure and
 *     refresh it without the timer ever biting the machine.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/watchdog.h"

#define TYPE_IMX95_WDOG "imx95.wdog"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95WDogState, IMX95_WDOG)

#define IMX95_WDOG_REG_SIZE     0x10000

/* Register offsets. */
#define WDOG_CS                 0x00
#define WDOG_CNT                0x04
#define WDOG_TOVAL              0x08
#define WDOG_WIN                0x0C

/* CS bit fields. */
#define CS_EN                   0x00000080
#define CS_RCS                  0x00000400  /* reconfig complete */
#define CS_ULK                  0x00000800  /* unlocked */
#define CS_PRES                 0x00001000  /* 256x prescaler */
#define CS_CMD32EN              0x00002000  /* 32-bit unlock/refresh */

#define UNLOCK_WORD             0xD928C520u
#define REFRESH_WORD            0xB480A602u

/*
 * The i.MX 95 WDOG is clocked by the 32 kHz oscillator (not the 1 kHz LPO the
 * i.MX7ULP/93 use): the imx7ulp_wdt driver programs toval = timeout x 32768
 * (with the 256x prescaler when the timeout is large), so the model must use
 * the same rate to map toval back to wall-clock time.
 */
#define WDOG_HZ                 32768

struct IMX95WDogState {
    SysBusDevice    parent_obj;

    MemoryRegion    iomem;
    QEMUTimer       timer;
    bool            functional; /* arm a real timeout (WDOG3); else stub */

    uint32_t        cs;
    uint32_t        cnt;
    uint32_t        toval;
    uint32_t        win;
    bool            unlocked;
    bool            rcs;
};

static uint32_t wdog_rate(IMX95WDogState *s)
{
    return (s->cs & CS_PRES) ? (WDOG_HZ / 256) : WDOG_HZ;
}

static void wdog_arm(IMX95WDogState *s)
{
    uint32_t rate = wdog_rate(s);
    uint64_t ms;

    /* Only WDOG3 bites; WDOG2 (SM) and the disable-only path never arm. */
    if (!s->functional || !(s->cs & CS_EN) || rate == 0 || s->toval == 0) {
        timer_del(&s->timer);
        return;
    }
    ms = (uint64_t)s->toval * 1000 / rate;
    timer_mod(&s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ms * SCALE_MS);
}

static void wdog_expire(void *opaque)
{
    watchdog_perform_action();
}

static uint64_t imx95_wdog_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX95WDogState *s = opaque;
    uint32_t cs = s->cs;

    cs |= s->unlocked ? CS_ULK : 0;
    cs |= s->rcs ? CS_RCS : 0;

    switch (offset) {
    case WDOG_CS:
        return cs;
    case WDOG_CNT:
        /* Opaque to the driver; it only writes UNLOCK/REFRESH here. */
        return 0;
    case WDOG_TOVAL:
        return s->toval;
    case WDOG_WIN:
        return s->win;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void imx95_wdog_write(void *opaque, hwaddr offset,
                             uint64_t value, unsigned size)
{
    IMX95WDogState *s = opaque;

    switch (offset) {
    case WDOG_CS:
        /*
         * A CS write reconfigures the watchdog. Acknowledge by setting RCS
         * (the "wait for reconfig" loop in both U-Boot's disable_wdog() and
         * the Linux driver then exits) and re-lock. Honour EN/PRES so a
         * functional WDOG3 arms its timeout.
         */
        if (s->unlocked) {
            s->cs = value & ~(CS_ULK | CS_RCS);
            s->unlocked = false;    /* writing CS re-locks */
            s->rcs = true;
            wdog_arm(s);
        }
        break;

    case WDOG_CNT:
        s->cnt = value;
        if ((uint32_t)value == UNLOCK_WORD) {
            s->unlocked = true;
            s->rcs = false;
        } else if ((uint32_t)value == REFRESH_WORD) {
            wdog_arm(s);            /* ping: restart the timeout */
        }
        break;

    case WDOG_TOVAL:
        if (s->unlocked) {
            s->toval = value;
        }
        break;

    case WDOG_WIN:
        if (s->unlocked) {
            s->win = value;
        }
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx
                      " value 0x%" PRIx64 "\n",
                      __func__, offset, value);
        break;
    }
}

static const MemoryRegionOps imx95_wdog_ops = {
    .read = imx95_wdog_read,
    .write = imx95_wdog_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx95_wdog_reset(DeviceState *dev)
{
    IMX95WDogState *s = IMX95_WDOG(dev);

    /*
     * Default disabled (no bootloader started it under QEMU) but with CMD32EN
     * set so the driver uses the single 32-bit UNLOCK/REFRESH sequence. CS.EN
     * clear makes U-Boot's disable_wdog() early-exit.
     */
    s->cs = CS_CMD32EN;
    s->cnt = 0;
    s->toval = 0;
    s->win = 0;
    s->unlocked = false;
    s->rcs = false;
    timer_del(&s->timer);
}

static void imx95_wdog_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    IMX95WDogState *s = IMX95_WDOG(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &imx95_wdog_ops, s,
                          TYPE_IMX95_WDOG, IMX95_WDOG_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, wdog_expire, s);
}

static const VMStateDescription vmstate_imx95_wdog = {
    .name = TYPE_IMX95_WDOG,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cs, IMX95WDogState),
        VMSTATE_UINT32(cnt, IMX95WDogState),
        VMSTATE_UINT32(toval, IMX95WDogState),
        VMSTATE_UINT32(win, IMX95WDogState),
        VMSTATE_BOOL(unlocked, IMX95WDogState),
        VMSTATE_BOOL(rcs, IMX95WDogState),
        VMSTATE_TIMER(timer, IMX95WDogState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property imx95_wdog_properties[] = {
    DEFINE_PROP_BOOL("functional", IMX95WDogState, functional, false),
};

static void imx95_wdog_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx95_wdog_realize;
    dc->vmsd = &vmstate_imx95_wdog;
    device_class_set_legacy_reset(dc, imx95_wdog_reset);
    device_class_set_props(dc, imx95_wdog_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "NXP i.MX 95 ULP watchdog";
}

static const TypeInfo imx95_wdog_info = {
    .name           = TYPE_IMX95_WDOG,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(IMX95WDogState),
    .class_init     = imx95_wdog_class_init,
};

static void imx95_wdog_register_types(void)
{
    type_register_static(&imx95_wdog_info);
}

type_init(imx95_wdog_register_types)
