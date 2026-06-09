/*
 * NXP i.MX 95 ISI (Image Sensing Interface) - capture channels
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Functional capture model for the imx8-isi V4L2 driver. The ISI normally
 * pulls pixels from the MIPI CSI-2 receiver / sensor and DMAs frames to the
 * driver's queued capture buffers. There is no real sensor pixel data here (the
 * ov5640 model only answers I2C register reads), so when a channel is enabled
 * (CHNL_CTRL.CHNL_EN) the ISI synthesises a moving test pattern and DMAs it
 * into the ping-pong output buffers (CHNL_OUT_BUF1/2_ADDR_Y, the i.MX95 36-bit
 * extended high bits) at the programmed pitch, raising that channel's
 * frame-stored interrupt per frame. The driver's ISR drains the buffer to the
 * V4L2 queue, so a capture client (REQBUFS/STREAMON/DQBUF) gets real frames.
 *
 * The register layout matches the i.MX93 ISI (shared imx8-isi IP); the i.MX95
 * adds the per-buffer extended-address registers for >4 GiB DMA and exposes all
 * eight channels (each a 0x10000 block). Set ISI_DBG to trace accesses.
 */

#include "qemu/osdep.h"
#include "hw/display/imx95_isi.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"

/* Per-channel register offsets (within a channel's 0x10000 block). */
#define CHNL_CTRL               0x0000
#define   CHNL_CTRL_CHNL_EN     0x80000000  /* BIT(31) */
#define CHNL_IMG_CTRL           0x0004
#define CHNL_IMG_CFG            0x000c      /* (height << 16) | width */
#define   CHNL_IMG_CFG_W_MASK   0x00001fff
#define   CHNL_IMG_CFG_H_SHIFT  16
#define   CHNL_IMG_CFG_H_MASK   0x1fff
#define CHNL_IER                0x0010
#define CHNL_STS                0x0014
#define   CHNL_STS_FRM_STRD     0x20000000  /* BIT(29) frame stored */
#define   CHNL_STS_BUF1_ACTIVE  0x00000100  /* BIT(8) */
#define   CHNL_STS_BUF2_ACTIVE  0x00000200  /* BIT(9) */
#define CHNL_OUT_BUF1_ADDR_Y    0x0070
#define CHNL_OUT_BUF_PITCH      0x007c
#define   CHNL_OUT_BUF_PITCH_MASK 0x0000ffff
#define CHNL_OUT_BUF2_ADDR_Y    0x008c
#define CHNL_Y_BUF1_XTND_ADDR   0x00a0      /* high 4 bits, 36-bit DMA */
#define CHNL_Y_BUF2_XTND_ADDR   0x00ac

/* Index of a channel-relative offset into the flat register array. */
#define CR(s, ch, off) \
    ((s)->regs[((ch) * IMX95_ISI_CHANNEL_STRIDE + (off)) / 4])

/* ~30 fps frame cadence. */
#define FRAME_PERIOD_NS         (NANOSECONDS_PER_SECOND / 30)

static void imx95_isi_trace(const char *op, hwaddr offset, uint32_t value)
{
    if (getenv("ISI_DBG")) {
        fprintf(stderr, "[isi] %s +0x%05x = 0x%08x\n", op,
                (unsigned)offset, value);
    }
}

static void imx95_isi_update_irq(IMX95IsiState *s, int ch)
{
    uint32_t pending = CR(s, ch, CHNL_STS) & CR(s, ch, CHNL_IER);

    qemu_set_irq(s->irq[ch], pending ? 1 : 0);
}

/* The Y buffer address for ping-pong slot @buf2, combining the 36-bit high. */
static uint64_t imx95_isi_buf_addr(IMX95IsiState *s, int ch, bool buf2)
{
    uint32_t lo = buf2 ? CR(s, ch, CHNL_OUT_BUF2_ADDR_Y)
                       : CR(s, ch, CHNL_OUT_BUF1_ADDR_Y);
    uint32_t hi = buf2 ? CR(s, ch, CHNL_Y_BUF2_XTND_ADDR)
                       : CR(s, ch, CHNL_Y_BUF1_XTND_ADDR);

    return ((uint64_t)(hi & 0xf) << 32) | lo;
}

/*
 * Generate one moving test-pattern frame into the ping-pong output buffer the
 * hardware would be filling, then raise the frame-stored interrupt.
 *
 * The mxc-isi driver double-buffers: BUF1 (0x70) and BUF2 (0x8c). Frame N is
 * written to BUF1 on even N and BUF2 on odd N. The ISR reads CHNL_STS to learn
 * which buffer just completed: the ACTIVE bit names the *next* buffer the
 * hardware switches to, so after filling BUF1 we flag BUF2 active, and after
 * BUF2 we flag BUF1 active. The driver re-programs the just-freed slot address
 * each IRQ, so we re-read the buffer address from the register every frame.
 */
