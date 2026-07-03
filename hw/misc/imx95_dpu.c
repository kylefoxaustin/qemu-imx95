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
 * 1. SCANOUT + COMPOSITING (the display side). FetchLayer units read plane
 *    framebuffers out of guest DRAM, a chain of LayerBlend units composites
 *    them, and the FrameGen times the result out:
 *      - FetchLayer (DPU+0x1d0000 / 0x1e0000): BASEADDRESS(+MSB) = FB address,
 *        SOURCEBUFFERATTRIBUTES (stride+bpp), SOURCEBUFFERDIMENSION (w x h),
 *        LAYERPROPERTY (SOURCEBUFFERENABLE);
 *      - LayerBlend (DPU+0x170000..0x1c0000): PIXENGCFG dynamic routes its two
 *        inputs (a ConstFrame / previous LayerBlend as primary, a plane as
 *        secondary), POSITION places the plane, BLENDCONTROL.ALPHA blends it;
 *      - FrameGen (DPU+0x2b0000): HTCFG1/VTCFG1 (active w/h), FGINCTRL (mode).
 *    On each console refresh we walk the LayerBlend chain bottom-up and
 *    composite the planes (primary + overlays) to the QEMU console, honouring
 *    each LayerBlend's BLENDCONTROL funcs + constant alpha (Porter-Duff; the
 *    opaque path is e2e-validated, the const-alpha path reuses the blit's
 *    g2d-validated factors). The connector chain (pixel-interleaver /
 *    pixel-link / LDB / LVDS PHY / panel) registers a DRM connector when the
 *    dtb enables it, but carries no pixels here.
 *
 * 2. 2D BLIT (the "9"-suffixed pipeline: FetchDecode9 source -> ROP9 /
 *    BlitBlend9 / scalers -> Store9 dest writeback, sequenced by the Command
 *    Sequencer). Linux exposes it as a DRM render node (dpu95-blit.c); a
 *    userspace G2D library submits a CmdSeq HIF program that configures the
 *    pipeline and triggers Store9, and the engine writes the destination buffer
 *    back to DRAM and signals a fence via a ComCtrl SW interrupt. We decode the
 *    HIF command stream, run the configured operation, and write the result:
 *    same-format copy, constant-colour fill, Porter-Duff alpha-blend
 *    (BlitBlend9), nearest-neighbour scaling (H/VScaler9), rotation (FetchRot9)
 *    and RGBA<->YUV colour conversion (CSC). qtest coverage in
 *    tests/qtest/imx95-blit-test.c (fill/copy/blend/scale, integrity oracles).
 *    ROP9 raster ops (AND/OR/XOR of source with destination) are NOT modelled:
 *    the mainline dpu95-blit driver leaves ROP9 at passthrough (ROP9_CONTROL
 *    reset = 0 = passthrough, so a copy already represents it correctly), no
 *    in-tree consumer programs a raster op, and the ROP9_CONTROL op-code
 *    encoding is not in the driver headers (RM-only) - modelling it from a
 *    guessed layout would risk mis-handling the passthrough/copy path.
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

/* sub-block bases within the DPU MMIO (dpu95-core.c offset tables) */
#define DPU_FETCHLAYER0   0x1d0000   /* primary-plane fetch unit */
#define DPU_FRAMEGEN0     0x2b0000   /* stream-0 frame generator */
#define DPU_FRAMEGEN1     0x330000   /* stream-1 frame generator (fg_ofss[1]) */
#define DPU_BLOCK_SIZE    0x1000
#define DPU_NUM_STREAMS   2
/* per-stream FrameGen base; stream 1 is the 2nd pixel pipeline / CRTC. */
#define DPU_FRAMEGEN(st)  ((st) ? DPU_FRAMEGEN1 : DPU_FRAMEGEN0)
/* a stream's LayerBlend chain roots at its own ConstFrame(s) (dpu95.h ids). */
#define DPU_LINK_IS_CF_STREAM(l, st) \
    ((st) ? ((l) == 0x10 || (l) == 0x11) : ((l) == 0x0c || (l) == 0x0d))

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

/*
 * Display compositing. The dpu95 driver builds a LayerBlend chain per CRTC:
 * ConstFrame (background) -> LayerBlend1 (secondary = primary plane) ->
 * LayerBlend2 (primary = LB1, secondary = an overlay plane) -> ... -> ExtDst ->
 * FrameGen. Each plane is fed by a FetchLayer; the LayerBlend's POSITION places
 * it and BLENDCONTROL.ALPHA blends it. We walk that chain and composite the
 * planes (opaque, matching DRM atomic plane composition; alpha-blend is TODO).
 */
#define DPU_FETCHLAYER1   0x1e0000   /* second FetchLayer = overlay plane */
#define DPU_LINK_FETCHLAYER0 0x1a    /* pixengcfg link ids (dpu95.h) */
#define DPU_LINK_FETCHLAYER1 0x1b
/*
 * FetchYUV planes (Y) + their FetchEco companions (chroma) for NV12 overlays.
 * FetchYUV register offsets differ from FetchLayer: BASEADDRESS +0x28, ATTR
 * +0x38, DIM +0x3c, LAYERPROPERTY +0x58, and its pixengcfg dynamic (+0x1008)
 * selects the FetchEco that holds the interleaved UV plane (BASEADDRESS +0x10).
 */
#define FY_BASEADDRESS    0x28
#define FY_BASEADDRESSMSB 0x2c
#define FY_ATTR           0x38       /* [15:0] stride-1 */
#define FY_DIM            0x3c       /* w-1 | (h-1)<<16 */
#define FY_LAYERPROPERTY  0x58       /* bit31 SOURCEBUFFERENABLE */
#define FY_PEC_DYNAMIC    0x1008     /* SEC_SEL[..] = the FetchEco link id */
#define FE_BASEADDRESS    0x10
#define FE_BASEADDRESSMSB 0x14
#define FE_ATTR           0x20       /* [15:0] stride-1 */
#define FE_DIM            0x24       /* chroma w-1 | (h-1)<<16 (half-res) */
/* LayerBlend per-unit regs: pixengcfg DYNAMIC at +0x1008, rest at low offs. */
#define DPU_LB_DYNAMIC    0x1008   /* PRIM_SEL[5:0] SEC_SEL[13:8] CLKEN[25:24]*/
#define DPU_LB_BLENDCTL   0x10     /* PRIM_C[3:0] SEC_C[7:4] ALPHA[23:16] */
#define DPU_LB_POSITION   0x14     /* XPOS[15:0] YPOS[31:16] */
#define DPU_LB_ALPHA(v)   (((v) >> 16) & 0xff)   /* const alpha */
#define DPU_LB_PRIM_FUNC(v) ((v) & 0xf)
#define DPU_LB_SEC_FUNC(v)  (((v) >> 4) & 0xf)
#define DPU_LB_PRIM(v)    ((v) & 0x3f)
#define DPU_LB_SEC(v)     (((v) >> 8) & 0x3f)
#define DPU_LB_CLKEN(v)   (((v) >> 24) & 0x3)
#define DPU_LINK_IS_CONSTFRAME(l) \
    ((l) == 0x0c || (l) == 0x0d || (l) == 0x10 || (l) == 0x11)
