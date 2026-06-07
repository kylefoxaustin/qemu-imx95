/*
 * NXP i.MX 95 DPU (Display Processing Unit) — framebuffer scanout + 2D blit
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The i.MX 95 display controller is a Socionext display-controller IP (two
 * pixel pipelines plus a 2D blit engine), driven by Linux's dpu95 driver
 * (drivers/gpu/drm/imx/dpu95) through a large register file in one 4 MiB window.
 * This models two functional datapaths and leaves the rest as permission stubs:
 *
 * 1. SCANOUT (the display side). The primary plane's FetchLayer unit reads a
 *    framebuffer out of guest DRAM and the FrameGen unit times it out to a
 *    display stream:
 *      - FetchLayer (DPU+0x1d0000): BASEADDRESS(+MSB) = the FB DMA address,
 *        SOURCEBUFFERATTRIBUTES (stride+bpp), SOURCEBUFFERDIMENSION (w x h),
 *        LAYERPROPERTY (SOURCEBUFFERENABLE);
 *      - FrameGen (DPU+0x2b0000): HTCFG1/VTCFG1 (active w/h), FGINCTRL (display
 *        mode / scanning out).
 *    On each console refresh, if the FrameGen is enabled and a FetchLayer base
 *    is set, we blit that guest buffer to the QEMU console via framebuffer.c.
 *    Downstream (pixel-link / DSI / LDB / bridge) carries no pixels — it is
 *    "permission plumbing" that lets the DRM driver bind and enable scanout.
 *
 * 2. 2D BLIT (the "9"-suffixed pipeline: FetchDecode9 source -> ROP9 /
 *    BlitBlend9 / scalers -> Store9 dest writeback, sequenced by the Command
 *    Sequencer). Linux exposes it as a DRM render node (dpu95-blit.c); a
 *    userspace G2D library submits a CmdSeq HIF program that configures the
 *    pipeline and triggers Store9, and the engine writes the destination buffer
 *    back to DRAM and signals a fence via a ComCtrl SW interrupt. We decode the
 *    HIF command stream, run the configured operation, and write the result:
 *    same-format rectangular copy, constant-colour fill, and Porter-Duff
 *    alpha-blend (BlitBlend9). Scaling, rotation and format conversion land in
 *    later commits.
 *
 * The full 4 MiB MMIO is backed by a plain register store (read-back-what-was-
 * written) so the driver's many other writes (layerblend, extdst, etc.) are
 * harmless. Set IMX95_DPU_TRACE=1 to log writes into the FetchLayer/FrameGen
 * blocks and the blit command stream while refining offsets against a live boot.
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

/*
 * 2D Blit engine register map (dpu95-blit-registers.h). All offsets are within
 * the same 4 MiB DPU window. The Command Sequencer's HIF (host interface) is a
 * FIFO command port: userspace pushes a word stream the sequencer executes.
 *   - opcode top byte 0x14 => "write N words": next word is the target byte
 *     offset, followed by N values written to offset, offset+4, ... ;
 *   - opcode top byte 0x20 => sync/wait (e.g. SEQCOMPLETE_SYNC 0x20000102).
 * The program ends by writing PIXENGCFG_STORE9_TRIGGER (runs the blit) then a
 * ComCtrl SW-interrupt preset that signals the driver's completion fence.
 */
#define BLIT_CMDSEQ_HIF          0x10000   /* FIFO command port */
#define BLIT_HIF_OP_WRITE        0x14
#define BLIT_HIF_OP_SYNC         0x20

/* ComCtrl interrupt block (blit completion path; STATUS1 carries the SW IRQs) */
#define BLIT_COMCTRL_INTPRESET0  0x1014
#define BLIT_COMCTRL_INTPRESET1  0x1018
#define BLIT_COMCTRL_INTCLEAR0   0x1020
#define BLIT_COMCTRL_INTCLEAR1   0x1024
#define BLIT_COMCTRL_INTSTATUS0  0x102c
#define BLIT_COMCTRL_INTSTATUS1  0x1030
/*
 * ComCtrl SW interrupts SW0..3 are DPU internal IRQs 54..57 (dpu95.h); in the
 * second 32-bit status word (STATUS1) they are bits 54&0x1f..57&0x1f = 22..25.
 * The cmdseq fires one (INTERRUPTPRESET1) when a blit completes; the dpu95-blit
 * ISR signals the driver's fence. Each maps to a displaymix-irqsteer input line
 * (dtsi dpu interrupts: comctrl_sw0..3 -> lines 1..4).
 */
