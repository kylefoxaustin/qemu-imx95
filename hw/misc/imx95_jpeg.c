/*
 * NXP i.MX 95 JPEG decoder (CAST JPEG codec, "fsl,imx9-jpgdec")
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Functional decode for the mxc-jpeg V4L2 mem2mem driver. The HW is
 * descriptor-driven: the driver builds a chain of mxc_jpeg_desc records in
 * guest memory, points the slot's NXT_DESCPT_PTR at the head, and kicks decode
 * by writing CAST_CTRL (= MXC_DEC_EXIT_IDLE_MODE). The end-of-chain descriptor
 * (next_descpt_ptr == 0) carries the real source bitstream (stm_bufbase) and
 * the destination frame (buf_base0/1); the leading "configuration" descriptor
 * just primes the real HW's parser, which libjpeg does not need.
 *
 * On the kick we walk to the end descriptor, DMA the JPEG in, decode it to RGB
 * with libjpeg, convert to the descriptor's output format (STM_CTRL image
 * format field), DMA the frame out, latch SLOT_STATUS.FRMDONE + the payload
 * size in SLOT_BUF_PTR, and raise the slot's interrupt - exactly what the
 * driver's IRQ handler consumes. Without libjpeg at build time the registers
 * still model (the node registers) but a decode reports an error.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "system/dma.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#ifdef CONFIG_LIBJPEG
#include <jpeglib.h>
#include <jerror.h>
#include <setjmp.h>
#endif

#define TYPE_IMX95_JPEG "imx95.jpeg"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95JpegState, IMX95_JPEG)

#define IMX95_JPEG_MMIO         0x50000
#define IMX95_JPEG_NUM_SLOTS    4

/* Wrapper registers. */
#define GLB_CTRL                0x00
#define COM_STATUS              0x04
#define BUF_BASE0               0x14
#define STM_CTRL                0x2c

/* CAST core. */
#define CAST_STATUS0            0x100
#define CAST_CTRL               0x134   /* = CAST_STATUS13 */
#define CAST_STATUS_LAST        0x14c

/* Per-slot registers (slot s at 0x10000 * (s + 1)). */
#define SLOT_BASE               0x10000
#define SLOT_STATUS             0x0
#define SLOT_IRQ_EN             0x4
#define SLOT_BUF_PTR            0x8
#define SLOT_CUR_DESCPT_PTR     0xc
#define SLOT_NXT_DESCPT_PTR     0x10

#define GLB_CTRL_JPG_EN         0x1
#define GLB_CTRL_SFT_RST        (0x1 << 1)
#define GLB_CTRL_SLOT_EN(s)     (0x1 << ((s) + 4))

#define SLOT_STATUS_FRMDONE     (0x1 << 3)
#define SLOT_STATUS_ENC_CONFIG_ERR (0x1 << 8)

#define MXC_NXT_DESCPT_EN       0x1
#define MXC_DEC_EXIT_IDLE_MODE  0x4

/* STM_CTRL image-format field (bits [6:3]). */
#define STM_CTRL_IMG_FMT_SHIFT  3
#define STM_CTRL_IMG_FMT_MASK   0xf
#define IMGFMT_YUV420           0x0   /* NV12: Y plane + interleaved UV plane */
#define IMGFMT_YUV422           0x1   /* YUYV packed                          */
#define IMGFMT_BGR              0x2   /* BGR packed                           */
#define IMGFMT_YUV444           0x3   /* YUVYUV packed                        */
#define IMGFMT_GRAY             0x4   /* Y8                                   */
#define IMGFMT_ABGR             0x6   /* ABGR packed                          */

/* The 8-word mxc_jpeg_desc, little-endian in guest memory. */
typedef struct MxcJpegDesc {
    uint32_t next_descpt_ptr;
    uint32_t buf_base0;
    uint32_t buf_base1;
    uint32_t line_pitch;
    uint32_t stm_bufbase;
    uint32_t stm_bufsize;
    uint32_t imgsize;
    uint32_t stm_ctrl;
} MxcJpegDesc;

#define JPEG_MAX_DIM            8192    /* sanity cap on decoded dimensions */

typedef struct IMX95JpegSlot {
    uint32_t status;
    uint32_t irq_en;
    uint32_t buf_ptr;
    uint32_t cur_descpt;
    uint32_t nxt_descpt;
} IMX95JpegSlot;

struct IMX95JpegState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    uint32_t glb_ctrl;
    uint32_t cast[(CAST_STATUS_LAST - CAST_STATUS0) / 4 + 1];
    IMX95JpegSlot slot[IMX95_JPEG_NUM_SLOTS];

    qemu_irq irq[IMX95_JPEG_NUM_SLOTS];
};

