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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("imx95/blit/fill", test_blit_fill);
    qtest_add_func("imx95/blit/copy", test_blit_copy);
    return g_test_run();
}