static void imx95_isi_frame_tick(void *opaque)
{
    IMX95IsiChan *c = opaque;
    IMX95IsiState *s = c->isi;
    int ch = c->index;
    uint32_t cfg = CR(s, ch, CHNL_IMG_CFG);
    uint32_t width = cfg & CHNL_IMG_CFG_W_MASK;
    uint32_t height = (cfg >> CHNL_IMG_CFG_H_SHIFT) & CHNL_IMG_CFG_H_MASK;
    uint32_t pitch = CR(s, ch, CHNL_OUT_BUF_PITCH) & CHNL_OUT_BUF_PITCH_MASK;
    bool buf2 = c->frame & 1;
    uint64_t buf = imx95_isi_buf_addr(s, ch, buf2);
    uint32_t bpp, x, y;
    g_autofree uint8_t *line = NULL;

    if (!(CR(s, ch, CHNL_CTRL) & CHNL_CTRL_CHNL_EN)) {
        return;
    }
    if (buf && width && height && pitch) {
        bpp = pitch / width ? pitch / width : 4;
        line = g_malloc((size_t)width * bpp);
        for (y = 0; y < height; y++) {
            for (x = 0; x < width; x++) {
                /* moving diagonal gradient so consecutive frames differ */
                uint32_t v = (x + y + c->frame * 4) & 0xff;
                uint32_t px = 0xff000000u | (v << 16) | (v << 8) | v;
                memcpy(line + (size_t)x * bpp, &px, bpp < 4 ? bpp : 4);
            }
            dma_memory_write(&address_space_memory, buf + (uint64_t)y * pitch,
                             line, (size_t)width * bpp, MEMTXATTRS_UNSPECIFIED);
        }
    }

    /* Flag the buffer the hardware switches to next, then set frame-stored. */
    CR(s, ch, CHNL_STS) &= ~(CHNL_STS_BUF1_ACTIVE | CHNL_STS_BUF2_ACTIVE);
    CR(s, ch, CHNL_STS) |= buf2 ? CHNL_STS_BUF1_ACTIVE : CHNL_STS_BUF2_ACTIVE;
    CR(s, ch, CHNL_STS) |= CHNL_STS_FRM_STRD;
    c->frame++;
    imx95_isi_update_irq(s, ch);

    timer_mod(c->frame_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FRAME_PERIOD_NS);
}

static uint64_t imx95_isi_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX95IsiState *s = opaque;
    uint32_t val = s->regs[offset / 4];

    imx95_isi_trace("rd", offset, val);
    return val;
}

static void imx95_isi_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    IMX95IsiState *s = opaque;
    int ch = offset / IMX95_ISI_CHANNEL_STRIDE;
    hwaddr coff = offset % IMX95_ISI_CHANNEL_STRIDE;
    uint32_t old;

    imx95_isi_trace("wr", offset, (uint32_t)value);

    if (ch >= IMX95_ISI_NUM_CHANNELS) {
        return;
    }

    if (coff == CHNL_STS) {
        /* Write-1-to-clear status; drop the IRQ when the driver acks. */
        s->regs[offset / 4] &= ~(uint32_t)value;
        imx95_isi_update_irq(s, ch);
        return;
    }

    old = s->regs[offset / 4];
    s->regs[offset / 4] = value;

    if (coff == CHNL_CTRL) {
        IMX95IsiChan *c = &s->chan[ch];

        if ((value & CHNL_CTRL_CHNL_EN) && !(old & CHNL_CTRL_CHNL_EN)) {
            /* Stream on: start delivering frames on this channel. */
            c->frame = 0;
            timer_mod(c->frame_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FRAME_PERIOD_NS);
        } else if (!(value & CHNL_CTRL_CHNL_EN)) {
            timer_del(c->frame_timer);
            qemu_set_irq(s->irq[ch], 0);
        }
    } else if (coff == CHNL_IER) {
        imx95_isi_update_irq(s, ch);
    }
}

static const MemoryRegionOps imx95_isi_ops = {
    .read = imx95_isi_read,
    .write = imx95_isi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx95_isi_reset(DeviceState *dev)
{
    IMX95IsiState *s = IMX95_ISI(dev);
    int i;

    memset(s->regs, 0, sizeof(s->regs));
    for (i = 0; i < IMX95_ISI_NUM_CHANNELS; i++) {
        timer_del(s->chan[i].frame_timer);
        s->chan[i].frame = 0;
        qemu_set_irq(s->irq[i], 0);
    }
}

static void imx95_isi_init(Object *obj)
{
    IMX95IsiState *s = IMX95_ISI(obj);
    int i;

    memory_region_init_io(&s->iomem, obj, &imx95_isi_ops, s,
                          TYPE_IMX95_ISI, IMX95_ISI_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    for (i = 0; i < IMX95_ISI_NUM_CHANNELS; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
}

static void imx95_isi_realize(DeviceState *dev, Error **errp)
{
    IMX95IsiState *s = IMX95_ISI(dev);
    int i;

    for (i = 0; i < IMX95_ISI_NUM_CHANNELS; i++) {
        s->chan[i].isi = s;
        s->chan[i].index = i;
        s->chan[i].frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                              imx95_isi_frame_tick,
                                              &s->chan[i]);
    }
}

static const VMStateDescription vmstate_imx95_isi = {
    .name = TYPE_IMX95_ISI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX95IsiState, IMX95_ISI_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx95_isi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "i.MX 95 ISI (Image Sensing Interface)";
    dc->realize = imx95_isi_realize;
    device_class_set_legacy_reset(dc, imx95_isi_reset);
    dc->vmsd = &vmstate_imx95_isi;
}

static const TypeInfo imx95_isi_types[] = {
    {
        .name           = TYPE_IMX95_ISI,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(IMX95IsiState),
        .instance_init  = imx95_isi_init,
        .class_init     = imx95_isi_class_init,
    },
};

DEFINE_TYPES(imx95_isi_types)