static void imx95_jpeg_read_desc(MxcJpegDesc *d, uint32_t addr)
{
    uint32_t raw[8];
    int i;

    dma_memory_read(&address_space_memory, addr, raw, sizeof(raw),
                    MEMTXATTRS_UNSPECIFIED);
    for (i = 0; i < 8; i++) {
        raw[i] = le32_to_cpu(raw[i]);
    }
    d->next_descpt_ptr = raw[0];
    d->buf_base0       = raw[1];
    d->buf_base1       = raw[2];
    d->line_pitch      = raw[3];
    d->stm_bufbase     = raw[4];
    d->stm_bufsize     = raw[5];
    d->imgsize         = raw[6];
    d->stm_ctrl        = raw[7];
}

#ifdef CONFIG_LIBJPEG
struct jpeg_err_jmp {
    struct jpeg_error_mgr pub;
    jmp_buf jmp;
};

static void imx95_jpeg_err_exit(j_common_ptr cinfo)
{
    struct jpeg_err_jmp *e = (struct jpeg_err_jmp *)cinfo->err;
    longjmp(e->jmp, 1);
}

/*
 * Decode the JPEG at [src,src_len) into a width*height*3 RGB buffer. Returns a
 * g_malloc'd buffer (caller frees) with the width/height set, or NULL on error.
 */
static uint8_t *imx95_jpeg_decode_rgb(const uint8_t *src, unsigned long src_len,
                                      uint32_t *w, uint32_t *h)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_err_jmp jerr;
    uint8_t *rgb = NULL;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = imx95_jpeg_err_exit;
    if (setjmp(jerr.jmp)) {
        jpeg_destroy_decompress(&cinfo);
        g_free(rgb);
        return NULL;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, src, src_len);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        longjmp(jerr.jmp, 1);
    }
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    if (cinfo.output_width == 0 || cinfo.output_height == 0 ||
        cinfo.output_width > JPEG_MAX_DIM ||
        cinfo.output_height > JPEG_MAX_DIM) {
        longjmp(jerr.jmp, 1);
    }
    *w = cinfo.output_width;
    *h = cinfo.output_height;
    rgb = g_malloc((size_t)*w * *h * 3);

    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = rgb + (size_t)cinfo.output_scanline * *w * 3;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return rgb;
}