#define BLIT_SW0_BIT             22
#define DPU_NUM_BLIT_IRQ         4

/* FetchDecode9 — blit source 0 (sub-block 0x90000) */
#define BLIT_FD9_BASEADDRESS0    0x90028
#define BLIT_FD9_BASEADDRESSMSB0 0x9002c
#define BLIT_FD9_SRCBUFATTR0     0x90038   /* [15:0] stride-1; [21:16] bits/px */
#define BLIT_FD9_SRCBUFDIM0      0x9003c   /* [13:0] w-1; [29:16] h-1 */
#define BLIT_FD9_CONSTANTCOLOR0  0x90054   /* fill colour (RGBA, 8b/comp) */
#define BLIT_FD9_LAYERPROPERTY0  0x90058   /* bit31 SOURCEBUFFERENABLE */

/*
 * BlitBlend9: Porter-Duff alpha blend (sub-block 0x70000). When the pixengcfg
 * routes the source through BlitBlend9 (CLKEN set in its PIXENGCFG dynamic reg)
 * the blit blends the source (FetchDecode9) with the destination's current
 * contents: out = src*Fsrc + dst*Fdst, per channel. The per-channel blend-
 * function registers carry the source factor in [3:0] and the destination
 * factor in [11:8], using the g2d_blend_func enum (ZERO=0, ONE=1, SRC_ALPHA=2,
 * 1-SRC_ALPHA=3, DST_ALPHA=4, 1-DST_ALPHA=5). CLEAR = both ZERO -> output 0.
 */
#define BLIT_PIXENGCFG_BLEND9_DYN 0x71008
#define BLIT_BLEND9_COLORFUNC    0x70018   /* red; green/blue mirror it */
#define BLIT_BLEND9_ALPHAFUNC    0x70024
#define BLIT_BLEND9_CLKEN(v)     (((v) >> 24) & 0x3)   /* PIXENGCFG CLKEN */
#define BLIT_BLEND_SRC_FUNC(v)   ((v) & 0xf)
#define BLIT_BLEND_DST_FUNC(v)   (((v) >> 8) & 0xf)

/* Store9 — blit destination (sub-block 0xe0000) + the trigger */
#define BLIT_STORE9_BASEADDRESS0    0xe0020
#define BLIT_STORE9_BASEADDRESSMSB0 0xe0024
#define BLIT_STORE9_DSTBUFATTR0     0xe0030   /* [15:0] stride-1; [21:16] b/px */
#define BLIT_STORE9_DSTBUFDIM       0xe0054   /* [13:0] w-1; [29:16] h-1 */
#define BLIT_STORE9_TRIGGER         0xe1014   /* PIXENGCFG_STORE9_TRIGGER */

#define BLIT_SRCBUF_ENABLE       (1u << 31)
#define BLIT_ATTR_STRIDE(v)      (((v) & 0xffff) + 1)        /* bytes (n-1) */
/*
 * BITSPERPIXEL lives at different bit positions in the two units: the
 * FetchDecode9 source (fetchunit layout, dpu95-fetchunit.h) puts it at [21:16];
 * the Store9 destination puts it at [31:24] (observed against live libg2d-dpu
 * cmdlists). Same STRIDE [15:0] = bytes-1 in both.
 */
#define BLIT_FETCH_BPP(v)        (((v) >> 16) & 0x3f)        /* source */
#define BLIT_STORE_BPP(v)        (((v) >> 24) & 0xff)        /* destination */
#define BLIT_DIM_W(v)            (((v) & 0x3fff) + 1)
#define BLIT_DIM_H(v)            ((((v) >> 16) & 0x3fff) + 1)

struct IMX95DPUState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq_out[DPU_NUM_IRQ_OUT];  /* -> displaymix irqsteer input lines */
    QemuConsole *con;
    uint32_t *regs;                 /* full 4 MiB register-file backing */
    MemoryRegionSection fbsection;
    bool invalidate;
    bool trace;
    bool pipe_trace;
    uint64_t fb_base;
    int rows;
    int src_width;

    /* stream-0 display interrupt block (n=0) + the frame tick */
    uint32_t int_status;
    uint32_t int_enable;
    uint32_t frame_index;       /* HW vblank counter (FGTIMESTAMP) */
    QEMUTimer *vblank_timer;

    /* 2D blit engine: CmdSeq HIF decoder state + ComCtrl completion status */
    uint32_t hif_count;         /* words still to consume for a write opcode */
    uint32_t hif_off;           /* current target offset for the burst write */
    bool     hif_have_off;      /* next HIF word is the target offset */
    uint32_t comctrl_status0;
    uint32_t comctrl_status1;   /* SW interrupts (blit completion) live here */
    qemu_irq comctrl_irq[DPU_NUM_BLIT_IRQ];   /* -> irqsteer lines 1..4 */
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

