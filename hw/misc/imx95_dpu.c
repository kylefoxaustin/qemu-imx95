/*
 * NXP i.MX 95 DPU (Display Processing Unit) — framebuffer scanout
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The i.MX 95 display controller is a Vivante-class DPU. Linux's dpu95 driver
 * (drivers/gpu/drm/imx/dpu95) drives it through a large register file; the only
 * part we need for *pixels* is the scanout: the primary plane's FetchLayer unit
 * reads a framebuffer out of guest DRAM, and the FrameGen unit times it out to a
 * display stream. Everything downstream (pixel-link / DSI / LDB / bridge) carries
 * no pixels in emulation — it is "permission plumbing" that lets the DRM driver
 * bind and enable scanout (those blocks stay read-0 stubs in the machine).
 *
 * What this models, from the dpu95 driver register map:
 *   - the blit-engine command-sequencer status poll (probe-time; the driver
 *     busy-waits CMDSEQ_STATUS.IDLE + FIFOSPACE>=192 with no timeout, so a
 *     0-returning stub would hang wait_for_device_probe());
 *   - FetchLayer (DPU+0x1d0000): BASEADDRESS(+MSB) = the FB DMA address,
 *     SOURCEBUFFERATTRIBUTES (stride+bpp), SOURCEBUFFERDIMENSION (w x h),
 *     LAYERPROPERTY (SOURCEBUFFERENABLE);
 *   - FrameGen (DPU+0x2b0000): HTCFG1/VTCFG1 (active w/h), FGINCTRL (display
 *     mode / scanning out).
 * On each console refresh, if the FrameGen is enabled and a FetchLayer base is
 * set, we blit that guest buffer to the QEMU console via framebuffer.c.
 *
 * The full 4 MiB MMIO is backed by a plain register store (read-back-what-was-
 * written) so the driver's many other writes (layerblend, extdst, etc.) are
 * harmless. Set IMX95_DPU_TRACE=1 to log writes into the FetchLayer/FrameGen
 * blocks while refining the offsets against a live boot.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qom/object.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "hw/display/framebuffer.h"
#include "system/address-spaces.h"
#include "exec/cpu-common.h"
#include "qemu/timer.h"

#define TYPE_IMX95_DPU "imx95.dpu"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95DPUState, IMX95_DPU)

#define IMX95_DPU_REG_SIZE 0x400000

/* sub-block bases within the DPU MMIO (dpu95-core.c offset tables, stream 0) */
#define DPU_FETCHLAYER0   0x1d0000   /* primary-plane fetch unit */
#define DPU_FRAMEGEN0     0x2b0000   /* frame generator */
#define DPU_BLOCK_SIZE    0x1000

/*
 * FetchLayer registers (dpu95-fetchunit.h: reg + sub_id*0x38 + reg_offset1).
 * FetchLayer's reg_offset1 = 0x18 (confirmed by tracing a live modeset: the
 * BASEADDRESS write lands at FetchLayer+0x18, LAYERPROPERTY at +0x48).
 */
#define FL_REG_OFFSET1            0x18
#define FL_BASEADDRESS            (0x00 + FL_REG_OFFSET1)  /* 0x18: FB addr lo */
#define FL_BASEADDRESSMSB         (0x04 + FL_REG_OFFSET1)  /* 0x1c: FB addr hi */
#define FL_SOURCEBUFFERATTRIBUTES (0x10 + FL_REG_OFFSET1)  /* 0x28: stride+bpp */
#define FL_SOURCEBUFFERDIMENSION  (0x14 + FL_REG_OFFSET1)  /* 0x2c: w | h<<16 */
#define FL_LAYERPROPERTY          (0x30 + FL_REG_OFFSET1)  /* 0x48: enable */
#define FL_SOURCEBUFFERENABLE     (1u << 31)
/* SOURCEBUFFERATTRIBUTES: STRIDE = (field & 0xffff) + 1 (bytes); BPP in [21:16] */
#define FL_ATTR_STRIDE(v)         (((v) & 0xffff) + 1)