/* FetchLayer SOURCEBUFFERDIMENSION (+0x2c): w-1 [13:0], h-1 [29:16]. */
#define FL_DIM_W(v)       (((v) & 0x3fff) + 1)
#define FL_DIM_H(v)       ((((v) >> 16) & 0x3fff) + 1)

/*
 * Display H/VScaler units (dpu95-core.c hs_ofss[0]/vs_ofss[0], link ids from
 * dpu95.h). A scaled plane chains FetchUnit -> [VScaler4] -> [HScaler4] ->
 * LayerBlend: the LB secondary is the scaler link, the scaler's pixengcfg
 * DYNAMIC (+0x1008, SRC_SEL[5:0]) points at its source, and CONTROL (+0x14)
 * OUTPUT_SIZE[29:16] holds the (dst-1) line/pixel count.
 */
#define DPU_HSCALER4      0x270000
#define DPU_VSCALER4      0x280000
#define DPU_LINK_HSCALER4 0x24
#define DPU_LINK_VSCALER4 0x25
#define SC_PEC_DYNAMIC    0x1008
#define SC_CONTROL        0x14
#define SC_OUTPUT_SIZE(v) ((((v) >> 16) & 0x3fff) + 1)

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
/* stream-1 block (disp_irq2): two registers n=0,1 (ENABLE 0x08+4n, etc.) */
#define DPU_INT2_ENABLE(n)      (DPU_DISP_IRQ2_BASE + 0x08 + 4 * (n))
#define DPU_INT2_PRESET(n)      (DPU_DISP_IRQ2_BASE + 0x14 + 4 * (n))
#define DPU_INT2_CLEAR(n)       (DPU_DISP_IRQ2_BASE + 0x20 + 4 * (n))
#define DPU_INT2_STATUS(n)      (DPU_DISP_IRQ2_BASE + 0x2c + 4 * (n))

#define DPU_IRQ_EXTDST0_SHDLOAD      (1u << 3)
#define DPU_IRQ_DOMAINBLEND0_SHDLOAD (1u << 15)
#define DPU_IRQ_DISENGCFG_SHDLOAD0   (1u << 18)
#define DPU_IRQ_DISENGCFG_FRAMECOMP0 (1u << 19)   /* vblank */
#define DPU_IRQ_SOURCES (DPU_IRQ_EXTDST0_SHDLOAD | DPU_IRQ_DOMAINBLEND0_SHDLOAD |\
                         DPU_IRQ_DISENGCFG_SHDLOAD0 | DPU_IRQ_DISENGCFG_FRAMECOMP0)

/*
 * Stream-1 display interrupt block (disp_irq2_reg = dpu_base + 0x3a1000), the
 * 2nd pixel pipeline's. Its sources span two 32-bit registers: register-index n
 * has ENABLE at 0x08+4n, PRESET 0x14+4n, CLEAR 0x20+4n, STATUS 0x2c+4n
 * (dpu95.h). The four the CRTC commit waits on (global IRQ ids from dpu95.h):
 *   n=0: bit 9  EXTDST1_SHDLOAD
 *   n=1: bit 6  DOMAINBLEND1_SHDLOAD   bit 9  DISENGCFG_SHDLOAD1
 *        bit 10 DISENGCFG_FRAMECOMPLETE1 (vblank)
 * Each drives a displaymix-irqsteer line (dpu interrupts: 9->192, 38->198,
 * 41->201, 42->202), all in irqsteer group 3 -> GIC SPI 217.
 */
#define DPU_DISP_IRQ2_BASE   0x3a1000
#define DPU_IRQ2_EXTDST1_SHDLOAD      (1u << 9)    /* register n=0 */
#define DPU_IRQ2_DOMAINBLEND1_SHDLOAD (1u << 6)    /* register n=1 */
#define DPU_IRQ2_DISENGCFG_SHDLOAD1   (1u << 9)    /* register n=1 */
#define DPU_IRQ2_DISENGCFG_FRAMECOMP1 (1u << 10)   /* register n=1, vblank */
#define DPU_IRQ2_SOURCES0 (DPU_IRQ2_EXTDST1_SHDLOAD)
#define DPU_IRQ2_SOURCES1 (DPU_IRQ2_DOMAINBLEND1_SHDLOAD | \
                           DPU_IRQ2_DISENGCFG_SHDLOAD1 | \
                           DPU_IRQ2_DISENGCFG_FRAMECOMP1)

#define DPU_NUM_IRQ_OUT  4   /* one irqsteer line per stream-0 source */
#define DPU_NUM_IRQ_OUT2 4   /* stream-1 sources (lines 192/198/201/202) */

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

/*
 * Blit scaling: a transforming blit routes FetchDecode9 -> HScaler9 (0xb0000)
 * -> VScaler9 (0xc0000) -> Store9. Each scaler's pixengcfg DYNAMIC (+0x1008)
 * CLKEN [25:24] is set when it is in the pipeline; the src/dst dimension regs
 * then carry the scale ratio (we nearest-neighbour scale src dims -> dst dims).
 * (Rotation routes through FetchRot9 (0x80000) but is not modelled: libg2d does
 * it as tiled FetchRot9 strips with no single rotate flag to detect.)
 */
#define BLIT_HSCALER9_DYN   0xb1008
#define BLIT_VSCALER9_DYN   0xc1008
#define BLIT_SCALER_CLKEN(v) (((v) >> 24) & 0x3)

/*
 * Rotation / format conversion route the source through FetchRot9 (0x80000)
 * instead of FetchDecode9: the Store9 source-select (PIXENGCFG_STORE9_DYNAMIC,
 * 0xe100c) then reads the FetchRot9 link id (0x05). libg2d does a rotate as a
 * sequence of 16-row source bands (its clip window), each transposed into a
 * 16-column destination strip — we replay one strip per Store9 trigger:
 *   dst[j][clipH-1-i] = src[i][j]  (a 90-degree CW rotation of the clip band).
 * Same case with a differing dst format (e.g. RGBA -> YUYV) is a colour-convert
 * (no transpose). FetchRot9 register layout from dpu95-blit-registers.h.
 */
#define BLIT_STORE9_SRCSEL        0xe100c
#define BLIT_STORE9_SRC_FETCHROT9 0x05
#define BLIT_STORE9_SRC_CSC       0x06   /* colour-space converter output */
#define BLIT_FR9_BASEADDRESS0     0x80020
#define BLIT_FR9_BASEADDRESSMSB0  0x80024
#define BLIT_FR9_SRCBUFATTR0      0x80030   /* [15:0] stride-1; [21:16] bpp */
#define BLIT_FR9_CLIPWINDOWDIM    0x80048   /* W-1 | (H-1)<<16 (strip) */
#define BLIT_FR9_LAYERPROPERTY0   0x80050

/* Store9 — blit destination (sub-block 0xe0000) + the trigger */
#define BLIT_STORE9_BASEADDRESS0    0xe0020
#define BLIT_STORE9_BASEADDRESSMSB0 0xe0024
#define BLIT_STORE9_DSTBUFATTR0     0xe0030   /* [15:0] stride-1; [21:16] b/px */
#define BLIT_STORE9_DSTBUFDIM       0xe0054   /* [13:0] w-1; [29:16] h-1 */
#define BLIT_STORE9_TRIGGER         0xe1014   /* PIXENGCFG_STORE9_TRIGGER */
/*
 * NV12 (2-plane 4:2:0) conversion. Store9 writes plane 0 = Y (BASEADDRESS0,
 * 8bpp) and plane 1 = interleaved Cb,Cr (BASEADDRESS1, half-res). For the
 * reverse, FetchDecode9 reads Y and FetchEco9 (0xa0000) reads the chroma plane.
 */
