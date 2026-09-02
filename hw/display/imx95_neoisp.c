/*
 * i.MX 95 NeoISP - camera image signal processor (V4L2 mem2mem)
 *
 * Copyright (c) 2026 Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The NeoISP develops raw Bayer into a viewable image: buffers in DRAM,
 * buffers out, no streaming from the sensor. The `neoisp` driver programs a
 * few thousand tuning registers and then starts work with a single write:
 *
 *   TRIG_CAM0 (0x20) BIT(0)  ->  read IMG0_IN_ADDR, develop, write OUTCH0_ADDR,
 *                                raise INT_STAT0.FD2 on the shared camera IRQ
 *
 * Addresses live in the registers SHIFTED RIGHT BY 4 (NEO_PIPE_CONF_ADDR_SET),
 * so they must be shifted back before touching guest memory. Getting that
 * wrong reads an address 16x too low - it faults rather than corrupting, which
 * is the kind failure mode, but it is still wrong.
 *
 * ⚠️ FIDELITY, STATED PLAINLY BECAUSE IT CANNOT BE MEASURED AWAY. The real
 * block's per-pixel arithmetic is proprietary: its exact demosaic kernel, its
 * colour matrix, its tone curve. This model is STRUCTURALLY correct - it moves
 * the right bytes, in the right order, and raises the right interrupt - and its
 * output is a faithful *bilinear* debayer, not a reproduction of NXP's silicon.
 * A guest sees a real developed image; it does not see the same image the
 * hardware would produce. Anyone comparing model output against silicon
 * pixel-for-pixel will find differences, and they are expected.
 */
#include "qemu/osdep.h"
#include "hw/display/imx95_neoisp.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "migration/vmstate.h"

/* --- register offsets (NEO_PIPE_CONF_*, from the driver's neoisp_regs.h) --- */
#define NEO_TRIG_CAM0           0x20
#define  NEO_TRIG_CAM0_TRIGGER  BIT(0)
#define NEO_INT_EN0_V2          0x24
#define NEO_INT_STAT0_V2        0x28
#define NEO_INT_EN0_V1          0x68
#define NEO_INT_STAT0_V1        0x6c
#define  NEO_INT_S_FS1          BIT(0)
#define  NEO_INT_S_FS2          BIT(1)
#define  NEO_INT_S_FD1          BIT(2)
#define  NEO_INT_S_FD2          BIT(3)   /* frame done - the one that completes a job */
#define NEO_IMG_CONF_CAM0       0x30
#define  NEO_IMG_CONF_IBPP0(v)  ((v) & 0xf)
#define NEO_IMG_SIZE_CAM0       0x34
#define NEO_IMG0_IN_ADDR_CAM0   0x3c
#define NEO_OUTCH0_ADDR_CAM0    0x44
#define NEO_OUTCH1_ADDR_CAM0    0x48
#define NEO_IMG0_IN_LS_CAM0     0x50
#define NEO_OUTCH0_LS_CAM0      0x58
#define NEO_OUTCH1_LS_CAM0      0x5c
/*
 * OB/WB0 - optical-black subtraction and white-balance gain, per Bayer channel.
 * One register per channel packs BOTH stages: offset in 15:0, gain in 31:16.
 * Gain is Q8, so 1<<8 = 256 is unity (the driver's default), and offset 0.
 * This is the first stage a tuning engineer touches, which is why it is the
 * first one modelled: it is what makes the params node stop being a no-op.
 */
#define NEO_OB_WB0_R_CTRL_CAM0  0x204
#define NEO_OB_WB0_GR_CTRL_CAM0 0x208
#define NEO_OB_WB0_GB_CTRL_CAM0 0x20c
#define NEO_OB_WB0_B_CTRL_CAM0  0x210
#define  NEO_OBWB_OFFSET(v)     ((v) & 0xffff)
#define  NEO_OBWB_GAIN(v)       (((v) >> 16) & 0xffff)
#define  NEO_OBWB_GAIN_UNITY    256             /* Q8 */

/*
 * Stages this model does NOT implement. Each block's control register carries
 * its enable in BIT(31), so a guest that programs one is asking for processing
 * that will not happen.
 *
 * ⚠️ PARTIAL HONOURING IS WORSE THAN NONE, and this is the reason these are
 * announced rather than quietly skipped. An engineer who loads a full tuning
 * blob sees white balance work - which earns the interface their trust - then
 * changes gamma, sees no difference, and concludes GAMMA DOES NOT AFFECT THIS
 * HARDWARE. That is a specific false belief about silicon, arrived at BECAUSE
 * part of the control was honest. Saying which stages are live is the only way
 * a partial implementation stays safe to use.
 */