/* FrameGen registers (dpu95-framegen.c) */
#define FG_HTCFG1   0x0c   /* HACT (active width)  in [13:0] */
#define FG_VTCFG1   0x14   /* VACT (active height) in [13:0] */
#define FG_FGINCTRL 0x80   /* FGDM[2:0] display mode; non-zero => scanning out */
#define FG_FGENABLE 0x8c   /* FGEN (bit0): FrameGen running */
#define FG_FGEN     (1u << 0)
#define FG_FGCHSTAT 0x9c   /* channel status: PRIMSYNCSTAT (bit8) = primary synced */
#define FG_PRIMSYNCSTAT (1u << 8)
#define FG_FGTIMESTAMP 0x98   /* FRAMEINDEX in [..:14] - the HW vblank counter */
#define FG_FRAMEINDEX_SHIFT 14

/* blit-engine command-sequencer status (probe-time poll) */
#define DPU_CMDSEQ_STATUS           0x1019c
#define DPU_CMDSEQ_STATUS_IDLE      0x40000000u
#define DPU_CMDSEQ_STATUS_FIFOSPACE 0x0001ffffu

/*
 * Stream-0 display interrupt-status block (dpu95-core.c: disp_irq0_reg =
 * dpu_base + 0x381000). Register n covers IRQs [n*32, n*32+32); the four
 * sources the dpu95 CRTC waits on during an atomic commit all live in n=0:
 *   bit 3  EXTDST0_SHDLOAD        bit 18 DISENGCFG_SHDLOAD0
 *   bit 15 DOMAINBLEND0_SHDLOAD   bit 19 DISENGCFG_FRAMECOMPLETE0 (vblank)
 * Each enabled source drives a dedicated displaymix-irqsteer input line (the
 * dpu node's interrupts map index->line: 3->64, 15->70, 18->73, 19->74),
 * which the irqsteer funnels onto GIC SPI 215. The driver acks by writing
 * INTERRUPTCLEAR; handle_level_irq masks via INTERRUPTENABLE.
 */
#define DPU_DISP_IRQ0_BASE      0x381000
#define DPU_INT_ENABLE0         (DPU_DISP_IRQ0_BASE + 0x08)
#define DPU_INT_PRESET0         (DPU_DISP_IRQ0_BASE + 0x14)
#define DPU_INT_CLEAR0          (DPU_DISP_IRQ0_BASE + 0x20)
#define DPU_INT_STATUS0         (DPU_DISP_IRQ0_BASE + 0x2c)

#define DPU_IRQ_EXTDST0_SHDLOAD      (1u << 3)
#define DPU_IRQ_DOMAINBLEND0_SHDLOAD (1u << 15)
#define DPU_IRQ_DISENGCFG_SHDLOAD0   (1u << 18)
#define DPU_IRQ_DISENGCFG_FRAMECOMP0 (1u << 19)   /* vblank */
#define DPU_IRQ_SOURCES (DPU_IRQ_EXTDST0_SHDLOAD | DPU_IRQ_DOMAINBLEND0_SHDLOAD |\
                         DPU_IRQ_DISENGCFG_SHDLOAD0 | DPU_IRQ_DISENGCFG_FRAMECOMP0)
#define DPU_NUM_IRQ_OUT  4   /* one irqsteer line per modelled source */

/* ~60 Hz frame tick that drives the shadow-load + frame-complete interrupts. */
#define DPU_VBLANK_PERIOD_NS  16666667

struct IMX95DPUState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq_out[DPU_NUM_IRQ_OUT];  /* -> displaymix irqsteer input lines */
    QemuConsole *con;
    uint32_t *regs;                 /* full 4 MiB register-file backing */
    MemoryRegionSection fbsection;
    bool invalidate;
    bool trace;
    uint64_t fb_base;
    int rows;
    int src_width;

    /* stream-0 display interrupt block (n=0) + the frame tick */
    uint32_t int_status;
    uint32_t int_enable;
    uint32_t frame_index;       /* HW vblank counter (FGTIMESTAMP) */
    QEMUTimer *vblank_timer;
};