#define BLIT_STORE9_BASEADDRESS1    0xe0038
#define BLIT_STORE9_BASEADDRESSMSB1 0xe003c
#define BLIT_STORE9_DSTBUFATTR1     0xe0040   /* UV plane stride */
#define BLIT_FE9_BASEADDRESS0       0xa0010   /* FetchEco9 chroma source base */
#define BLIT_FE9_BASEADDRESSMSB0    0xa0014
#define BLIT_FE9_SRCBUFATTR0        0xa0020   /* [15:0] stride-1 */

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

/* per-console scanout context so one gfx_ops can serve both pixel pipelines */
typedef struct {
    IMX95DPUState *s;
    int stream;
} DPUConsoleCtx;

struct IMX95DPUState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq_out[DPU_NUM_IRQ_OUT];   /* stream-0 -> irqsteer input lines */
    qemu_irq irq_out2[DPU_NUM_IRQ_OUT2]; /* stream-1 -> irqsteer input lines */
    QemuConsole *con;                    /* stream-0 (CRTC 0) scanout */
    QemuConsole *con1;                   /* stream-1 (CRTC 1) scanout */
    DPUConsoleCtx ctx[DPU_NUM_STREAMS];  /* per-console gfx_ops opaque */
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
    /* stream-1 display interrupt block (disp_irq2), registers n=0 and n=1 */
    uint32_t int_status2[2];
    uint32_t int_enable2[2];
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
    uint32_t a0 = s->int_status2[0] & s->int_enable2[0];
    uint32_t a1 = s->int_status2[1] & s->int_enable2[1];

    qemu_set_irq(s->irq_out[0], !!(active & DPU_IRQ_EXTDST0_SHDLOAD));
    qemu_set_irq(s->irq_out[1], !!(active & DPU_IRQ_DOMAINBLEND0_SHDLOAD));
    qemu_set_irq(s->irq_out[2], !!(active & DPU_IRQ_DISENGCFG_SHDLOAD0));
    qemu_set_irq(s->irq_out[3], !!(active & DPU_IRQ_DISENGCFG_FRAMECOMP0));

    /* stream 1: EXTDST1 in register n=0; the others in register n=1 */
    qemu_set_irq(s->irq_out2[0], !!(a0 & DPU_IRQ2_EXTDST1_SHDLOAD));
    qemu_set_irq(s->irq_out2[1], !!(a1 & DPU_IRQ2_DOMAINBLEND1_SHDLOAD));
    qemu_set_irq(s->irq_out2[2], !!(a1 & DPU_IRQ2_DISENGCFG_SHDLOAD1));
    qemu_set_irq(s->irq_out2[3], !!(a1 & DPU_IRQ2_DISENGCFG_FRAMECOMP1));
}

/*
 * Re-arm (or stop) the frame tick. It only needs to run while one of our
 * sources is enabled — i.e. during a modeset or while DRM holds a vblank
 * reference. When DRM disables vblank on idle the tick stops, so a quiescent
 * display costs nothing (and doesn't steal guest cycles from the rest of boot).
 */