static const struct {
    hwaddr off;
    const char *name;
} neoisp_unmodelled[] = {
    { 0x400,  "colour temperature / auto white balance" },
    { 0x800,  "Bayer noise reduction" },
    { 0x900,  "vignetting correction" },
    { 0x1480, "edge enhancement" },
    { 0x166c, "GCM black level / gamma / colour matrix" },
};

/* demosaic control: FMT bits 5:4 select the Bayer phase */
#define NEO_DEMOSAIC_CTRL_CAM0  0x1000
#define  NEO_DEMOSAIC_FMT(v)    (((v) >> 4) & 0x3)

/*
 * IMG_CONF.IBPP0 is an ENCODING, not a bit count. The driver writes
 * neoisp_format->bpp_enc, and the format table maps those to real sample
 * widths - notably 6 means EIGHT bits, not six. Reading it literally clamps
 * every sample to 2^6-1 = 63, which looks like a correctly structured image
 * that is simply dark and desaturated: the shape survives, so nothing appears
 * broken, and only the pixel VALUES give it away.
 */
static int neo_ibpp_bits(unsigned enc)
{
    switch (enc) {
    case 0:  return 12;
    case 1:  return 14;
    case 2:  return 16;
    case 4:  return 10;
    case 5:  return 10;                         /* 10-bit packed */
    case 6:  return 8;
    default: return 8;
    }
}

static uint32_t neo_int_stat_off(IMX95NeoIspState *s)
{
    return s->v2 ? NEO_INT_STAT0_V2 : NEO_INT_STAT0_V1;
}

static uint32_t neo_int_en_off(IMX95NeoIspState *s)
{
    return s->v2 ? NEO_INT_EN0_V2 : NEO_INT_EN0_V1;
}

static inline uint32_t neo_reg(IMX95NeoIspState *s, hwaddr off)
{
    return s->regs[off / 4];
}

/*
 * Bayer phase. The FMT field names which colour sits at pixel (0,0) of the
 * 2x2 mosaic; everything else follows from it.
 *   0 = RGGB   1 = GRBG   2 = GBRG   3 = BGGR
 * Returns the (x, y) parity at which RED appears.
 */
static void bayer_red_origin(unsigned fmt, int *rx, int *ry)
{
    switch (fmt & 3) {
    case 0: *rx = 0; *ry = 0; break;   /* RGGB */
    case 1: *rx = 1; *ry = 0; break;   /* GRBG */
    case 2: *rx = 0; *ry = 1; break;   /* GBRG */
    default: *rx = 1; *ry = 1; break;  /* BGGR */
    }
}

/* colour at (x,y): 0 = red, 1 = green, 2 = blue */
static inline int bayer_colour(int x, int y, int rx, int ry)
{
    int ox = (x & 1) == rx, oy = (y & 1) == ry;
    if (ox && oy) {
        return 0;
    }
    if (!ox && !oy) {
        return 2;
    }
    return 1;
}

static inline uint16_t clamp16(int v, int max)
{
    return v < 0 ? 0 : (v > max ? max : v);
}

/*
 * Bilinear demosaic. Deliberately the textbook kernel rather than something
 * cleverer: it is the one a reader can check by hand, and since this model
 * cannot be bit-exact against the real block anyway, a defensible and obvious
 * kernel beats an elaborate one that is differently wrong.
 */
/*
 * Per-Bayer-channel black level and gain. Returns the corrected sample.
 * A channel's identity at (x,y) selects which of the four OB/WB registers
 * applies - R, GR, GB or B - which is why this cannot be folded into a single
 * scalar: white balance IS the per-channel difference.
 */
static inline int neoisp_obwb(const IMX95NeoIspState *s, int v, int colour,
                              int on_red_row, int maxval)
{
    uint32_t reg;
    int off, gain;

    if (colour == 0) {
        reg = s->regs[NEO_OB_WB0_R_CTRL_CAM0 / 4];
    } else if (colour == 2) {
        reg = s->regs[NEO_OB_WB0_B_CTRL_CAM0 / 4];
    } else {
        /* the two greens are tuned separately: GR sits on a red row, GB on a
         * blue one, and a sensor's two green photosites genuinely differ */
        reg = on_red_row ? s->regs[NEO_OB_WB0_GR_CTRL_CAM0 / 4]
                         : s->regs[NEO_OB_WB0_GB_CTRL_CAM0 / 4];
    }
    off  = NEO_OBWB_OFFSET(reg);
    gain = NEO_OBWB_GAIN(reg);
    if (!gain) {
        gain = NEO_OBWB_GAIN_UNITY;             /* unprogrammed: pass through */
    }
    v = ((v - off) * gain) / NEO_OBWB_GAIN_UNITY;
    return v < 0 ? 0 : (v > maxval ? maxval : v);
}