/* Drive each modelled source's irqsteer line from (status & enable). */
static void imx95_dpu_update_irq(IMX95DPUState *s)
{
    uint32_t active = s->int_status & s->int_enable;

    qemu_set_irq(s->irq_out[0], !!(active & DPU_IRQ_EXTDST0_SHDLOAD));
    qemu_set_irq(s->irq_out[1], !!(active & DPU_IRQ_DOMAINBLEND0_SHDLOAD));
    qemu_set_irq(s->irq_out[2], !!(active & DPU_IRQ_DISENGCFG_SHDLOAD0));
    qemu_set_irq(s->irq_out[3], !!(active & DPU_IRQ_DISENGCFG_FRAMECOMP0));
}

/*
 * Re-arm (or stop) the frame tick. It only needs to run while one of our
 * sources is enabled — i.e. during a modeset or while DRM holds a vblank
 * reference. When DRM disables vblank on idle the tick stops, so a quiescent
 * display costs nothing (and doesn't steal guest cycles from the rest of boot).
 */
static void imx95_dpu_arm_vblank(IMX95DPUState *s)
{
    if (s->int_enable & DPU_IRQ_SOURCES) {
        timer_mod(s->vblank_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + DPU_VBLANK_PERIOD_NS);
    } else {
        timer_del(s->vblank_timer);
    }
}

/*
 * Frame tick: raise the status for whichever of our sources the driver has
 * enabled. The driver acks each (INTERRUPTCLEAR) inside handle_level_irq, so a
 * source re-fires once per tick while it stays enabled — a steady ~60 Hz
 * vblank plus the shadow-load completions an atomic commit blocks on. Without
 * this the dpu95 commit falls through its ~10 s flip_done/SHDLD timeouts.
 */
static void imx95_dpu_vblank_tick(void *opaque)
{
    IMX95DPUState *s = opaque;

    s->frame_index++;   /* advance the HW vblank counter (FGTIMESTAMP) */
    s->int_status |= s->int_enable & DPU_IRQ_SOURCES;
    imx95_dpu_update_irq(s);
    imx95_dpu_arm_vblank(s);
}

static inline uint32_t dpu_r(IMX95DPUState *s, hwaddr off)
{
    return s->regs[off >> 2];
}

static inline bool in_block(hwaddr off, hwaddr base)
{
    return off >= base && off < base + DPU_BLOCK_SIZE;
}

/* Derive scanout geometry from the FrameGen + FetchLayer state. */
static bool imx95_dpu_scanout(IMX95DPUState *s, uint32_t *w, uint32_t *h,
                              uint64_t *base, uint32_t *stride)
{
    uint32_t fgdm = dpu_r(s, DPU_FRAMEGEN0 + FG_FGINCTRL) & 0x7;
    uint32_t prop = dpu_r(s, DPU_FETCHLAYER0 + FL_LAYERPROPERTY);
    uint32_t attr = dpu_r(s, DPU_FETCHLAYER0 + FL_SOURCEBUFFERATTRIBUTES);

    *w = dpu_r(s, DPU_FRAMEGEN0 + FG_HTCFG1) & 0x3fff;
    *h = dpu_r(s, DPU_FRAMEGEN0 + FG_VTCFG1) & 0x3fff;
    *base = dpu_r(s, DPU_FETCHLAYER0 + FL_BASEADDRESS) |
            ((uint64_t)dpu_r(s, DPU_FETCHLAYER0 + FL_BASEADDRESSMSB) << 32);
    *stride = FL_ATTR_STRIDE(attr);

    /* Scanning out only when the FrameGen is in a display mode, the primary
     * FetchLayer is enabled, and a framebuffer base is programmed. */
    return fgdm != 0 && (prop & FL_SOURCEBUFFERENABLE) &&
           *base != 0 && *w != 0 && *h != 0;
}