static inline uint8_t clamp_u8(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* BT.601 full-range RGB -> Y/Cb/Cr (matches libjpeg's JFIF convention). */
static inline uint8_t rgb_y(int r, int g, int b)
{
    return clamp_u8((19595 * r + 38470 * g + 7471 * b + 32768) >> 16);
}
static inline uint8_t rgb_cb(int r, int g, int b)
{
    return clamp_u8(((-11059 * r - 21709 * g + 32768 * b + 8388608) >> 16));
}
static inline uint8_t rgb_cr(int r, int g, int b)
{
    return clamp_u8(((32768 * r - 27439 * g - 5329 * b + 8388608) >> 16));
}

/*
 * Convert the RGB frame to the descriptor's output format and DMA it to the
 * destination buffer(s). Returns the number of output bytes written.
 */
static uint32_t imx95_jpeg_emit(const MxcJpegDesc *d, const uint8_t *rgb,
                                uint32_t w, uint32_t h)
{
    uint32_t fmt = (d->stm_ctrl >> STM_CTRL_IMG_FMT_SHIFT) &
                   STM_CTRL_IMG_FMT_MASK;
    uint32_t pitch = d->line_pitch;
    uint32_t y, x, total = 0;
    g_autofree uint8_t *line = NULL;

    switch (fmt) {
    case IMGFMT_BGR:
    case IMGFMT_ABGR:
    case IMGFMT_GRAY:
    case IMGFMT_YUV422:
    case IMGFMT_YUV444: {
        uint32_t bpp = (fmt == IMGFMT_GRAY) ? 1 :
                       (fmt == IMGFMT_ABGR) ? 4 :
                       (fmt == IMGFMT_YUV422) ? 2 : 3;
        if (!pitch) {
            pitch = w * bpp;
        }
        line = g_malloc(pitch);
        for (y = 0; y < h; y++) {
            const uint8_t *s = rgb + (size_t)y * w * 3;
            memset(line, 0, pitch);
            for (x = 0; x < w; x++) {
                int r = s[x * 3], g = s[x * 3 + 1], b = s[x * 3 + 2];
                uint8_t *o = line;
                switch (fmt) {
                case IMGFMT_BGR:
                    o += x * 3; o[0] = b; o[1] = g; o[2] = r;
                    break;
                case IMGFMT_ABGR:
                    o += x * 4; o[0] = 0xff; o[1] = b; o[2] = g; o[3] = r;
                    break;
                case IMGFMT_GRAY:
                    o[x] = rgb_y(r, g, b);
                    break;
                case IMGFMT_YUV444:
                    o += x * 3;
                    o[0] = rgb_y(r, g, b);
                    o[1] = rgb_cb(r, g, b);
                    o[2] = rgb_cr(r, g, b);
                    break;
                case IMGFMT_YUV422:    /* YUYV: shared chroma per 2 px */
                    o += x * 2;
                    o[0] = rgb_y(r, g, b);
                    o[1] = (x & 1) ? rgb_cr(r, g, b) : rgb_cb(r, g, b);
                    break;
                default:
                    break;
                }
            }
            dma_memory_write(&address_space_memory,
                             d->buf_base0 + (uint64_t)y * pitch, line, pitch,
                             MEMTXATTRS_UNSPECIFIED);
            total += pitch;
        }
        break;
    }
    case IMGFMT_YUV420: {       /* NV12: Y plane -> buf0, UV plane -> buf1 */
        uint32_t ypitch = pitch ? pitch : w;
        line = g_malloc(ypitch);
        for (y = 0; y < h; y++) {
            const uint8_t *s = rgb + (size_t)y * w * 3;
            memset(line, 0, ypitch);
            for (x = 0; x < w; x++) {
                line[x] = rgb_y(s[x * 3], s[x * 3 + 1], s[x * 3 + 2]);
            }
            dma_memory_write(&address_space_memory,
                             d->buf_base0 + (uint64_t)y * ypitch, line, ypitch,
                             MEMTXATTRS_UNSPECIFIED);
            total += ypitch;
        }
        for (y = 0; y < h / 2; y++) {
            const uint8_t *s = rgb + (size_t)(y * 2) * w * 3;
            memset(line, 0, ypitch);
            for (x = 0; x < w / 2; x++) {
                int r = s[x * 6], g = s[x * 6 + 1], b = s[x * 6 + 2];
                line[x * 2]     = rgb_cb(r, g, b);
                line[x * 2 + 1] = rgb_cr(r, g, b);
            }
            dma_memory_write(&address_space_memory,
                             d->buf_base1 + (uint64_t)y * ypitch, line, ypitch,
                             MEMTXATTRS_UNSPECIFIED);
            total += ypitch;
        }
        break;
    }
    default:
        return 0;
    }
    return total;
}
#endif /* CONFIG_LIBJPEG */

/* Perform the decode for one slot, latch status, raise the interrupt. */
static void imx95_jpeg_run(IMX95JpegState *s, int slot_idx)
{
    IMX95JpegSlot *slot = &s->slot[slot_idx];
    uint32_t addr = slot->nxt_descpt & ~MXC_NXT_DESCPT_EN;
    MxcJpegDesc d;
    int guard = 0;

    if (!addr) {
        return;
    }
    /* Walk to the end-of-chain (data) descriptor. */
    do {
        slot->cur_descpt = addr;
        imx95_jpeg_read_desc(&d, addr);
        addr = d.next_descpt_ptr & ~MXC_NXT_DESCPT_EN;
    } while (addr && ++guard < IMX95_JPEG_NUM_SLOTS + 2);

#ifdef CONFIG_LIBJPEG
    if (d.stm_bufbase && d.stm_bufsize && d.buf_base0) {
        g_autofree uint8_t *src = g_malloc(d.stm_bufsize);
        uint32_t w = 0, h = 0;
        uint8_t *rgb;

        dma_memory_read(&address_space_memory, d.stm_bufbase, src,
                        d.stm_bufsize, MEMTXATTRS_UNSPECIFIED);
        rgb = imx95_jpeg_decode_rgb(src, d.stm_bufsize, &w, &h);
        if (rgb) {
            slot->buf_ptr = imx95_jpeg_emit(&d, rgb, w, h);
            g_free(rgb);
            slot->status |= SLOT_STATUS_FRMDONE;
            qemu_set_irq(s->irq[slot_idx], 1);
            return;
        }
    }
#endif
    /* No libjpeg, or a malformed stream: report a decode error. */
    slot->status |= SLOT_STATUS_ENC_CONFIG_ERR;
    qemu_set_irq(s->irq[slot_idx], 1);
}

/* Active slot = the lowest one the driver has enabled in GLB_CTRL. */
static int imx95_jpeg_active_slot(IMX95JpegState *s)
{
    int i;

    for (i = 0; i < IMX95_JPEG_NUM_SLOTS; i++) {
        if (s->glb_ctrl & GLB_CTRL_SLOT_EN(i)) {
            return i;
        }
    }
    return 0;
}

static uint64_t imx95_jpeg_read(void *opaque, hwaddr off, unsigned size)
{
    IMX95JpegState *s = opaque;
    int i;

    for (i = 0; i < IMX95_JPEG_NUM_SLOTS; i++) {
        hwaddr base = SLOT_BASE * (i + 1);
        if (off >= base && off < base + 0x100) {
            switch (off - base) {
            case SLOT_STATUS:        return s->slot[i].status;
            case SLOT_IRQ_EN:        return s->slot[i].irq_en;
            case SLOT_BUF_PTR:       return s->slot[i].buf_ptr;
            case SLOT_CUR_DESCPT_PTR: return s->slot[i].cur_descpt;
            case SLOT_NXT_DESCPT_PTR: return s->slot[i].nxt_descpt;
            default:                 return 0;
            }
        }
    }

    if (off >= CAST_STATUS0 && off <= CAST_STATUS_LAST) {
        return s->cast[(off - CAST_STATUS0) / 4];
    }
    switch (off) {
    case GLB_CTRL:   return s->glb_ctrl;
    case COM_STATUS: return 0;       /* idle: no decode in flight */
    default:         return 0;
    }
}

static void imx95_jpeg_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    IMX95JpegState *s = opaque;
    uint32_t v = val;
    int i;

    for (i = 0; i < IMX95_JPEG_NUM_SLOTS; i++) {
        hwaddr base = SLOT_BASE * (i + 1);
        if (off >= base && off < base + 0x100) {
            switch (off - base) {
            case SLOT_STATUS:
                s->slot[i].status &= ~v;     /* write 1 to clear */
                if (!s->slot[i].status) {
                    qemu_set_irq(s->irq[i], 0);
                }
                break;
            case SLOT_IRQ_EN:
                s->slot[i].irq_en = v;
                break;
            case SLOT_NXT_DESCPT_PTR:
                s->slot[i].nxt_descpt = v;
                break;
            default:
                break;
            }
            return;
        }
    }

    if (off >= CAST_STATUS0 && off <= CAST_STATUS_LAST) {
        s->cast[(off - CAST_STATUS0) / 4] = v;
        /* dec_mode_go() writes CAST_CTRL = MXC_DEC_EXIT_IDLE_MODE: kick. */
        if (off == CAST_CTRL && v == MXC_DEC_EXIT_IDLE_MODE) {
            imx95_jpeg_run(s, imx95_jpeg_active_slot(s));
        }
        return;
    }

    switch (off) {
    case GLB_CTRL:
        if (v & GLB_CTRL_SFT_RST) {
            s->glb_ctrl = 0;
        } else {
            s->glb_ctrl = v;
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps imx95_jpeg_ops = {
    .read = imx95_jpeg_read,
    .write = imx95_jpeg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx95_jpeg_reset(DeviceState *dev)
{
    IMX95JpegState *s = IMX95_JPEG(dev);
    int i;

    s->glb_ctrl = 0;
    memset(s->cast, 0, sizeof(s->cast));
    memset(s->slot, 0, sizeof(s->slot));
    for (i = 0; i < IMX95_JPEG_NUM_SLOTS; i++) {
        qemu_set_irq(s->irq[i], 0);
    }
}

static void imx95_jpeg_init(Object *obj)
{
    IMX95JpegState *s = IMX95_JPEG(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    int i;

    memory_region_init_io(&s->iomem, obj, &imx95_jpeg_ops, s,
                          TYPE_IMX95_JPEG, IMX95_JPEG_MMIO);
    sysbus_init_mmio(sbd, &s->iomem);
    for (i = 0; i < IMX95_JPEG_NUM_SLOTS; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
}

static const VMStateDescription vmstate_imx95_jpeg_slot = {
    .name = "imx95.jpeg.slot",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(status, IMX95JpegSlot),
        VMSTATE_UINT32(irq_en, IMX95JpegSlot),
        VMSTATE_UINT32(buf_ptr, IMX95JpegSlot),
        VMSTATE_UINT32(cur_descpt, IMX95JpegSlot),
        VMSTATE_UINT32(nxt_descpt, IMX95JpegSlot),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_imx95_jpeg = {
    .name = TYPE_IMX95_JPEG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(glb_ctrl, IMX95JpegState),
        VMSTATE_UINT32_ARRAY(cast, IMX95JpegState,
                             (CAST_STATUS_LAST - CAST_STATUS0) / 4 + 1),
        VMSTATE_STRUCT_ARRAY(slot, IMX95JpegState, IMX95_JPEG_NUM_SLOTS, 1,
                             vmstate_imx95_jpeg_slot, IMX95JpegSlot),
        VMSTATE_END_OF_LIST()
    },
};

static void imx95_jpeg_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_imx95_jpeg;
    device_class_set_legacy_reset(dc, imx95_jpeg_reset);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "NXP i.MX95 JPEG decoder";
}

static const TypeInfo imx95_jpeg_info = {
    .name          = TYPE_IMX95_JPEG,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMX95JpegState),
    .instance_init = imx95_jpeg_init,
    .class_init    = imx95_jpeg_class_init,
};

static void imx95_jpeg_register_types(void)
{
    type_register_static(&imx95_jpeg_info);
}

type_init(imx95_jpeg_register_types)