static void neoisp_develop(IMX95NeoIspState *s, const uint8_t *in, uint8_t *out,
                           int w, int h, int in_ls, int out_ls, int fmt, int bpp,
                           int obpp)
{
    int rx, ry, x, y;
    int maxval = (1 << bpp) - 1;
    int shift = bpp > 8 ? bpp - 8 : 0;

    bayer_red_origin(fmt, &rx, &ry);

/*
 * Every raw sample passes through OB/WB before it is used, at the site whose
 * colour it actually is - not the colour of the pixel being reconstructed.
 * Applying the centre pixel's gain to its neighbours would silently defeat
 * white balance, since the neighbours are the OTHER channels.
 */
#define RAW(xx, yy) ({                                                        \
        int _x = (xx) < 0 ? 0 : ((xx) > w - 1 ? w - 1 : (xx));                \
        int _y = (yy) < 0 ? 0 : ((yy) > h - 1 ? h - 1 : (yy));                \
        int _v = bpp > 8 ? (int)lduw_le_p(in + (size_t)_y * in_ls + _x * 2)    \
                         : (int)in[(size_t)_y * in_ls + _x];                  \
        neoisp_obwb(s, _v, bayer_colour(_x, _y, rx, ry),                      \
                    (_y & 1) == ry, maxval);                                  \
    })

    for (y = 0; y < h; y++) {
        uint8_t *dst = out + (size_t)y * out_ls;
        for (x = 0; x < w; x++) {
            int c = bayer_colour(x, y, rx, ry);
            int r, g, b, v = RAW(x, y);

            if (c == 1) {                       /* green site */
                g = v;
                /* the two horizontal neighbours are one colour, vertical the
                 * other; which is which depends on the row's phase */
                if (((y & 1) == ry)) {
                    r = (RAW(x - 1, y) + RAW(x + 1, y) + 1) / 2;
                    b = (RAW(x, y - 1) + RAW(x, y + 1) + 1) / 2;
                } else {
                    b = (RAW(x - 1, y) + RAW(x + 1, y) + 1) / 2;
                    r = (RAW(x, y - 1) + RAW(x, y + 1) + 1) / 2;
                }
            } else {
                int diag = (RAW(x - 1, y - 1) + RAW(x + 1, y - 1) +
                            RAW(x - 1, y + 1) + RAW(x + 1, y + 1) + 2) / 4;
                g = (RAW(x - 1, y) + RAW(x + 1, y) +
                     RAW(x, y - 1) + RAW(x, y + 1) + 2) / 4;
                if (c == 0) {                   /* red site */
                    r = v; b = diag;
                } else {                        /* blue site */
                    b = v; r = diag;
                }
            }
            /*
             * Output pixel width comes from the line stride the driver
             * programmed, not from an assumption: this pipeline negotiates
             * BGR3 (3 bytes) as readily as a 32-bit format, and writing the
             * wrong width would shear the image while every byte count still
             * looked plausible.
             */
            dst[x * obpp + 0] = clamp16(b, maxval) >> shift;
            dst[x * obpp + 1] = clamp16(g, maxval) >> shift;
            dst[x * obpp + 2] = clamp16(r, maxval) >> shift;
            if (obpp == 4) {
                dst[x * 4 + 3] = 0xff;
            }
        }
    }
#undef RAW
}