/* ---- 2D blit engine ------------------------------------------------------ */

/*
 * Run the configured blit. If the source layer (FetchDecode9) is enabled with a
 * base address we copy the source rectangle to the destination (Store9);
 * otherwise we treat it as a constant-colour fill (FetchDecode9 CONSTANTCOLOR0).
 * First cut handles a same-format rectangular copy + fill honouring the source/
 * destination strides; ROP, alpha-blend and scaling come in later commits.
 */
/* Porter-Duff blend factor (g2d_blend_func enum) as a numerator over 255. */
static uint32_t blit_blend_factor(uint32_t f, uint8_t sa, uint8_t da)
{
    switch (f & 0x7) {
    case 0:  return 0;          /* G2D_ZERO */
    case 1:  return 255;        /* G2D_ONE */
    case 2:  return sa;         /* G2D_SRC_ALPHA */
    case 3:  return 255 - sa;   /* G2D_ONE_MINUS_SRC_ALPHA */
    case 4:  return da;         /* G2D_DST_ALPHA */
    case 5:  return 255 - da;   /* G2D_ONE_MINUS_DST_ALPHA */
    default: return 0;
    }
}

static void imx95_blit_run(IMX95DPUState *s)
{
    uint32_t dattr = dpu_r(s, BLIT_STORE9_DSTBUFATTR0);
    uint32_t ddim  = dpu_r(s, BLIT_STORE9_DSTBUFDIM);
    uint64_t dbase = dpu_r(s, BLIT_STORE9_BASEADDRESS0) |
                     ((uint64_t)dpu_r(s, BLIT_STORE9_BASEADDRESSMSB0) << 32);
    uint32_t dstride = BLIT_ATTR_STRIDE(dattr);
    uint32_t dbpp = BLIT_STORE_BPP(dattr) ? BLIT_STORE_BPP(dattr) : 32;
    uint32_t dbytes = dbpp / 8;
    uint32_t w = BLIT_DIM_W(ddim);
    uint32_t h = BLIT_DIM_H(ddim);

    uint32_t sattr = dpu_r(s, BLIT_FD9_SRCBUFATTR0);
    uint32_t sdim  = dpu_r(s, BLIT_FD9_SRCBUFDIM0);
    uint32_t sprop = dpu_r(s, BLIT_FD9_LAYERPROPERTY0);
    uint64_t sbase = dpu_r(s, BLIT_FD9_BASEADDRESS0) |
                     ((uint64_t)dpu_r(s, BLIT_FD9_BASEADDRESSMSB0) << 32);
    uint32_t sstride = BLIT_ATTR_STRIDE(sattr);
    bool copy = (sprop & BLIT_SRCBUF_ENABLE) && sbase != 0;
    /* BlitBlend9 in the pipeline (its pixengcfg clock on) => alpha blend. */
    bool blend = copy && dbpp == 32 &&
        BLIT_BLEND9_CLKEN(dpu_r(s, BLIT_PIXENGCFG_BLEND9_DYN)) != 0;

    if (dbase == 0 || w == 0 || h == 0 || dbytes == 0 || dbytes > 4) {
        if (s->trace) {
            qemu_log("imx95-dpu: blit skipped (dbase=0x%" PRIx64 " %ux%u "
                     "dbpp=%u)\n", dbase, w, h, dbpp);
        }
        return;
    }

    if (copy) {
        /*
         * First cut: only a 1:1 same-geometry copy. Scaling, rotation and
         * format conversion (src/dst dimensions or bpp differ) are not modelled
         * yet — skip them rather than corrupt the destination.
         */
        if (BLIT_DIM_W(sdim) != w || BLIT_DIM_H(sdim) != h ||
            BLIT_FETCH_BPP(sattr) != dbpp) {
            if (s->trace) {
                qemu_log("imx95-dpu: blit unsupported transform "
                         "(src %ux%u/%ubpp -> dst %ux%u/%ubpp)\n",
                         BLIT_DIM_W(sdim), BLIT_DIM_H(sdim),
                         BLIT_FETCH_BPP(sattr), w, h, dbpp);
            }
            return;
        }
        if (blend) {
            /*
             * Porter-Duff blend of the source over the destination's current
             * contents (RGBA8888, byte order R,G,B,A). CLEAR (both factors
             * ZERO) -> 0; SRC (ONE/ZERO) is a copy; SRC_OVER blends by alpha.
             */
            uint32_t cf = dpu_r(s, BLIT_BLEND9_COLORFUNC);
            uint32_t af = dpu_r(s, BLIT_BLEND9_ALPHAFUNC);
            uint32_t csf = BLIT_BLEND_SRC_FUNC(cf);
            uint32_t cdf = BLIT_BLEND_DST_FUNC(cf);
            uint32_t asf = BLIT_BLEND_SRC_FUNC(af);
            uint32_t adf = BLIT_BLEND_DST_FUNC(af);
            g_autofree uint8_t *srow = g_malloc(w * 4);
            g_autofree uint8_t *drow = g_malloc(w * 4);

            for (uint32_t y = 0; y < h; y++) {
                cpu_physical_memory_read(sbase + (uint64_t)y * sstride, srow,
                                         w * 4);
                cpu_physical_memory_read(dbase + (uint64_t)y * dstride, drow,
                                         w * 4);
                for (uint32_t x = 0; x < w; x++) {
                    uint8_t *sp = srow + x * 4, *dp = drow + x * 4;
                    uint8_t sa = sp[3], da = dp[3];
                    uint32_t v;
                    for (int c = 0; c < 3; c++) {
                        v = (sp[c] * blit_blend_factor(csf, sa, da) +
                             dp[c] * blit_blend_factor(cdf, sa, da)) / 255;
                        dp[c] = v > 255 ? 255 : v;
                    }
                    v = (sa * blit_blend_factor(asf, sa, da) +
                         da * blit_blend_factor(adf, sa, da)) / 255;
                    dp[3] = v > 255 ? 255 : v;
                }
                cpu_physical_memory_write(dbase + (uint64_t)y * dstride, drow,
                                          w * 4);
            }
            if (s->trace) {
                qemu_log("imx95-dpu: blit BLEND %ux%u csf=%u cdf=%u asf=%u "
                         "adf=%u dst=0x%" PRIx64 "\n", w, h, csf, cdf, asf, adf,
                         dbase);
            }
            return;
        }
        g_autofree uint8_t *row = g_malloc(w * dbytes);
        for (uint32_t y = 0; y < h; y++) {
            cpu_physical_memory_read(sbase + (uint64_t)y * sstride, row,
                                     w * dbytes);
            cpu_physical_memory_write(dbase + (uint64_t)y * dstride, row,
                                      w * dbytes);
        }
        if (s->trace) {
            qemu_log("imx95-dpu: blit COPY %ux%u %ubpp src=0x%" PRIx64
                     " dst=0x%" PRIx64 "\n", w, h, dbpp, sbase, dbase);
        }
    } else {
        uint32_t color = dpu_r(s, BLIT_FD9_CONSTANTCOLOR0);
        uint8_t pix[4] = { color & 0xff, (color >> 8) & 0xff,
                           (color >> 16) & 0xff, (color >> 24) & 0xff };
        g_autofree uint8_t *row = g_malloc(w * dbytes);
        for (uint32_t x = 0; x < w; x++) {
            memcpy(row + x * dbytes, pix, dbytes);
        }
        for (uint32_t y = 0; y < h; y++) {
            cpu_physical_memory_write(dbase + (uint64_t)y * dstride, row,
                                      w * dbytes);
        }
        if (s->trace) {
            qemu_log("imx95-dpu: blit FILL %ux%u %ubpp color=0x%08x "
                     "dst=0x%" PRIx64 "\n", w, h, dbpp, color, dbase);
        }
    }
}