static void imx95_dpu_arm_vblank(IMX95DPUState *s)
{
    bool want = (s->int_enable & DPU_IRQ_SOURCES) ||
                (s->int_enable2[0] & DPU_IRQ2_SOURCES0) ||
                (s->int_enable2[1] & DPU_IRQ2_SOURCES1);

    if (want) {
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
    s->int_status2[0] |= s->int_enable2[0] & DPU_IRQ2_SOURCES0;
    s->int_status2[1] |= s->int_enable2[1] & DPU_IRQ2_SOURCES1;
    imx95_dpu_update_irq(s);
    /*
     * Drive scanout from the frame tick: with -display none there is no UI
     * refresh loop, so re-composite each active stream's console here while its
     * vblank is enabled (the tick only runs then) — this keeps both pixel
     * pipelines' surfaces current for a QMP screendump of either head.
     */
    if (s->int_enable & DPU_IRQ_DISENGCFG_FRAMECOMP0) {
        qemu_console_hw_update(s->con);
    }
    if (s->int_enable2[1] & DPU_IRQ2_DISENGCFG_FRAMECOMP1) {
        qemu_console_hw_update(s->con1);
    }
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

/* Output geometry from a stream's FrameGen (the display-timing active area). */
static bool imx95_dpu_display_on(IMX95DPUState *s, hwaddr fg, uint32_t *w,
                                 uint32_t *h)
{
    uint32_t fgdm = dpu_r(s, fg + FG_FGINCTRL) & 0x7;

    *w = dpu_r(s, fg + FG_HTCFG1) & 0x3fff;
    *h = dpu_r(s, fg + FG_VTCFG1) & 0x3fff;
    return fgdm != 0 && *w != 0 && *h != 0;
}

/* pixengcfg link id -> FetchLayer sub-block base (the RGB planes we model). */
static hwaddr imx95_dpu_plane_base(uint32_t link)
{
    switch (link) {
    case DPU_LINK_FETCHLAYER0: return DPU_FETCHLAYER0;
    case DPU_LINK_FETCHLAYER1: return DPU_FETCHLAYER1;
    default:                   return 0;   /* not a FetchLayer */
    }
}

/*
 * pixengcfg link id -> FetchYUV sub-block base (NV12 luma plane). (dpu95-core.c
 * fy_ofss[], indexed by fy_ids {0,1,2,3}; link ids from dpu95.h.)
 */
static hwaddr imx95_dpu_fetchyuv_base(uint32_t link)
{
    switch (link) {
    case 0x1d: return 0x200000;   /* FETCHYUV0 */
    case 0x1f: return 0x220000;   /* FETCHYUV1 */
    case 0x21: return 0x240000;   /* FETCHYUV2 */
    case 0x1c: return 0x1f0000;   /* FETCHYUV3 */
    default:   return 0;          /* not a FetchYUV */
    }
}

/* pixengcfg link id -> FetchEco sub-block base (interleaved-UV companion). */
static hwaddr imx95_dpu_fetcheco_base(uint32_t link)
{
    switch (link) {
    case 0x1e: return 0x210000;   /* FETCHECO0 */
    case 0x20: return 0x230000;   /* FETCHECO1 */
    case 0x22: return 0x250000;   /* FETCHECO2 */
    default:   return 0;          /* not a FetchEco */
    }
}

/*
 * LayerBlend colour blend factor as a numerator over 255. ca = the LayerBlend's
 * constant alpha (BLENDCONTROL.ALPHA); sa = the secondary pixel's alpha.
 * (dpu95-layerblend.c: ZERO=0 ONE=1 SEC_ALPHA=4 1-SEC_ALPHA=5 CONST_ALPHA=6
 * 1-CONST_ALPHA=7.)
 */
static uint32_t imx95_lb_factor(uint32_t f, uint8_t ca, uint8_t sa)
{
    switch (f & 0x7) {
    case 1:  return 255;        /* ONE */
    case 4:  return sa;         /* SEC_ALPHA */
    case 5:  return 255 - sa;   /* ONE_MINUS_SEC_ALPHA */
    case 6:  return ca;         /* CONST_ALPHA */
    case 7:  return 255 - ca;   /* ONE_MINUS_CONST_ALPHA */
    default: return 0;          /* ZERO */
    }
}

/*
 * Composite one enabled FetchLayer plane onto the surface at (px,py), XRGB8888,
 * blending per its LayerBlend's BLENDCONTROL (lb = the LayerBlend base, or 0
 * for a plain opaque copy). dw/dh are the on-screen size (0 = native source
 * size); when they differ from the source the plane is nearest-neighbour
 * scaled. The surface is the standard 32bpp xRGB console fmt.
 */
static int imx95_dpu_blit_plane(IMX95DPUState *s, DisplaySurface *surface,
                                hwaddr fl, hwaddr lb, int px, int py,
                                uint32_t dw, uint32_t dh)
{
    uint32_t prop = dpu_r(s, fl + FL_LAYERPROPERTY);
    uint64_t base = dpu_r(s, fl + FL_BASEADDRESS) |
                    ((uint64_t)dpu_r(s, fl + FL_BASEADDRESSMSB) << 32);
    uint32_t stride = FL_ATTR_STRIDE(dpu_r(s, fl + FL_SOURCEBUFFERATTRIBUTES));
    uint32_t dim = dpu_r(s, fl + FL_SOURCEBUFFERDIMENSION);
    uint32_t pw = FL_DIM_W(dim), ph = FL_DIM_H(dim);
    uint32_t ow = dw ? dw : pw, oh = dh ? dh : ph;
    uint8_t *sd = surface_data(surface);
    int sstride = surface_stride(surface);
    int sw = surface_width(surface), sh = surface_height(surface);
    uint32_t bc = lb ? dpu_r(s, lb + DPU_LB_BLENDCTL) : 0;
    uint8_t ca = DPU_LB_ALPHA(bc);
    uint32_t pf = DPU_LB_PRIM_FUNC(bc), sf = DPU_LB_SEC_FUNC(bc);
    /* opaque fast path: no LB, or PRIM=ZERO + SEC=CONST_ALPHA at full alpha */
    bool opaque = lb == 0 || (pf == 0 && sf == 6 && ca == 0xff);
    g_autofree uint8_t *row = NULL;

    if (!(prop & FL_SOURCEBUFFERENABLE) || base == 0 || pw == 0 || ph == 0) {
        return 0;
    }
    row = g_malloc((size_t)pw * 4);
    for (uint32_t oy = 0; oy < oh; oy++) {
        int dy = py + (int)oy;
        uint32_t sy = (uint32_t)((uint64_t)oy * ph / oh);   /* nearest source */
        uint32_t *d;
        if (dy < 0 || dy >= sh) {
            continue;
        }
        cpu_physical_memory_read(base + (uint64_t)sy * stride, row,
                                 (size_t)pw * 4);
        d = (uint32_t *)(sd + (size_t)dy * sstride);
        for (uint32_t ox = 0; ox < ow; ox++) {
            int dx = px + (int)ox;
            uint32_t sx = (uint32_t)((uint64_t)ox * pw / ow);
            uint32_t p;
            uint8_t sr, sg, sb;
            if (dx < 0 || dx >= sw) {
                continue;
            }
            p = ldl_le_p(row + sx * 4);
            sr = (p >> 16) & 0xff; sg = (p >> 8) & 0xff; sb = p & 0xff;
            if (opaque) {
                d[dx] = rgb_to_pixel32(sr, sg, sb);
            } else {
                /*
                 * We model XRGB8888 planes (no per-pixel alpha), so each source
                 * pixel is fully opaque; the secondary-alpha funcs see 0xff and
                 * translucency comes from the constant alpha.
                 */
                uint8_t sa = 0xff;
                uint32_t fp = imx95_lb_factor(pf, ca, sa);
                uint32_t fs = imx95_lb_factor(sf, ca, sa);
                uint32_t pp = d[dx];   /* primary already in the surface */
                uint32_t r = (((pp >> 16) & 0xff) * fp + sr * fs) / 255;
                uint32_t g = (((pp >> 8) & 0xff) * fp + sg * fs) / 255;
                uint32_t b = ((pp & 0xff) * fp + sb * fs) / 255;
                d[dx] = rgb_to_pixel32(r > 255 ? 255 : r, g > 255 ? 255 : g,
                                       b > 255 ? 255 : b);
            }
        }
    }
    if (s->trace) {
        qemu_log("imx95-dpu: plane fl=0x%" HWADDR_PRIx " %ux%u->%ux%u (%d,%d) "
                 "%s a=%u\n", fl, pw, ph, ow, oh, px, py,
                 opaque ? "opaque" : "blend", ca);
    }
    return 1;
}

/* BT.601 limited-range NV12 -> RGB for one pixel. */
static inline void imx95_yuv601_to_rgb(int y, int u, int v,
                                       uint8_t *r, uint8_t *g, uint8_t *b)
{
    int c = y - 16, d = u - 128, e = v - 128;
    int rr = (298 * c + 409 * e + 128) >> 8;
    int gg = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int bb = (298 * c + 516 * d + 128) >> 8;

    *r = rr < 0 ? 0 : rr > 255 ? 255 : rr;
    *g = gg < 0 ? 0 : gg > 255 ? 255 : gg;
    *b = bb < 0 ? 0 : bb > 255 ? 255 : bb;
}

/* BT.601 limited-range RGB -> YUV for one pixel (inverse of the above). */
static inline void imx95_rgb_to_yuv601(int r, int g, int b,
                                       uint8_t *y, uint8_t *u, uint8_t *v)
{
    *y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    *u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
    *v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
}

/*
 * Composite a plane fed by a FetchYUV unit (fy) onto the surface at (px,py).
 * FetchYUV is the unit a scaled plane always uses (only FetchYUV units have
 * scalers), so it carries either NV12 (a FetchEco companion holds interleaved
 * chroma) or plain RGB (no FetchEco). dw/dh are the on-screen size (0 =
 * native); differ => nearest-neighbour scaled. Same LayerBlend blend as
 * imx95_dpu_blit_plane; pixels are fully opaque.
 */
static int imx95_dpu_blit_yuv(IMX95DPUState *s, DisplaySurface *surface,
                              hwaddr fy, hwaddr lb, int px, int py,
                              uint32_t dw, uint32_t dh)
{
    uint32_t prop = dpu_r(s, fy + FY_LAYERPROPERTY);
    uint64_t ybase = dpu_r(s, fy + FY_BASEADDRESS) |
                     ((uint64_t)dpu_r(s, fy + FY_BASEADDRESSMSB) << 32);
    uint32_t ystride = FL_ATTR_STRIDE(dpu_r(s, fy + FY_ATTR));
    uint32_t dim = dpu_r(s, fy + FY_DIM);
    uint32_t pw = (dim & 0x3fff) + 1, ph = ((dim >> 16) & 0x3fff) + 1;
    uint32_t ow = dw ? dw : pw, oh = dh ? dh : ph;
    uint32_t eco = dpu_r(s, fy + FY_PEC_DYNAMIC) & 0x3f;
    hwaddr fe = imx95_dpu_fetcheco_base(eco);
    bool yuv = fe != 0;   /* FetchEco companion => NV12; else RGB */
    uint64_t cbase = 0;
    uint32_t cstride = 0;
    uint8_t *sd = surface_data(surface);
    int sstride = surface_stride(surface);
    int sw = surface_width(surface), sh = surface_height(surface);
    uint32_t bc = lb ? dpu_r(s, lb + DPU_LB_BLENDCTL) : 0;
    uint8_t ca = DPU_LB_ALPHA(bc);
    uint32_t pf = DPU_LB_PRIM_FUNC(bc), sf = DPU_LB_SEC_FUNC(bc);
    bool opaque = lb == 0 || (pf == 0 && sf == 6 && ca == 0xff);
    g_autofree uint8_t *yrow = NULL;
    g_autofree uint8_t *crow = NULL;

    if (!(prop & FL_SOURCEBUFFERENABLE) || ybase == 0 || pw == 0 || ph == 0) {
        return 0;
    }
    if (yuv) {
        cbase = dpu_r(s, fe + FE_BASEADDRESS) |
                ((uint64_t)dpu_r(s, fe + FE_BASEADDRESSMSB) << 32);
        cstride = FL_ATTR_STRIDE(dpu_r(s, fe + FE_ATTR));
        if (cbase == 0) {
            return 0;
        }
    }
    yrow = g_malloc((size_t)pw * (yuv ? 1 : 4));
    crow = g_malloc((size_t)(pw / 2 + 1) * 2);   /* interleaved Cb,Cr (YUV) */
    for (uint32_t oy = 0; oy < oh; oy++) {
        int dy = py + (int)oy;
        uint32_t sy = (uint32_t)((uint64_t)oy * ph / oh);   /* nearest source */
        uint32_t *d;
        if (dy < 0 || dy >= sh) {
            continue;
        }
        cpu_physical_memory_read(ybase + (uint64_t)sy * ystride, yrow,
                                 (size_t)pw * (yuv ? 1 : 4));
        if (yuv) {
            cpu_physical_memory_read(cbase + (uint64_t)(sy / 2) * cstride, crow,
                                     (size_t)(pw / 2) * 2);
        }
        d = (uint32_t *)(sd + (size_t)dy * sstride);
        for (uint32_t ox = 0; ox < ow; ox++) {
            int dx = px + (int)ox;
            uint32_t sx = (uint32_t)((uint64_t)ox * pw / ow);
            uint8_t sr, sg, sb;
            if (dx < 0 || dx >= sw) {
                continue;
            }
            if (yuv) {
                uint32_t ci = (sx / 2) * 2;
                imx95_yuv601_to_rgb(yrow[sx], crow[ci], crow[ci + 1],
                                    &sr, &sg, &sb);
            } else {
                uint32_t p = ldl_le_p(yrow + sx * 4);
                sr = (p >> 16) & 0xff; sg = (p >> 8) & 0xff; sb = p & 0xff;
            }
            if (opaque) {
                d[dx] = rgb_to_pixel32(sr, sg, sb);
            } else {
                uint8_t sa = 0xff;
                uint32_t fp = imx95_lb_factor(pf, ca, sa);
                uint32_t fs = imx95_lb_factor(sf, ca, sa);
                uint32_t pp = d[dx];
                uint32_t r = (((pp >> 16) & 0xff) * fp + sr * fs) / 255;
                uint32_t g = (((pp >> 8) & 0xff) * fp + sg * fs) / 255;
                uint32_t b = ((pp & 0xff) * fp + sb * fs) / 255;
                d[dx] = rgb_to_pixel32(r > 255 ? 255 : r, g > 255 ? 255 : g,
                                       b > 255 ? 255 : b);
            }
        }
    }
    if (s->trace) {
        qemu_log("imx95-dpu: fy=0x%" HWADDR_PRIx " %s eco=0x%" HWADDR_PRIx
                 " %ux%u->%ux%u @ (%d,%d) %s a=%u\n", fy, yuv ? "nv12" : "rgb",
                 fe, pw, ph, ow, oh, px, py, opaque ? "opaque" : "blend", ca);
    }
    return 1;
}

/*
 * Resolve a LayerBlend secondary link to the fetch unit that ultimately feeds
 * it, walking through any H/VScaler in the chain (FetchUnit -> [VScaler4] ->
 * [HScaler4] -> LB). Returns the fetch-unit link id and, via dw/dh, the scaled
 * on-screen size each scaler imposes (0 = that axis is not scaled).
 */
static uint32_t imx95_dpu_resolve_chain(IMX95DPUState *s, uint32_t sec,
                                        uint32_t *dw, uint32_t *dh)
{
    *dw = 0;
    *dh = 0;
    if (sec == DPU_LINK_HSCALER4) {
        *dw = SC_OUTPUT_SIZE(dpu_r(s, DPU_HSCALER4 + SC_CONTROL));
        sec = dpu_r(s, DPU_HSCALER4 + SC_PEC_DYNAMIC) & 0x3f;
    }
    if (sec == DPU_LINK_VSCALER4) {
        *dh = SC_OUTPUT_SIZE(dpu_r(s, DPU_VSCALER4 + SC_CONTROL));
        sec = dpu_r(s, DPU_VSCALER4 + SC_PEC_DYNAMIC) & 0x3f;
    }
    return sec;   /* the fetch-unit link feeding the (possibly scaled) plane */
}

static bool imx95_dpu_gfx_update(void *opaque)
{
    DPUConsoleCtx *ctx = opaque;
    IMX95DPUState *s = ctx->s;
    int st = ctx->stream;
    QemuConsole *con = st ? s->con1 : s->con;
    DisplaySurface *surface = qemu_console_surface(con);
    hwaddr fg = DPU_FRAMEGEN(st);
    static const hwaddr lb_ofs[6] = { 0x170000, 0x180000, 0x190000,
                                      0x1a0000, 0x1b0000, 0x1c0000 };
    uint32_t w, h;
    int cur, guard, planes = 0;

    if (!imx95_dpu_display_on(s, fg, &w, &h)) {
        return true;   /* this CRTC not driving a display yet */
    }
    if (surface_width(surface) != (int)w || surface_height(surface) != (int)h) {
        qemu_console_resize(con, w, h);
        surface = qemu_console_surface(con);
    }
    /* Background (ConstFrame): clear to black; opaque planes paint over it. */
    memset(surface_data(surface), 0, (size_t)surface_stride(surface) * h);

    /*
     * Composite this stream's LayerBlend chain bottom-up: the bottom LayerBlend
     * takes a ConstFrame as its primary input and the primary plane as
     * secondary; each one above takes the previous LayerBlend's output as
     * primary and another plane as secondary. Each stream roots at its own
     * ConstFrame(s), so the chains for the two pixel pipelines stay separate.
     */
    cur = -1;
    for (int i = 0; i < 6; i++) {
        uint32_t dyn = dpu_r(s, lb_ofs[i] + DPU_LB_DYNAMIC);
        if (DPU_LB_CLKEN(dyn) &&
            DPU_LINK_IS_CF_STREAM(DPU_LB_PRIM(dyn), st)) {
            cur = i;
            break;
        }
    }
    for (guard = 0; cur >= 0 && guard < 6; guard++) {
        uint32_t dyn = dpu_r(s, lb_ofs[cur] + DPU_LB_DYNAMIC);
        uint32_t pos = dpu_r(s, lb_ofs[cur] + DPU_LB_POSITION);
        uint32_t dw, dh;
        uint32_t sec = imx95_dpu_resolve_chain(s, DPU_LB_SEC(dyn), &dw, &dh);
        hwaddr fl = imx95_dpu_plane_base(sec);
        hwaddr fy = imx95_dpu_fetchyuv_base(sec);
        int px = pos & 0xffff, py = (pos >> 16) & 0xffff;
        int next = -1;

        if (fl) {
            planes += imx95_dpu_blit_plane(s, surface, fl, lb_ofs[cur],
                                           px, py, dw, dh);
        } else if (fy) {
            planes += imx95_dpu_blit_yuv(s, surface, fy, lb_ofs[cur],
                                         px, py, dw, dh);
        }
        for (int j = 0; j < 6; j++) {
            uint32_t dj = dpu_r(s, lb_ofs[j] + DPU_LB_DYNAMIC);
            if (DPU_LB_CLKEN(dj) && DPU_LB_PRIM(dj) == 0x14 + cur) {
                next = j;
                break;
            }
        }
        cur = next;
    }

    /*
     * Fallback (stream 0 only): if no LayerBlend chain was active, scan the
     * primary directly (keeps the plain boot-logo path working regardless of LB
     * programming). Stream 1 has no boot-logo path — it stays blank until its
     * chain is programmed.
     */
    if (planes == 0 && st == 0) {
        imx95_dpu_blit_plane(s, surface, DPU_FETCHLAYER0, 0, 0, 0, 0, 0);
    }

    qemu_console_update(con, 0, 0, w, h);
    return true;
}

static void imx95_dpu_invalidate(void *opaque)
{
    DPUConsoleCtx *ctx = opaque;

    ctx->s->invalidate = true;
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

/*
 * FetchRot9 path (Store9 fed by FetchRot9): one rotated strip per trigger. The
 * clip window is a clipW x clipH source band; it is transposed 90 CW into a
 * clipH-wide x clipW-tall destination strip at the Store9 base. 32bpp only.
 */
static void imx95_blit_rotate(IMX95DPUState *s)
{
    uint32_t sattr = dpu_r(s, BLIT_FR9_SRCBUFATTR0);
    uint32_t cdim  = dpu_r(s, BLIT_FR9_CLIPWINDOWDIM);
    uint64_t sbase = dpu_r(s, BLIT_FR9_BASEADDRESS0) |
                     ((uint64_t)dpu_r(s, BLIT_FR9_BASEADDRESSMSB0) << 32);
    uint32_t sstride = BLIT_ATTR_STRIDE(sattr);
    uint32_t dattr = dpu_r(s, BLIT_STORE9_DSTBUFATTR0);
    uint64_t dbase = dpu_r(s, BLIT_STORE9_BASEADDRESS0) |
                     ((uint64_t)dpu_r(s, BLIT_STORE9_BASEADDRESSMSB0) << 32);
    uint32_t dstride = BLIT_ATTR_STRIDE(dattr);
    uint32_t cw = BLIT_DIM_W(cdim), ch = BLIT_DIM_H(cdim);
    g_autofree uint8_t *band = NULL;
    g_autofree uint8_t *drow = NULL;

    if (sbase == 0 || dbase == 0 || cw == 0 || ch == 0 ||
        BLIT_FETCH_BPP(sattr) != 32) {
        return;
    }
    band = g_malloc((size_t)cw * ch * 4);
    drow = g_malloc((size_t)ch * 4);
    for (uint32_t i = 0; i < ch; i++) {
        cpu_physical_memory_read(sbase + (uint64_t)i * sstride,
                                 band + (size_t)i * cw * 4, (size_t)cw * 4);
    }
    /* output row j (0..cw-1) is the transpose of source column j */
    for (uint32_t j = 0; j < cw; j++) {
        for (uint32_t i = 0; i < ch; i++) {
            memcpy(drow + (size_t)(ch - 1 - i) * 4,
                   band + ((size_t)i * cw + j) * 4, 4);
        }
        cpu_physical_memory_write(dbase + (uint64_t)j * dstride, drow,
                                  (size_t)ch * 4);
    }
    if (s->trace) {
        qemu_log("imx95-dpu: blit ROTATE band %ux%u src=0x%" PRIx64
                 " dst=0x%" PRIx64 "\n", cw, ch, sbase, dbase);
    }
}

/* RGBA8888 -> YUYV (4:2:2 packed, bytes Y0 U Y1 V), BT.601. */
static void imx95_blit_rgba_to_yuyv(uint64_t sbase,
                                    uint32_t sstride, uint64_t dbase,
                                    uint32_t dstride, uint32_t w, uint32_t h)
{
    g_autofree uint8_t *srow = g_malloc((size_t)w * 4);
    g_autofree uint8_t *drow = g_malloc((size_t)w * 2);

    for (uint32_t y = 0; y < h; y++) {
        cpu_physical_memory_read(sbase + (uint64_t)y * sstride, srow,
                                 (size_t)w * 4);
        for (uint32_t x = 0; x + 1 < w; x += 2) {
            uint32_t p0 = ldl_le_p(srow + x * 4);
            uint32_t p1 = ldl_le_p(srow + (x + 1) * 4);
            uint8_t y0, u0, v0, y1, u1, v1;
            /* G2D_RGBA8888 = bytes R,G,B,A: R is the low byte. */
            imx95_rgb_to_yuv601(p0 & 0xff, (p0 >> 8) & 0xff,
                                (p0 >> 16) & 0xff, &y0, &u0, &v0);
            imx95_rgb_to_yuv601(p1 & 0xff, (p1 >> 8) & 0xff,
                                (p1 >> 16) & 0xff, &y1, &u1, &v1);
            drow[x * 2 + 0] = y0;
            drow[x * 2 + 1] = (u0 + u1) / 2;
            drow[x * 2 + 2] = y1;
            drow[x * 2 + 3] = (v0 + v1) / 2;
        }
        cpu_physical_memory_write(dbase + (uint64_t)y * dstride, drow,
                                  (size_t)w * 2);
    }
}

/* YUYV -> RGBA8888, BT.601. */
static void imx95_blit_yuyv_to_rgba(uint64_t sbase,
                                    uint32_t sstride, uint64_t dbase,
                                    uint32_t dstride, uint32_t w, uint32_t h)
{
    g_autofree uint8_t *srow = g_malloc((size_t)w * 2);
    g_autofree uint8_t *drow = g_malloc((size_t)w * 4);

    for (uint32_t y = 0; y < h; y++) {
        cpu_physical_memory_read(sbase + (uint64_t)y * sstride, srow,
                                 (size_t)w * 2);
        for (uint32_t x = 0; x + 1 < w; x += 2) {
            uint8_t y0 = srow[x * 2 + 0], u = srow[x * 2 + 1];
            uint8_t y1 = srow[x * 2 + 2], v = srow[x * 2 + 3];
            uint8_t r, g, b;
            imx95_yuv601_to_rgb(y0, u, v, &r, &g, &b);
            stl_le_p(drow + x * 4, r | (g << 8) | (b << 16) | 0xff000000u);
            imx95_yuv601_to_rgb(y1, u, v, &r, &g, &b);
            stl_le_p(drow + (x + 1) * 4,
                     r | (g << 8) | (b << 16) | 0xff000000u);
        }
        cpu_physical_memory_write(dbase + (uint64_t)y * dstride, drow,
                                  (size_t)w * 4);
    }
}

/*
 * RGBA8888 -> NV12: Y to plane 0 (full res), interleaved Cb,Cr to plane 1 at
 * 4:2:0 (one chroma pair per 2x2 luma block, horizontally averaged).
 */
static void imx95_blit_rgba_to_nv12(uint64_t sbase,
                                    uint32_t sstride, uint64_t ybase,
                                    uint32_t ystride, uint64_t cbase,
                                    uint32_t cstride, uint32_t w, uint32_t h)
{
    g_autofree uint8_t *srow = g_malloc((size_t)w * 4);
    g_autofree uint8_t *yrow = g_malloc((size_t)w);
    g_autofree uint8_t *crow = g_malloc((size_t)w);

    for (uint32_t y = 0; y < h; y++) {
        cpu_physical_memory_read(sbase + (uint64_t)y * sstride, srow,
                                 (size_t)w * 4);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t p = ldl_le_p(srow + x * 4);
            uint8_t yy, uu, vv;
            imx95_rgb_to_yuv601(p & 0xff, (p >> 8) & 0xff, (p >> 16) & 0xff,
                                &yy, &uu, &vv);
            yrow[x] = yy;
        }
        cpu_physical_memory_write(ybase + (uint64_t)y * ystride, yrow,
                                  (size_t)w);
        if ((y & 1) == 0) {                 /* one chroma row per 2 luma rows */
            for (uint32_t x = 0; x + 1 < w; x += 2) {
                uint32_t p0 = ldl_le_p(srow + x * 4);
                uint32_t p1 = ldl_le_p(srow + (x + 1) * 4);
                uint8_t y0, u0, v0, y1, u1, v1;
                imx95_rgb_to_yuv601(p0 & 0xff, (p0 >> 8) & 0xff,
                                    (p0 >> 16) & 0xff, &y0, &u0, &v0);
                imx95_rgb_to_yuv601(p1 & 0xff, (p1 >> 8) & 0xff,
                                    (p1 >> 16) & 0xff, &y1, &u1, &v1);
                crow[x + 0] = (u0 + u1) / 2;
                crow[x + 1] = (v0 + v1) / 2;
            }
            cpu_physical_memory_write(cbase + (uint64_t)(y / 2) * cstride,
                                      crow, (size_t)w);
        }
    }
}

/* NV12 -> RGBA8888: Y from plane 0, Cb,Cr from plane 1 (half-res). */
static void imx95_blit_nv12_to_rgba(uint64_t ybase,
                                    uint32_t ystride, uint64_t cbase,
                                    uint32_t cstride, uint64_t dbase,
                                    uint32_t dstride, uint32_t w, uint32_t h)
{
    g_autofree uint8_t *yrow = g_malloc((size_t)w);
    g_autofree uint8_t *crow = g_malloc((size_t)w);
    g_autofree uint8_t *drow = g_malloc((size_t)w * 4);

    for (uint32_t y = 0; y < h; y++) {
        cpu_physical_memory_read(ybase + (uint64_t)y * ystride, yrow,
                                 (size_t)w);
        cpu_physical_memory_read(cbase + (uint64_t)(y / 2) * cstride, crow,
                                 (size_t)w);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t ci = (x / 2) * 2;
            uint8_t r, g, b;
            imx95_yuv601_to_rgb(yrow[x], crow[ci], crow[ci + 1], &r, &g, &b);
            stl_le_p(drow + x * 4, r | (g << 8) | (b << 16) | 0xff000000u);
        }
        cpu_physical_memory_write(dbase + (uint64_t)y * dstride, drow,
                                  (size_t)w * 4);
    }
}

/*
 * CSC path (Store9 fed by the colour-space converter, src-select 0x06): convert
 * between RGBA8888 and YUYV (4:2:2 packed) or NV12 (2-plane 4:2:0), BT.601. The
 * luma/RGB source is FetchDecode9 (like a copy); NV12's chroma is the Store9
 * plane 1 (write) or FetchEco9 (read). Returns true if a conversion ran, so a
 * non-conversion blit (e.g. a copy with a stale CSC select) falls through.
 */
static bool imx95_blit_convert(IMX95DPUState *s)
{
    uint32_t sattr = dpu_r(s, BLIT_FD9_SRCBUFATTR0);
    uint32_t ddim  = dpu_r(s, BLIT_STORE9_DSTBUFDIM);
    uint32_t dattr = dpu_r(s, BLIT_STORE9_DSTBUFATTR0);
    uint64_t sbase = dpu_r(s, BLIT_FD9_BASEADDRESS0) |
                     ((uint64_t)dpu_r(s, BLIT_FD9_BASEADDRESSMSB0) << 32);
    uint64_t dbase = dpu_r(s, BLIT_STORE9_BASEADDRESS0) |
                     ((uint64_t)dpu_r(s, BLIT_STORE9_BASEADDRESSMSB0) << 32);
    uint32_t sstride = BLIT_ATTR_STRIDE(sattr);
    uint32_t dstride = BLIT_ATTR_STRIDE(dattr);
    uint32_t sbpp = BLIT_FETCH_BPP(sattr);
    uint32_t dbpp = BLIT_STORE_BPP(dattr) ? BLIT_STORE_BPP(dattr) : 32;
    uint32_t w = BLIT_DIM_W(ddim), h = BLIT_DIM_H(ddim);

    if (sbase == 0 || dbase == 0 || w < 2 || h == 0) {
        return false;
    }
    if (sbpp == 32 && dbpp == 16) {                  /* RGBA8888 -> YUYV */
        imx95_blit_rgba_to_yuyv(sbase, sstride, dbase, dstride, w, h);
    } else if (sbpp == 16 && dbpp == 32) {           /* YUYV -> RGBA8888 */
        imx95_blit_yuyv_to_rgba(sbase, sstride, dbase, dstride, w, h);
    } else if (sbpp == 32 && dbpp == 8) {            /* RGBA8888 -> NV12 */
        uint32_t cmsb = dpu_r(s, BLIT_STORE9_BASEADDRESSMSB1);
        uint64_t cbase = dpu_r(s, BLIT_STORE9_BASEADDRESS1) |
                         ((uint64_t)cmsb << 32);
        uint32_t cstride = BLIT_ATTR_STRIDE(dpu_r(s, BLIT_STORE9_DSTBUFATTR1));
        if (cbase == 0) {
            return false;
        }
        imx95_blit_rgba_to_nv12(sbase, sstride, dbase, dstride,
                                cbase, cstride, w, h);
    } else if (sbpp == 8 && dbpp == 32) {            /* NV12 -> RGBA8888 */
        uint64_t cbase = dpu_r(s, BLIT_FE9_BASEADDRESS0) |
                         ((uint64_t)dpu_r(s, BLIT_FE9_BASEADDRESSMSB0) << 32);
        uint32_t cstride = BLIT_ATTR_STRIDE(dpu_r(s, BLIT_FE9_SRCBUFATTR0));
        if (cbase == 0) {
            return false;
        }
        imx95_blit_nv12_to_rgba(sbase, sstride, cbase, cstride,
                                dbase, dstride, w, h);
    } else {
        return false;
    }
    if (s->trace) {
        qemu_log("imx95-dpu: blit CONVERT src %ubpp -> dst %ubpp %ux%u "
                 "dst=0x%" PRIx64 "\n", sbpp, dbpp, w, h, dbase);
    }
    return true;
}

static void imx95_blit_run(IMX95DPUState *s)
{
    uint32_t store_src = dpu_r(s, BLIT_STORE9_SRCSEL) & 0x3f;
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

    /*
     * Store9 fed by FetchRot9 => rotation (same format) or colour conversion
     * (format differs). FetchDecode9 (copy/blend/scale) is the other source.
     */
    if (store_src == BLIT_STORE9_SRC_FETCHROT9 &&
        (dpu_r(s, BLIT_FR9_LAYERPROPERTY0) & BLIT_SRCBUF_ENABLE)) {
        if (BLIT_FETCH_BPP(dpu_r(s, BLIT_FR9_SRCBUFATTR0)) == dbpp) {
            imx95_blit_rotate(s);
        } else if (s->trace) {
            qemu_log("imx95-dpu: blit FetchRot9 convert (src!=dst fmt) not "
                     "modelled — skipped\n");
        }
        return;
    }
    /*
     * CSC unit feeds Store9 => RGBA<->YUYV/NV12 colour convert (BT.601). It
     * returns false for a non-conversion blit (e.g. a copy that left the CSC
     * select stale), which then falls through to the copy/scale path below.
     */
    if (store_src == BLIT_STORE9_SRC_CSC && imx95_blit_convert(s)) {
        return;
    }

    if (copy) {
        uint32_t sw = BLIT_DIM_W(sdim), sh = BLIT_DIM_H(sdim);
        bool scale = sw != w || sh != h;
        bool scaler_on =
            BLIT_SCALER_CLKEN(dpu_r(s, BLIT_HSCALER9_DYN)) != 0 ||
            BLIT_SCALER_CLKEN(dpu_r(s, BLIT_VSCALER9_DYN)) != 0;

        if (BLIT_FETCH_BPP(sattr) != dbpp) {
            if (s->trace) {
                qemu_log("imx95-dpu: blit format-convert (src %ubpp -> dst "
                         "%ubpp) not modelled — skipped\n",
                         BLIT_FETCH_BPP(sattr), dbpp);
            }
            return;
        }
        if (scale) {
            /*
             * Transforming blit: HScaler9/VScaler9 in the pipeline scale the
             * source rect to the destination rect. Nearest-neighbour resample
             * (the HW uses a bilinear filter; this matches in flat regions).
             */
            if (!scaler_on || dbpp != 32) {
                if (s->trace) {
                    qemu_log("imx95-dpu: blit unsupported transform "
                             "(src %ux%u -> dst %ux%u, scaler %s)\n",
                             sw, sh, w, h, scaler_on ? "on" : "off");
                }
                return;
            }
            g_autofree uint8_t *srow = g_malloc((size_t)sw * 4);
            g_autofree uint8_t *drow = g_malloc((size_t)w * 4);
            for (uint32_t y = 0; y < h; y++) {
                uint32_t sy = (uint32_t)((uint64_t)y * sh / h);
                cpu_physical_memory_read(sbase + (uint64_t)sy * sstride, srow,
                                         (size_t)sw * 4);
                for (uint32_t x = 0; x < w; x++) {
                    uint32_t sx = (uint32_t)((uint64_t)x * sw / w);
                    memcpy(drow + x * 4, srow + sx * 4, 4);
                }
                cpu_physical_memory_write(dbase + (uint64_t)y * dstride, drow,
                                          (size_t)w * 4);
            }
            if (s->trace) {
                qemu_log("imx95-dpu: blit SCALE %ux%u -> %ux%u src=0x%" PRIx64
                         " dst=0x%" PRIx64 "\n", sw, sh, w, h, sbase, dbase);
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
    if (off == DPU_INT2_STATUS(0)) {
        return s->int_status2[0];
    }
    if (off == DPU_INT2_STATUS(1)) {
        return s->int_status2[1];
    }
    if (off == DPU_INT2_ENABLE(0)) {
        return s->int_enable2[0];
    }
    if (off == DPU_INT2_ENABLE(1)) {
        return s->int_enable2[1];
    }
    if (off == DPU_FRAMEGEN0 + FG_FGCHSTAT ||
        off == DPU_FRAMEGEN1 + FG_FGCHSTAT) {
        /*
         * Primary channel "synced up" once the FrameGen is enabled. The CRTC
         * enable path polls this (readl_poll_timeout) and would otherwise log
         * "FrameGen primary channel isn't syncup" and abort the modeset. Leave
         * P/S-FIFOEMPTY clear so the FIFO-empty warning path stays quiet.
         */
        hwaddr fg = (off & ~0xffful);
        return (dpu_r(s, fg + FG_FGENABLE) & FG_FGEN) ? FG_PRIMSYNCSTAT : 0;
    }
    if (off == DPU_FRAMEGEN0 + FG_FGTIMESTAMP ||
        off == DPU_FRAMEGEN1 + FG_FGTIMESTAMP) {
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
                       in_block(off, DPU_FRAMEGEN0) ||
                       in_block(off, DPU_FRAMEGEN1);

    /* display interrupt blocks: stream-0 (n=0) + stream-1 (disp_irq2, n=0,1) */
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
    case DPU_INT2_ENABLE(0):
        s->int_enable2[0] = val;
        imx95_dpu_update_irq(s);
        imx95_dpu_arm_vblank(s);
        return;
    case DPU_INT2_ENABLE(1):
        s->int_enable2[1] = val;
        imx95_dpu_update_irq(s);
        imx95_dpu_arm_vblank(s);
        return;
    case DPU_INT2_CLEAR(0):
        s->int_status2[0] &= ~(uint32_t)val;
        imx95_dpu_update_irq(s);
        return;
    case DPU_INT2_CLEAR(1):
        s->int_status2[1] &= ~(uint32_t)val;
        imx95_dpu_update_irq(s);
        return;
    case DPU_INT2_PRESET(0):
        s->int_status2[0] |= (uint32_t)val;
        imx95_dpu_update_irq(s);
        return;
    case DPU_INT2_PRESET(1):
        s->int_status2[1] |= (uint32_t)val;
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
    s->int_status2[0] = s->int_status2[1] = 0;
    s->int_enable2[0] = s->int_enable2[1] = 0;
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
    for (int i = 0; i < DPU_NUM_IRQ_OUT2; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq_out2[i]);
    }
    s->vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, imx95_dpu_vblank_tick, s);
    for (int st = 0; st < DPU_NUM_STREAMS; st++) {
        s->ctx[st].s = s;
        s->ctx[st].stream = st;
    }
    s->con = qemu_graphic_console_create(dev, 0, &imx95_dpu_gfx_ops,
                                         &s->ctx[0]);
    s->con1 = qemu_graphic_console_create(dev, 1, &imx95_dpu_gfx_ops,
                                          &s->ctx[1]);
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