/* A trigger arrived: develop one frame from guest memory back into it. */
static void neoisp_run_frame(IMX95NeoIspState *s)
{
    uint32_t size = neo_reg(s, NEO_IMG_SIZE_CAM0);
    int w = size & 0xffff, h = (size >> 16) & 0xffff;
    /* addresses are stored >> 4 */
    hwaddr in_pa  = (hwaddr)neo_reg(s, NEO_IMG0_IN_ADDR_CAM0) << 4;
    /*
     * Which output CHANNEL carries the image depends on the format, and the
     * driver is explicit about it: for a PACKED format (BGR3, YUYV...) it
     * deliberately writes channel 0 as zero and puts the buffer on channel 1;
     * channel 0 carries the first plane of a PLANAR format. Reading only
     * channel 0 sees address 0 and produces nothing, with no error anywhere -
     * the device simply triggers and never completes.
     */
    hwaddr out_pa = (hwaddr)neo_reg(s, NEO_OUTCH0_ADDR_CAM0) << 4;
    uint32_t in_ls  = neo_reg(s, NEO_IMG0_IN_LS_CAM0) & ~0xfu;
    uint32_t out_ls = neo_reg(s, NEO_OUTCH0_LS_CAM0) & ~0xfu;
    unsigned ibpp_enc = NEO_IMG_CONF_IBPP0(neo_reg(s, NEO_IMG_CONF_CAM0));
    int bpp = neo_ibpp_bits(ibpp_enc);
    int obpp;
    int fmt = NEO_DEMOSAIC_FMT(neo_reg(s, NEO_DEMOSAIC_CTRL_CAM0));
    g_autofree uint8_t *in = NULL;
    g_autofree uint8_t *out = NULL;

    if (!out_pa || !out_ls) {                   /* packed output: channel 1 */
        out_pa = (hwaddr)neo_reg(s, NEO_OUTCH1_ADDR_CAM0) << 4;
        out_ls = neo_reg(s, NEO_OUTCH1_LS_CAM0) & ~0xfu;
    }
    if (!w || !h || !in_pa || !out_pa) {
        qemu_log_mask(LOG_GUEST_ERROR, "imx95.neoisp: trigger with an incomplete "
                      "job (%dx%d in=0x%" HWADDR_PRIx " out=0x%" HWADDR_PRIx
                      ") - no frame developed\n", w, h, in_pa, out_pa);
        return;
    }
    if (!in_ls) {
        in_ls = w * (bpp > 8 ? 2 : 1);
    }
    if (!out_ls) {
        out_ls = w * 4;
    }
    obpp = out_ls / w;
    if (obpp != 3 && obpp != 4) {
        obpp = 4;                               /* unrecognised stride: assume 32bpp */
    }

    in  = g_malloc0((size_t)in_ls * h);
    out = g_malloc0((size_t)out_ls * h);
    if (dma_memory_read(&address_space_memory, in_pa, in, (size_t)in_ls * h,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "imx95.neoisp: input DMA read failed at "
                      "0x%" HWADDR_PRIx "\n", in_pa);
        return;
    }

    neoisp_develop(s, in, out, w, h, in_ls, out_ls, fmt, bpp, obpp);

    if (dma_memory_write(&address_space_memory, out_pa, out, (size_t)out_ls * h,
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "imx95.neoisp: output DMA write failed at "
                      "0x%" HWADDR_PRIx "\n", out_pa);
        return;
    }
    s->frames++;

    /* Say plainly what was asked for and not done - once per device. */
    if (!s->warned_stages) {
        for (size_t i = 0; i < ARRAY_SIZE(neoisp_unmodelled); i++) {
            if (s->regs[neoisp_unmodelled[i].off / 4] & (1u << 31)) {
                s->warned_stages = true;
                warn_report("imx95.neoisp: '%s' is ENABLED by the guest but is "
                            "NOT modelled. Black level and white balance (OB_WB0) "
                            "are applied; gamma, colour matrix, denoise and tone "
                            "mapping are not. Tuning those stages will not change "
                            "the output - that is a limit of this model, NOT of "
                            "the hardware.", neoisp_unmodelled[i].name);
                break;
            }
        }
    }

    /* Frame done. FD2 is the bit the driver's IRQ handler treats as completion. */
    s->regs[neo_int_stat_off(s) / 4] |= NEO_INT_S_FD2;
    if (s->regs[neo_int_en_off(s) / 4] & NEO_INT_S_FD2) {
        qemu_irq_raise(s->irq);
    }
}

static uint64_t neoisp_regs_read(void *opaque, hwaddr off, unsigned size)
{
    IMX95NeoIspState *s = opaque;

    if (off + size > IMX95_NEOISP_REGS_SIZE) {
        return 0;
    }
    return s->regs[off / 4];
}