/* First cut: assume an XRGB8888 primary plane (the standard DRM fb format). */
static void imx95_dpu_draw_xrgb8888(void *opaque, uint8_t *dst,
                                    const uint8_t *src, int width, int dststep)
{
    uint32_t *d = (uint32_t *)dst;
    int i;

    for (i = 0; i < width; i++) {
        uint32_t p = ldl_le_p(src);

        *d++ = rgb_to_pixel32((p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff);
        src += 4;
    }
}

static bool imx95_dpu_gfx_update(void *opaque)
{
    IMX95DPUState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t width, height, stride;
    uint64_t base;
    int first = 0, last = 0, src_width;

    if (!imx95_dpu_scanout(s, &width, &height, &base, &stride)) {
        if (s->trace) {
            qemu_log("imx95-dpu: SCANOUT off: fgdm=0x%x prop=0x%08x "
                     "base=0x%" PRIx64 " w=%u h=%u\n",
                     dpu_r(s, DPU_FRAMEGEN0 + FG_FGINCTRL) & 0x7,
                     dpu_r(s, DPU_FETCHLAYER0 + FL_LAYERPROPERTY),
                     base, width, height);
        }
        return true;   /* no active CRTC / framebuffer yet */
    }
    if (s->trace) {
        uint8_t probe[16] = {0};
        uint32_t nz = 0, i;
        cpu_physical_memory_read(base, probe, sizeof(probe));
        for (i = 0; i < sizeof(probe); i++) {
            nz |= probe[i];
        }
        qemu_log("imx95-dpu: SCANOUT on: base=0x%" PRIx64 " %ux%u stride=%u "
                 "dram[0..16]nz=%u first=%02x%02x%02x%02x\n",
                 base, width, height, stride, nz,
                 probe[0], probe[1], probe[2], probe[3]);
    }

    if (surface_width(surface) != width || surface_height(surface) != height) {
        qemu_console_resize(s->con, width, height);
        surface = qemu_console_surface(s->con);
        s->invalidate = true;
    }

    src_width = stride;   /* source row pitch in bytes (XRGB8888) */
    if (s->invalidate || s->fb_base != base || s->src_width != src_width ||
        s->rows != (int)height) {
        framebuffer_update_memory_section(&s->fbsection, get_system_memory(),
                                          base, height, src_width);
        s->fb_base = base;
        s->src_width = src_width;
        s->rows = height;
    }

    framebuffer_update_display(surface, &s->fbsection, width, height,
                               src_width, surface_stride(surface), 0,
                               s->invalidate, imx95_dpu_draw_xrgb8888, s,
                               &first, &last);
    if (first >= 0) {
        qemu_console_update(s->con, 0, first, width, last - first + 1);
    }
    s->invalidate = false;
    return true;
}

static void imx95_dpu_invalidate(void *opaque)
{
    IMX95DPUState *s = opaque;

    s->invalidate = true;
}

static const GraphicHwOps imx95_dpu_gfx_ops = {
    .invalidate = imx95_dpu_invalidate,
    .gfx_update = imx95_dpu_gfx_update,
};

static uint64_t imx95_dpu_read(void *opaque, hwaddr off, unsigned size)
{
    IMX95DPUState *s = opaque;

    if (off == DPU_CMDSEQ_STATUS) {
        /*
         * Report the sequencer idle with a full FIFO so the dpu95 blit probe's
         * untimed IDLE + FIFOSPACE>=192 polls pass instantly (the real command
         * FIFO is never exercised in emulation). NOT the reset value
         * 0x41000080 (FIFOSPACE=128, below the threshold).
         */
        return DPU_CMDSEQ_STATUS_IDLE | DPU_CMDSEQ_STATUS_FIFOSPACE;
    }
    if (off == DPU_INT_STATUS0) {
        return s->int_status;
    }
    if (off == DPU_INT_ENABLE0) {
        return s->int_enable;
    }
    if (off == DPU_FRAMEGEN0 + FG_FGCHSTAT) {
        /*
         * Primary channel "synced up" once the FrameGen is enabled. The CRTC
         * enable path polls this (readl_poll_timeout) and would otherwise log
         * "FrameGen primary channel isn't syncup" and abort the modeset. Leave
         * P/S-FIFOEMPTY clear so the FIFO-empty warning path stays quiet.
         */
        return (dpu_r(s, DPU_FRAMEGEN0 + FG_FGENABLE) & FG_FGEN)
               ? FG_PRIMSYNCSTAT : 0;
    }
    if (off == DPU_FRAMEGEN0 + FG_FGTIMESTAMP) {
        /* HW vblank counter: drm_crtc diffs the frame index across vblanks. */
        return s->frame_index << FG_FRAMEINDEX_SHIFT;
    }
    return dpu_r(s, off);
}

static void imx95_dpu_write(void *opaque, hwaddr off, uint64_t val,
                            unsigned size)
{
    IMX95DPUState *s = opaque;
    bool scanout_reg = in_block(off, DPU_FETCHLAYER0) ||
                       in_block(off, DPU_FRAMEGEN0);

    /* stream-0 display interrupt block (n=0): enable / ack / preset */
    switch (off) {
    case DPU_INT_ENABLE0:
        s->int_enable = val;
        imx95_dpu_update_irq(s);
        imx95_dpu_arm_vblank(s);   /* start ticking once a source is enabled */
        return;
    case DPU_INT_CLEAR0:
        s->int_status &= ~(uint32_t)val;   /* W1C ack */
        imx95_dpu_update_irq(s);
        return;
    case DPU_INT_PRESET0:
        s->int_status |= (uint32_t)val;    /* software trigger */
        imx95_dpu_update_irq(s);
        return;
    default:
        break;
    }

    s->regs[off >> 2] = val;

    if (scanout_reg) {
        if (s->trace) {
            qemu_log("imx95-dpu: W %s+0x%03x = 0x%08x\n",
                     in_block(off, DPU_FRAMEGEN0) ? "FG0" : "FL0",
                     (int)(off & 0xfff), (uint32_t)val);
        }
        s->invalidate = true;
    }
}

static const MemoryRegionOps imx95_dpu_ops = {
    .read = imx95_dpu_read,
    .write = imx95_dpu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx95_dpu_reset(DeviceState *dev)
{
    IMX95DPUState *s = IMX95_DPU(dev);

    memset(s->regs, 0, IMX95_DPU_REG_SIZE);
    s->fb_base = 0;
    s->rows = 0;
    s->src_width = 0;
    s->invalidate = true;
    s->int_status = 0;
    s->int_enable = 0;
    s->frame_index = 0;
    imx95_dpu_update_irq(s);
    if (s->vblank_timer) {
        timer_del(s->vblank_timer);   /* armed lazily when a source is enabled */
    }
}

static void imx95_dpu_realize(DeviceState *dev, Error **errp)
{
    IMX95DPUState *s = IMX95_DPU(dev);

    s->regs = g_malloc0(IMX95_DPU_REG_SIZE);
    s->trace = getenv("IMX95_DPU_TRACE") != NULL;
    s->invalidate = true;

    memory_region_init_io(&s->iomem, OBJECT(dev), &imx95_dpu_ops, s,
                          TYPE_IMX95_DPU, IMX95_DPU_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    for (int i = 0; i < DPU_NUM_IRQ_OUT; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq_out[i]);
    }
    s->vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, imx95_dpu_vblank_tick, s);
    s->con = qemu_graphic_console_create(dev, 0, &imx95_dpu_gfx_ops, s);
}

static void imx95_dpu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx95_dpu_realize;
    device_class_set_legacy_reset(dc, imx95_dpu_reset);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->desc = "NXP i.MX 95 DPU";
}

static const TypeInfo imx95_dpu_info = {
    .name           = TYPE_IMX95_DPU,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(IMX95DPUState),
    .class_init     = imx95_dpu_class_init,
};

static void imx95_dpu_register_types(void)
{
    type_register_static(&imx95_dpu_info);
}

type_init(imx95_dpu_register_types)