/* Drive the four ComCtrl SW-interrupt irqsteer lines from STATUS1. */
static void imx95_blit_update_irq(IMX95DPUState *s)
{
    for (int i = 0; i < DPU_NUM_BLIT_IRQ; i++) {
        qemu_set_irq(s->comctrl_irq[i],
                     !!(s->comctrl_status1 & (1u << (BLIT_SW0_BIT + i))));
    }
}

/*
 * Write one blit register, applying side effects: STORE9_TRIGGER runs the blit,
 * and the ComCtrl interrupt PRESET/CLEAR registers maintain the completion
 * status the driver's fence waits on (the SW interrupt is asserted in STATUS1).
 */
static void imx95_blit_reg_write(IMX95DPUState *s, hwaddr off, uint32_t val)
{
    switch (off) {
    case BLIT_COMCTRL_INTPRESET0:
        s->comctrl_status0 |= val;
        return;
    case BLIT_COMCTRL_INTPRESET1:
        s->comctrl_status1 |= val;   /* blit done: SW interrupt asserted */
        imx95_blit_update_irq(s);
        return;
    case BLIT_COMCTRL_INTCLEAR0:
        s->comctrl_status0 &= ~val;
        return;
    case BLIT_COMCTRL_INTCLEAR1:
        s->comctrl_status1 &= ~val;   /* driver ack lowers the line */
        imx95_blit_update_irq(s);
        return;
    default:
        break;
    }

    s->regs[off >> 2] = val;
    if (off == BLIT_STORE9_TRIGGER) {
        imx95_blit_run(s);
    }
}