static void neoisp_regs_write(void *opaque, hwaddr off, uint64_t val,
                              unsigned size)
{
    IMX95NeoIspState *s = opaque;

    if (off + size > IMX95_NEOISP_REGS_SIZE) {
        return;
    }

    if (off == neo_int_stat_off(s)) {
        /* write-1-to-clear; drop the line when nothing is left pending */
        s->regs[off / 4] &= ~(uint32_t)val;
        if (!(s->regs[off / 4] & (NEO_INT_S_FS1 | NEO_INT_S_FS2 |
                                  NEO_INT_S_FD1 | NEO_INT_S_FD2))) {
            qemu_irq_lower(s->irq);
        }
        return;
    }

    s->regs[off / 4] = val;

    if (off == NEO_TRIG_CAM0 && (val & NEO_TRIG_CAM0_TRIGGER)) {
        /*
         * TRIG_CAM0.TRIGGER IS A SELF-CLEARING COMMAND BIT, and modelling it
         * as an ordinary latched bit breaks the SECOND frame and every one
         * after it.
         *
         * The driver kicks each job with regmap_field_write(), which is a
         * read-modify-write, and regmap_update_bits() SKIPS THE WRITE
         * ALTOGETHER when the new value equals the old one. Leave the bit set
         * and the next kick computes "no change", no MMIO write is ever
         * issued, the ISP is never triggered, and the driver waits forever on
         * a job that was never started. Frame 0 develops perfectly and frame 1
         * hangs - which looks like an intermittent ISP fault and is really a
         * command bit that never cleared. (Note this needs no register cache:
         * the elision is in regmap_update_bits itself. This driver sets
         * REGCACHE_NONE.)
         *
         * Clear it before running so a read-back reports idle, exactly as the
         * hardware does once it has consumed the trigger.
         */
        s->regs[off / 4] = val & ~(uint32_t)NEO_TRIG_CAM0_TRIGGER;
        neoisp_run_frame(s);
    }
}

static const MemoryRegionOps neoisp_regs_ops = {
    .read = neoisp_regs_read,
    .write = neoisp_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static uint64_t neoisp_stats_read(void *opaque, hwaddr off, unsigned size)
{
    IMX95NeoIspState *s = opaque;
    uint64_t v = 0;

    if (off + size <= IMX95_NEOISP_STATS_SIZE) {
        memcpy(&v, s->stats + off, size);
    }
    return v;
}

static void neoisp_stats_write(void *opaque, hwaddr off, uint64_t val,
                               unsigned size)
{
    IMX95NeoIspState *s = opaque;

    if (off + size <= IMX95_NEOISP_STATS_SIZE) {
        memcpy(s->stats + off, &val, size);
    }
}

static const MemoryRegionOps neoisp_stats_ops = {
    .read = neoisp_stats_read,
    .write = neoisp_stats_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
};

static void neoisp_reset(DeviceState *dev)
{
    IMX95NeoIspState *s = IMX95_NEOISP(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->stats, 0, sizeof(s->stats));
    s->frames = 0;
    qemu_irq_lower(s->irq);
}

static void neoisp_realize(DeviceState *dev, Error **errp)
{
    IMX95NeoIspState *s = IMX95_NEOISP(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->regs_mr, OBJECT(dev), &neoisp_regs_ops, s,
                          "imx95.neoisp.regs", IMX95_NEOISP_REGS_SIZE);
    memory_region_init_io(&s->stats_mr, OBJECT(dev), &neoisp_stats_ops, s,
                          "imx95.neoisp.stats", IMX95_NEOISP_STATS_SIZE);
    sysbus_init_mmio(sbd, &s->regs_mr);
    sysbus_init_mmio(sbd, &s->stats_mr);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_imx95_neoisp = {
    .name = "imx95.neoisp",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX95NeoIspState,
                             IMX95_NEOISP_REGS_SIZE / 4),
        VMSTATE_UINT8_ARRAY(stats, IMX95NeoIspState, IMX95_NEOISP_STATS_SIZE),
        VMSTATE_UINT64(frames, IMX95NeoIspState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property neoisp_props[] = {
    /* imx95-b0 (the EVK) puts the interrupt registers at 0x24/0x28; a0 uses
     * 0x68/0x6c. Default to b0. */
    DEFINE_PROP_BOOL("v2", IMX95NeoIspState, v2, true),
};

static void neoisp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = neoisp_realize;
    device_class_set_legacy_reset(dc, neoisp_reset);
    dc->vmsd = &vmstate_imx95_neoisp;
    dc->desc = "i.MX95 NeoISP camera image signal processor";
    device_class_set_props(dc, neoisp_props);
}

static const TypeInfo neoisp_types[] = {
    {
        .name = TYPE_IMX95_NEOISP,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX95NeoIspState),
        .class_init = neoisp_class_init,
    },
};

DEFINE_TYPES(neoisp_types)
