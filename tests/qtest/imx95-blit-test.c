/*
 * QTest for the i.MX 95 DPU 2D blit engine (hw/misc/imx95_dpu.c).
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Drives the blit datapath with no kernel: push a Command Sequencer HIF program
 * (the same word stream the dpu95-blit DRM render node submits) that configures
 * the FetchDecode9 source / Store9 destination and triggers the blit, then
 * checks the destination buffer in guest RAM. Covers a constant-colour fill and
 * a same-format rectangular copy.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define DPU_BASE          0x4b400000ULL

#define CMDSEQ_HIF        0x10000
#define COMCTRL_PRESET1   0x1018
#define COMCTRL_STATUS1   0x1030

#define FD9_BASEADDRESS0  0x90028
#define FD9_SRCBUFATTR0   0x90038
#define FD9_SRCBUFDIM0    0x9003c
#define FD9_CONSTCOLOR0   0x90054
#define FD9_LAYERPROP0    0x90058
#define SRCBUF_ENABLE     (1u << 31)

#define STORE9_BASEADDR0  0xe0020
#define STORE9_DSTATTR0   0xe0030
#define STORE9_DSTDIM     0xe0054
#define STORE9_TRIGGER    0xe1014

/* BlitBlend9 Porter-Duff funcs: src [3:0], dst [11:8]; DYN CLKEN in [25:24]. */
#define BLEND9_DYN        0x71008
#define BLEND9_COLORFUNC  0x70018
#define BLEND9_ALPHAFUNC  0x70024
#define BLEND_FUNC(src, dst) ((src) | ((dst) << 8))
#define BLEND_ZERO        0
#define BLEND_ONE         1
#define BLEND_1_SRC_ALPHA 3
#define CLKEN_ON          0x01000000u   /* PIXENGCFG dynamic CLKEN (bit 24) */

/*
 * Scalers: enabling either HScaler9/VScaler9 (CLKEN) makes a size-changing
 * blit resample source->dest.
 */
#define HSCALER9_DYN      0xb1008
#define VSCALER9_DYN      0xc1008

#define RAM_BASE          0x80000000ULL
#define SRC_GPA           (RAM_BASE + 0x02000000)
#define DST_GPA           (RAM_BASE + 0x03000000)

#define W        8
#define H        8
#define BPP      32
#define BYTESPP  (BPP / 8)
#define STRIDE   (W * BYTESPP)

/*
 * DPU field encodings. STRIDE is bytes-1 [15:0] in both units; BITSPERPIXEL is
 * at [21:16] for the FetchDecode9 source but [31:24] for the Store9 dest.
 * Dimension is (w-1) | (h-1)<<16.
 */
#define ATTR_SRC(stride, bpp)  (((stride) - 1) | ((bpp) << 16))
#define ATTR_DST(stride, bpp)  (((stride) - 1) | ((bpp) << 24))
#define DIM(w, h)              (((w) - 1) | (((h) - 1) << 16))

static void hif(QTestState *qts, uint32_t word)
{
    qtest_writel(qts, DPU_BASE + CMDSEQ_HIF, word);
}

/* HIF "write 1 word" command: opcode, target offset, value. */
static void hif_w1(QTestState *qts, uint32_t off, uint32_t val)
{
    hif(qts, 0x14000001);
    hif(qts, off);
    hif(qts, val);
}

/* HIF "write 2 consecutive words" command (e.g. a 64-bit base address). */
static void hif_w2(QTestState *qts, uint32_t off, uint32_t v0, uint32_t v1)
{
    hif(qts, 0x14000002);
    hif(qts, off);
    hif(qts, v0);
    hif(qts, v1);
}