/*
 * Feed one word into the Command Sequencer's HIF decoder. A "write N" opcode
 * (top byte 0x14) is followed by a target offset then N values; "sync" opcodes
 * (0x20) are no-ops here because the blit runs synchronously on its trigger.
 */
static void imx95_blit_hif_word(IMX95DPUState *s, uint32_t word)
{
    if (s->hif_count == 0 && !s->hif_have_off) {
        uint32_t op = word >> 24;
        if (s->trace) {
            qemu_log("imx95-dpu: HIF op 0x%08x\n", word);
        }
        if (op == BLIT_HIF_OP_WRITE) {
            s->hif_count = word & 0xffff;   /* N values follow the offset */
            s->hif_have_off = true;
        } else if (op != BLIT_HIF_OP_SYNC && s->trace) {
            qemu_log("imx95-dpu: blit HIF unknown opcode 0x%08x\n", word);
        }
        return;
    }

    if (s->hif_have_off) {
        s->hif_off = word;                  /* target byte offset */
        s->hif_have_off = false;
        return;
    }

    if (s->trace) {
        qemu_log("imx95-dpu: HIF wr [0x%05x] = 0x%08x\n",
                 (unsigned)s->hif_off, word);
    }
    imx95_blit_reg_write(s, s->hif_off, word);
    s->hif_off += 4;
    s->hif_count--;
}

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
    if (off == BLIT_COMCTRL_INTSTATUS0) {
        return s->comctrl_status0;
    }
    if (off == BLIT_COMCTRL_INTSTATUS1) {
        return s->comctrl_status1;   /* blit-completion SW interrupt */
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
    case BLIT_CMDSEQ_HIF:
        imx95_blit_hif_word(s, (uint32_t)val);   /* command-stream port */
        return;
    case BLIT_STORE9_TRIGGER:
    case BLIT_COMCTRL_INTPRESET0:
    case BLIT_COMCTRL_INTPRESET1:
    case BLIT_COMCTRL_INTCLEAR0:
    case BLIT_COMCTRL_INTCLEAR1:
        imx95_blit_reg_write(s, off, (uint32_t)val);  /* also handles direct */
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

    /*
     * Display-pipeline write trace (IMX95_DPU_TRACE_PIPE) — used to reverse-
     * engineer the LayerBlend/ExtDst/plane routing for compositing. Covers the
     * 0xf0000.. units (constframe / fetch* / layerblend / extdst / framegen);
     * the blit blocks sit below 0xf0000 so they are not logged here.
     */
    if (s->pipe_trace && off >= 0xf0000 && off < 0x3a0000) {
        qemu_log("imx95-dpu: PIPE 0x%05x = 0x%08x\n",
                 (unsigned)off, (uint32_t)val);
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
    s->hif_count = 0;
    s->hif_off = 0;
    s->hif_have_off = false;
    s->comctrl_status0 = 0;
    s->comctrl_status1 = 0;
    imx95_dpu_update_irq(s);
    imx95_blit_update_irq(s);
    if (s->vblank_timer) {
        timer_del(s->vblank_timer);   /* armed lazily when a source is enabled */
    }
}

static void imx95_dpu_realize(DeviceState *dev, Error **errp)
{
    IMX95DPUState *s = IMX95_DPU(dev);

    s->regs = g_malloc0(IMX95_DPU_REG_SIZE);
    s->trace = getenv("IMX95_DPU_TRACE") != NULL;
    s->pipe_trace = getenv("IMX95_DPU_TRACE_PIPE") != NULL;
    s->invalidate = true;

    memory_region_init_io(&s->iomem, OBJECT(dev), &imx95_dpu_ops, s,
                          TYPE_IMX95_DPU, IMX95_DPU_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    for (int i = 0; i < DPU_NUM_IRQ_OUT; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq_out[i]);
    }
    for (int i = 0; i < DPU_NUM_BLIT_IRQ; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->comctrl_irq[i]);
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