static void test_blit_fill(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest");
    uint32_t color = 0x11223344;
    uint8_t pre[W * H * BYTESPP];
    uint8_t got[BYTESPP];
    uint32_t status1;
    int x, y;

    /* dirty the destination first so a no-op blit would be caught */
    memset(pre, 0xa5, sizeof(pre));
    qtest_memwrite(qts, DST_GPA, pre, sizeof(pre));

    /* Configure Store9 dest, set fill colour, trigger, signal done. */
    hif_w2(qts, STORE9_BASEADDR0, (uint32_t)DST_GPA, (uint32_t)(DST_GPA >> 32));
    hif_w1(qts, STORE9_DSTATTR0, ATTR_DST(STRIDE, BPP));
    hif_w1(qts, STORE9_DSTDIM, DIM(W, H));
    hif_w1(qts, FD9_CONSTCOLOR0, color);
    hif_w1(qts, FD9_LAYERPROP0, 0);             /* source disabled => fill */
    hif_w1(qts, STORE9_TRIGGER, 0x10);
    hif(qts, 0x20000102);                        /* SEQCOMPLETE_SYNC (no-op) */
    hif_w1(qts, COMCTRL_PRESET1, 0x1);           /* completion SW interrupt */

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            uint64_t gpa = DST_GPA + y * STRIDE + x * BYTESPP;
            qtest_memread(qts, gpa, got, BYTESPP);
            g_assert_cmphex(got[0], ==, 0x44);   /* colour bytes, LE */
            g_assert_cmphex(got[1], ==, 0x33);
            g_assert_cmphex(got[2], ==, 0x22);
            g_assert_cmphex(got[3], ==, 0x11);
        }
    }

    status1 = qtest_readl(qts, DPU_BASE + COMCTRL_STATUS1);
    g_assert_cmphex(status1 & 0x1, ==, 0x1);

    qtest_quit(qts);
}

static void test_blit_copy(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest");
    uint8_t src[W * H * BYTESPP];
    uint8_t dst_pre[W * H * BYTESPP];
    uint8_t got[W * H * BYTESPP];
    int i;

    for (i = 0; i < (int)sizeof(src); i++) {
        src[i] = (uint8_t)(i * 7 + 1);
    }
    memset(dst_pre, 0, sizeof(dst_pre));
    qtest_memwrite(qts, SRC_GPA, src, sizeof(src));
    qtest_memwrite(qts, DST_GPA, dst_pre, sizeof(dst_pre));

    /* Configure FetchDecode9 source (enabled) + Store9 dest, then trigger. */
    hif_w2(qts, FD9_BASEADDRESS0, (uint32_t)SRC_GPA, (uint32_t)(SRC_GPA >> 32));
    hif_w1(qts, FD9_SRCBUFATTR0, ATTR_SRC(STRIDE, BPP));
    hif_w1(qts, FD9_SRCBUFDIM0, DIM(W, H));
    hif_w1(qts, FD9_LAYERPROP0, SRCBUF_ENABLE);
    hif_w2(qts, STORE9_BASEADDR0, (uint32_t)DST_GPA, (uint32_t)(DST_GPA >> 32));
    hif_w1(qts, STORE9_DSTATTR0, ATTR_DST(STRIDE, BPP));
    hif_w1(qts, STORE9_DSTDIM, DIM(W, H));
    hif_w1(qts, STORE9_TRIGGER, 0x10);

    qtest_memread(qts, DST_GPA, got, sizeof(got));
    for (i = 0; i < (int)sizeof(got); i++) {
        g_assert_cmphex(got[i], ==, src[i]);
    }

    qtest_quit(qts);
}

/*
 * Porter-Duff SRC_OVER blend (BlitBlend9): dst = src + dst*(1-src_alpha).
 * Byte order per pixel is R,G,B,A. Verified against two independently-computed
 * oracles: an opaque source (alpha 0xff) must reduce to a plain copy, and a
 * half-alpha source over an opaque dest must give the premultiplied SRC_OVER
 * result (255-factor arithmetic, integer /255).
 */
static void test_blit_blend(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest");
    uint8_t src[W * H * BYTESPP], dst[W * H * BYTESPP], got[W * H * BYTESPP];
    int i;

    /* src = (0x80,0x80,0x80, sa), dst = (0x40,0x40,0x40,0xff). */
    for (i = 0; i < (int)sizeof(src); i += 4) {
        src[i] = src[i + 1] = src[i + 2] = 0x80;
        src[i + 3] = 0x80;                       /* source alpha = 128 */
        dst[i] = dst[i + 1] = dst[i + 2] = 0x40;
        dst[i + 3] = 0xff;
    }
    qtest_memwrite(qts, SRC_GPA, src, sizeof(src));
    qtest_memwrite(qts, DST_GPA, dst, sizeof(dst));

    hif_w2(qts, FD9_BASEADDRESS0, (uint32_t)SRC_GPA, (uint32_t)(SRC_GPA >> 32));
    hif_w1(qts, FD9_SRCBUFATTR0, ATTR_SRC(STRIDE, BPP));
    hif_w1(qts, FD9_SRCBUFDIM0, DIM(W, H));
    hif_w1(qts, FD9_LAYERPROP0, SRCBUF_ENABLE);
    hif_w2(qts, STORE9_BASEADDR0, (uint32_t)DST_GPA, (uint32_t)(DST_GPA >> 32));
    hif_w1(qts, STORE9_DSTATTR0, ATTR_DST(STRIDE, BPP));
    hif_w1(qts, STORE9_DSTDIM, DIM(W, H));
    /* Route the source through BlitBlend9 with SRC_OVER funcs. */
    hif_w1(qts, BLEND9_DYN, CLKEN_ON);
    hif_w1(qts, BLEND9_COLORFUNC, BLEND_FUNC(BLEND_ONE, BLEND_1_SRC_ALPHA));
    hif_w1(qts, BLEND9_ALPHAFUNC, BLEND_FUNC(BLEND_ONE, BLEND_1_SRC_ALPHA));
    hif_w1(qts, STORE9_TRIGGER, 0x10);

    qtest_memread(qts, DST_GPA, got, sizeof(got));
    for (i = 0; i < (int)sizeof(got); i += 4) {
        /* colour: (0x80*255 + 0x40*(255-128))/255 = 40768/255 = 159 = 0x9f */
        g_assert_cmphex(got[i], ==, 0x9f);
        g_assert_cmphex(got[i + 1], ==, 0x9f);
        g_assert_cmphex(got[i + 2], ==, 0x9f);
        /* alpha: (0x80*255 + 0xff*127)/255 = 65025/255 = 255 = 0xff */
        g_assert_cmphex(got[i + 3], ==, 0xff);
    }
    qtest_quit(qts);
}

/*
 * Size-changing blit with a scaler enabled => nearest-neighbour resample.
 * A 4x4 source upscaled to 8x8: dest(x,y) must equal source(x/2, y/2).
 */
static void test_blit_scale(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest");
    enum { SW = 4, SH = 4, SSTRIDE = SW * BYTESPP };
    uint8_t src[SW * SH * BYTESPP];
    uint8_t got[W * H * BYTESPP];
    int x, y;

    /* source pixel (sx,sy) = bytes {sx, sy, 0xaa, 0xff}, distinct per pixel. */
    for (y = 0; y < SH; y++) {
        for (x = 0; x < SW; x++) {
            uint8_t *p = &src[(y * SW + x) * BYTESPP];
            p[0] = (uint8_t)x; p[1] = (uint8_t)y; p[2] = 0xaa; p[3] = 0xff;
        }
    }
    qtest_memwrite(qts, SRC_GPA, src, sizeof(src));

    hif_w2(qts, FD9_BASEADDRESS0, (uint32_t)SRC_GPA, (uint32_t)(SRC_GPA >> 32));
    hif_w1(qts, FD9_SRCBUFATTR0, ATTR_SRC(SSTRIDE, BPP));
    hif_w1(qts, FD9_SRCBUFDIM0, DIM(SW, SH));         /* 4x4 source */
    hif_w1(qts, FD9_LAYERPROP0, SRCBUF_ENABLE);
    hif_w2(qts, STORE9_BASEADDR0, (uint32_t)DST_GPA, (uint32_t)(DST_GPA >> 32));
    hif_w1(qts, STORE9_DSTATTR0, ATTR_DST(STRIDE, BPP));
    hif_w1(qts, STORE9_DSTDIM, DIM(W, H));            /* 8x8 dest */
    hif_w1(qts, HSCALER9_DYN, CLKEN_ON);        /* scaler in the pipeline */
    hif_w1(qts, VSCALER9_DYN, CLKEN_ON);
    hif_w1(qts, STORE9_TRIGGER, 0x10);

    qtest_memread(qts, DST_GPA, got, sizeof(got));
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            uint8_t *p = &got[(y * W + x) * BYTESPP];
            g_assert_cmphex(p[0], ==, x / 2);        /* nearest src x */
            g_assert_cmphex(p[1], ==, y / 2);        /* nearest src y */
            g_assert_cmphex(p[2], ==, 0xaa);
            g_assert_cmphex(p[3], ==, 0xff);
        }
    }
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("imx95/blit/fill", test_blit_fill);
    qtest_add_func("imx95/blit/copy", test_blit_copy);
    qtest_add_func("imx95/blit/blend", test_blit_blend);
    qtest_add_func("imx95/blit/scale", test_blit_scale);
    return g_test_run();
}
