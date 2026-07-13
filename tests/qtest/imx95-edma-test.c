/*
 * QTest for the i.MX 95 eDMA controller (hw/dma/imx95_edma.c).
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Programs a TCD on edma1 channel 0 with no kernel and triggers it by writing
 * CH_CSR.ERQ, then checks the moved bytes in guest RAM. Two cases:
 *   - a plain memory->memory minor/major transfer (no minor-loop offset), and
 *   - a transfer with the NBYTES minor-loop offset enabled (SMLOE + a negative
 *     MLOFF), the format MICFIL multichannel capture uses to rewind SADDR after
 *     each minor loop. Masking only the two flag bits (the old code) left the
 *     20-bit offset in the byte count, so a minor loop tried to move ~1 GiB and
 *     hung the guest; this asserts the count is the low 10 bits and the offset
 *     is applied per minor loop.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define EDMA1_BASE      0x44000000ULL
#define CHAN_OFFSET     0x10000             /* first channel page */
#define CH(n)           (EDMA1_BASE + CHAN_OFFSET + (n) * 0x10000)

/* Per-channel register offsets within a channel page. */
#define CH_CSR          0x00
#define CH_SBR          0x0c
#define TCD_SADDR       0x20
#define TCD_SOFF        0x24
#define TCD_ATTR        0x26
#define TCD_NBYTES      0x28
#define TCD_DADDR       0x30
#define TCD_DOFF        0x34
#define TCD_CITER       0x36
#define TCD_DLAST       0x38
#define TCD_CSR         0x3c
#define TCD_BITER       0x3e

#define CH_CSR_ERQ      (1u << 0)
#define CH_SBR_WR       (1u << 21)          /* memory -> device: run now */
#define CH_SBR_RD       (1u << 22)          /* device -> memory            */
#define TCD_CSR_INTMAJ  (1u << 1)
#define TCD_CSR_ESG     (1u << 4)           /* scatter/gather (cyclic)     */

/*
 * edma2 (fsl,imx95-edma5) uses the 64-bit TCD layout: 64-bit SADDR/DADDR and
 * shifted later fields. Base 0x42000000, 0x8000 channel stride.
 */
#define EDMA2_BASE      0x42000000ULL
#define E2_CH(n)        (EDMA2_BASE + CHAN_OFFSET + (n) * 0x8000)
#define T64_SADDR       0x20
#define T64_SOFF        0x28
#define T64_ATTR        0x2a
#define T64_NBYTES      0x2c
#define T64_DADDR       0x38
#define T64_DLAST       0x40
#define T64_DOFF        0x48
#define T64_CITER       0x4a
#define T64_CSR         0x4c
#define T64_BITER       0x4e

/* SAI3 (wm8962 card), served by edma2 for playback. */
#define SAI3_BASE       0x42650000ULL
#define SAI_TCSR        0x08
#define SAI_TCR1        0x0c
#define SAI_TDR0        0x20
#define TCSR_TE         (1u << 31)
#define TCSR_FRDE       (1u << 0)

/* MICFIL: PDM mic that feeds eDMA1 a capture stream. */
#define MICFIL_BASE     0x44520000ULL
#define MICFIL_CTRL1    0x00
#define MICFIL_FIFO_CTRL 0x10
#define MICFIL_DATACH0  0x24
#define CTRL1_PDMIEN    (1u << 29)
#define CTRL1_DISEL_DMA (1u << 24)
#define MICFIL_WORD_NS  (1000000000LL / 48000)

/* ATTR = (SSIZE << 8) | DSIZE; size field s encodes 1<<s bytes. */
#define ATTR(s)         (((s) << 8) | (s))

#define RAM_BASE        0x80000000ULL
#define SRC_GPA         (RAM_BASE + 0x02000000)
#define DST_GPA         (RAM_BASE + 0x03000000)

static void prog_tcd(QTestState *qts, uint64_t src, uint64_t dst,
                     uint16_t soff, uint16_t doff, uint16_t attr,
                     uint32_t nbytes, uint16_t citer)
{
    qtest_writel(qts, CH(0) + TCD_SADDR, (uint32_t)src);
    qtest_writew(qts, CH(0) + TCD_SOFF, soff);
    qtest_writew(qts, CH(0) + TCD_ATTR, attr);
    qtest_writel(qts, CH(0) + TCD_NBYTES, nbytes);
    qtest_writel(qts, CH(0) + TCD_DADDR, (uint32_t)dst);
    qtest_writew(qts, CH(0) + TCD_DOFF, doff);
    qtest_writew(qts, CH(0) + TCD_CITER, citer);
    /* Force the tx (run-now) path, then kick with ERQ. */
    qtest_writel(qts, CH(0) + CH_SBR, CH_SBR_WR);
    qtest_writel(qts, CH(0) + CH_CSR, CH_CSR_ERQ);
}

/* Plain mem->mem: 3 minor loops of 8 bytes, both sides advancing. */
static void test_edma_plain(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest");
    uint8_t src[24], dst_pre[24], got[24];
    int i;

    for (i = 0; i < (int)sizeof(src); i++) {
        src[i] = (uint8_t)(i * 7 + 3);
    }
    memset(dst_pre, 0xa5, sizeof(dst_pre));
    qtest_memwrite(qts, SRC_GPA, src, sizeof(src));
    qtest_memwrite(qts, DST_GPA, dst_pre, sizeof(dst_pre));

    /* esize 4 (ATTR size 2), nbytes 8/minor, citer 3, soff=doff=4. */
    prog_tcd(qts, SRC_GPA, DST_GPA, 4, 4, ATTR(2), 8, 3);

    qtest_memread(qts, DST_GPA, got, sizeof(got));
    for (i = 0; i < (int)sizeof(src); i++) {
        g_assert_cmphex(got[i], ==, src[i]);
    }
    qtest_quit(qts);
}

/*
 * Minor-loop offset (MICFIL-style): SMLOE set, MLOFF = -8, nbytes = 8, so each
 * minor loop reads the same 8 source bytes (SADDR rewinds by 8 after the loop)
 * while DADDR keeps advancing. 3 minor loops => the 8 source bytes repeated 3x.
 */
static void test_edma_mloff(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest");
    uint8_t src[8], dst_pre[24], got[24];
    uint32_t nb_mloff;
    int i;

    for (i = 0; i < (int)sizeof(src); i++) {
        src[i] = (uint8_t)(0x10 + i);
    }
    memset(dst_pre, 0xa5, sizeof(dst_pre));
    qtest_memwrite(qts, SRC_GPA, src, sizeof(src));
    qtest_memwrite(qts, DST_GPA, dst_pre, sizeof(dst_pre));

    /* SMLOE(31) | MLOFF=-8 in bits[29:10] (0xFFFF8<<10) | nbytes=8. */
    nb_mloff = (1u << 31) | ((0xFFFF8u << 10) & 0x3ffffc00u) | 8u;
    g_assert_cmphex(nb_mloff, ==, 0xBFFFE008u);

    prog_tcd(qts, SRC_GPA, DST_GPA, 4, 4, ATTR(2), nb_mloff, 3);

    /*
     * If MLOFF had leaked into the count the guest would have hung; here we
     * simply confirm each minor loop re-read the same 8 source bytes.
     */
    qtest_memread(qts, DST_GPA, got, sizeof(got));
    for (i = 0; i < (int)sizeof(got); i++) {
        g_assert_cmphex(got[i], ==, src[i % 8]);
    }
    qtest_quit(qts);
}

/*
 * Cyclic capture datapath end to end (no kernel): MICFIL synthesises a sample
 * stream into its FIFO and pulses a DMA request as it passes the watermark; a
 * cyclic (ESG) eDMA channel reads MICFIL DATACH0 into a memory ring one minor
 * loop per request. Stepping the virtual clock fires the MICFIL word timer, so
 * non-silent samples must land in RAM - the same path a real arecord drives.
 */
static void test_edma_micfil_cyclic(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest");
    uint8_t got[64 * 4];
    int i, nonzero = 0;

    /* Clear the destination ring so any landed sample is visible. */
    memset(got, 0, sizeof(got));
    qtest_memwrite(qts, DST_GPA, got, sizeof(got));

    /* Program a cyclic eDMA1 ch0: fixed src = MICFIL DATACH0 -> memory ring. */
    qtest_writel(qts, CH(0) + TCD_SADDR, MICFIL_BASE + MICFIL_DATACH0);
    qtest_writew(qts, CH(0) + TCD_SOFF, 0);             /* peripheral fixed */
    qtest_writew(qts, CH(0) + TCD_ATTR, ATTR(2));       /* 4-byte elements */
    qtest_writel(qts, CH(0) + TCD_NBYTES, 4);   /* one word per minor loop */
    qtest_writel(qts, CH(0) + TCD_DADDR, (uint32_t)DST_GPA);
    qtest_writew(qts, CH(0) + TCD_DOFF, 4);
    qtest_writel(qts, CH(0) + TCD_DLAST, 0);
    /* CITER/BITER large so a major loop never completes during the test. */
    qtest_writew(qts, CH(0) + TCD_CITER, 0x100);
    qtest_writew(qts, CH(0) + TCD_BITER, 0x100);
    qtest_writew(qts, CH(0) + TCD_CSR, TCD_CSR_ESG | TCD_CSR_INTMAJ);
    qtest_writel(qts, CH(0) + CH_SBR, CH_SBR_RD);
    qtest_writel(qts, CH(0) + CH_CSR, CH_CSR_ERQ);      /* arm cyclic channel */

    /* Enable MICFIL (PDM, DMA request mode) with a small FIFO watermark. */
    qtest_writel(qts, MICFIL_BASE + MICFIL_FIFO_CTRL, 4);
    qtest_writel(qts, MICFIL_BASE + MICFIL_CTRL1,
                 CTRL1_PDMIEN | CTRL1_DISEL_DMA);

    /* Advance ~100 word periods: timer fills the FIFO and fires requests. */
    qtest_clock_step(qts, MICFIL_WORD_NS * 100);

    qtest_memread(qts, DST_GPA, got, sizeof(got));
    for (i = 0; i < (int)sizeof(got); i++) {
        if (got[i]) {
            nonzero++;
        }
    }
    /* A real (non-silent) capture must have moved bytes into the ring. */
    g_assert_cmpint(nonzero, >, 0);
    qtest_quit(qts);
}

/*
 * Cyclic playback on the 64-bit-TCD controller (edma2 / fsl,imx95-edma5),
 * no kernel: program a cyclic (ESG) channel that reads a RAM ring into SAI3
 * TDR0, enable SAI3 (TE + DMA request), and step the virtual clock so the SAI
 * TX tick drains its FIFO and pulses dma-req. Each request makes the eDMA move
 * one minor loop ring->TDR0 and advance SADDR. A SADDR that has advanced proves
 * the 64-bit TCD was decoded correctly (the bug: reading 32-bit-TCD offsets on
 * edma5 missed TCD_CSR.ESG, so the channel was never recognised as cyclic and
 * never advanced).
 */
static void test_edma_tcd64_playback(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest");
    uint8_t pat[256];
    uint32_t saddr_lo;
    int i;

    for (i = 0; i < (int)sizeof(pat); i++) {
        pat[i] = (uint8_t)(i * 3 + 1);
    }
    qtest_memwrite(qts, SRC_GPA, pat, sizeof(pat));

    /* Cyclic eDMA2 ch0 (64-bit TCD): RAM ring -> SAI3 TDR0 (fixed dest). */
    qtest_writel(qts, E2_CH(0) + T64_SADDR, (uint32_t)SRC_GPA);
    qtest_writel(qts, E2_CH(0) + T64_SADDR + 4, (uint32_t)(SRC_GPA >> 32));
    qtest_writew(qts, E2_CH(0) + T64_SOFF, 4);
    qtest_writew(qts, E2_CH(0) + T64_ATTR, ATTR(2));   /* 4-byte elements */
    qtest_writel(qts, E2_CH(0) + T64_NBYTES, 4);
    qtest_writel(qts, E2_CH(0) + T64_DADDR, (uint32_t)(SAI3_BASE + SAI_TDR0));
    qtest_writel(qts, E2_CH(0) + T64_DADDR + 4, 0);
    qtest_writew(qts, E2_CH(0) + T64_DOFF, 0);              /* TDR0 fixed */
    qtest_writel(qts, E2_CH(0) + T64_DLAST, 0);
    qtest_writew(qts, E2_CH(0) + T64_CITER, 0x100);
    qtest_writew(qts, E2_CH(0) + T64_BITER, 0x100);
    qtest_writew(qts, E2_CH(0) + T64_CSR, TCD_CSR_ESG | TCD_CSR_INTMAJ);
    qtest_writel(qts, E2_CH(0) + CH_SBR, CH_SBR_WR);   /* memory -> device */
    qtest_writel(qts, E2_CH(0) + CH_CSR, CH_CSR_ERQ);       /* arm cyclic */

    /* Enable SAI3 TX with DMA request and a 0 watermark. */
    qtest_writel(qts, SAI3_BASE + SAI_TCR1, 0);
    qtest_writel(qts, SAI3_BASE + SAI_TCSR, TCSR_TE | TCSR_FRDE);

    /* Advance time so the SAI TX tick fires and drives the eDMA. */
    qtest_clock_step(qts, (1000000000LL / 96000) * 64);

    /* The eDMA must have walked the ring: SADDR advanced past the start. */
    saddr_lo = qtest_readl(qts, E2_CH(0) + T64_SADDR);
    g_assert_cmpuint(saddr_lo, >, (uint32_t)SRC_GPA);

    qtest_quit(qts);
}

/*
 * Negative tests for the peripheral-triggered (cyclic/ESG) path, per
 * rt1180emulator's eDMA "ERQ audit": a model whose CH_CSR.ERQ is a dead bit,
 * or that runs a cyclic TCD without an actual peripheral DMA request, still
 * reports bytes moved - so a "did it complete?" test passes on a broken model.
 * These assert the ABSENCE of a transfer. (rt1180's third case - servicing a
 * request inline and silently dropping it while still reporting DONE - is that
 * model's own re-entrancy bug; this model services each dma-req synchronously
 * and has no such path, so it is not reproduced here.)
 */
static void program_micfil_cyclic_tcd(QTestState *qts)
{
    qtest_writel(qts, CH(0) + TCD_SADDR, MICFIL_BASE + MICFIL_DATACH0);
    qtest_writew(qts, CH(0) + TCD_SOFF, 0);
    qtest_writew(qts, CH(0) + TCD_ATTR, ATTR(2));
    qtest_writel(qts, CH(0) + TCD_NBYTES, 4);
    qtest_writel(qts, CH(0) + TCD_DADDR, (uint32_t)DST_GPA);
    qtest_writew(qts, CH(0) + TCD_DOFF, 4);
    qtest_writel(qts, CH(0) + TCD_DLAST, 0);
    qtest_writew(qts, CH(0) + TCD_CITER, 0x100);
    qtest_writew(qts, CH(0) + TCD_BITER, 0x100);
    qtest_writew(qts, CH(0) + TCD_CSR, TCD_CSR_ESG | TCD_CSR_INTMAJ);
    qtest_writel(qts, CH(0) + CH_SBR, CH_SBR_RD);
}

static int dst_nonzero_bytes(QTestState *qts)
{
    uint8_t got[64 * 4];
    int i, nz = 0;

    qtest_memread(qts, DST_GPA, got, sizeof(got));
    for (i = 0; i < (int)sizeof(got); i++) {
        if (got[i]) {
            nz++;
        }
    }
    return nz;
}

/* ERQ gates the peripheral path: cleared ERQ -> a DMA request is ignored. */
static void test_edma_cyclic_erq_gated(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest");
    uint8_t zero[64 * 4] = {0};

    qtest_memwrite(qts, DST_GPA, zero, sizeof(zero));
    program_micfil_cyclic_tcd(qts);
    qtest_writel(qts, CH(0) + CH_CSR, CH_CSR_ERQ);   /* arm: cyclic */
    qtest_writel(qts, CH(0) + CH_CSR, 0);            /* then CLEAR ERQ */

    /* MICFIL fires DMA requests, but with ERQ clear nothing may move. */
    qtest_writel(qts, MICFIL_BASE + MICFIL_FIFO_CTRL, 4);
    qtest_writel(qts, MICFIL_BASE + MICFIL_CTRL1,
                 CTRL1_PDMIEN | CTRL1_DISEL_DMA);
    qtest_clock_step(qts, MICFIL_WORD_NS * 100);

    g_assert_cmpint(dst_nonzero_bytes(qts), ==, 0);
    qtest_quit(qts);
}

/* A cyclic channel needs the request line: ERQ set, no request = no move. */
static void test_edma_cyclic_needs_request(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest");
    uint8_t pat[64], zero[64 * 4] = {0};
    int i;

    /*
     * Source from a NON-ZERO RAM pattern, not a disabled peripheral: a disabled
     * MICFIL supplies zeros, so a model that wrongly ran the TCD on arm would
     * copy zeros and this test would pass anyway (vacuous). With a pattern,
     * a no-request transfer lands visible bytes and the assertion catches it.
     */
    for (i = 0; i < (int)sizeof(pat); i++) {
        pat[i] = (uint8_t)(0xA0 + i);
    }
    qtest_memwrite(qts, SRC_GPA, pat, sizeof(pat));
    qtest_memwrite(qts, DST_GPA, zero, sizeof(zero));

    /* Cyclic (ESG) channel, RAM src -> DST, armed with ERQ - but no dma-req. */
    qtest_writel(qts, CH(0) + TCD_SADDR, (uint32_t)SRC_GPA);
    qtest_writew(qts, CH(0) + TCD_SOFF, 4);
    qtest_writew(qts, CH(0) + TCD_ATTR, ATTR(2));
    qtest_writel(qts, CH(0) + TCD_NBYTES, 4);
    qtest_writel(qts, CH(0) + TCD_DADDR, (uint32_t)DST_GPA);
    qtest_writew(qts, CH(0) + TCD_DOFF, 4);
    qtest_writel(qts, CH(0) + TCD_DLAST, 0);
    qtest_writew(qts, CH(0) + TCD_CITER, 0x100);
    qtest_writew(qts, CH(0) + TCD_BITER, 0x100);
    qtest_writew(qts, CH(0) + TCD_CSR, TCD_CSR_ESG | TCD_CSR_INTMAJ);
    qtest_writel(qts, CH(0) + CH_SBR, CH_SBR_RD);
    qtest_writel(qts, CH(0) + CH_CSR, CH_CSR_ERQ);   /* armed, ERQ set */

    /* No peripheral is enabled: no dma-req ever pulses. Nothing may move. */
    qtest_clock_step(qts, MICFIL_WORD_NS * 100);

    g_assert_cmpint(dst_nonzero_bytes(qts), ==, 0);
    qtest_quit(qts);
}

/*
 * Reset-value differential (the fleet "memset-to-0 is a claim every bit is 0"
 * audit). CH_SBR is NOT zero at reset: the RM gives eDMA3/eDMA5 a 0x0000_8007
 * reset - PAL=1 (bit15, privileged) and MID=7 (bits[3:0], the bus master ID).
 * It matters because fsl-edma READ-MODIFY-WRITES CH_SBR to set its RD/WR
 * direction bit, so a zero reset is laundered into the guest's own config as
 * MID=0 / non-privileged - wrong on silicon (QEMU enforces no MID, so it is
 * silently wrong here). Mutation-proof: delete the CH_SBR seed in
 * imx95_edma_reset() and every assert below goes red.
 */
#define CH_SBR_MID      0x0000000fu             /* bus master ID, bits[3:0] */
#define CH_SBR_PAL      (1u << 15)              /* privileged access level  */

static void test_edma_ch_sbr_reset(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest");
    uint32_t v;

    /*
     * edma1 (eDMA3) and edma2 (eDMA5): both reset CH_SBR to 0x8007. Check a
     * few channels - the seed is per-channel, not just channel 0.
     */
    v = qtest_readl(qts, CH(0) + CH_SBR);
    g_assert_cmphex(v, ==, 0x8007);
    g_assert_cmphex(v & CH_SBR_MID, ==, 7);     /* NOT laundered to 0 */
    g_assert_cmphex(v & CH_SBR_PAL, ==, CH_SBR_PAL);
    g_assert_cmphex(qtest_readl(qts, CH(1) + CH_SBR), ==, 0x8007);
    g_assert_cmphex(qtest_readl(qts, CH(30) + CH_SBR), ==, 0x8007);
    g_assert_cmphex(qtest_readl(qts, E2_CH(0) + CH_SBR), ==, 0x8007);
    g_assert_cmphex(qtest_readl(qts, E2_CH(63) + CH_SBR), ==, 0x8007);

    /*
     * Reproduce fsl-edma's exact RMW (val = read; val |= RD; write) and prove
     * the MID+PAL SURVIVE - the whole point of a correct reset value.
     */
    v = qtest_readl(qts, CH(0) + CH_SBR);
    qtest_writel(qts, CH(0) + CH_SBR, v | CH_SBR_RD);
    v = qtest_readl(qts, CH(0) + CH_SBR);
    g_assert_cmphex(v & CH_SBR_MID, ==, 7);         /* still master ID 7 */
    g_assert_cmphex(v & CH_SBR_PAL, ==, CH_SBR_PAL);/* still privileged */
    g_assert_cmphex(v & CH_SBR_RD, ==, CH_SBR_RD);  /* direction applied */
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx95/edma/plain", test_edma_plain);
    qtest_add_func("/imx95/edma/mloff", test_edma_mloff);
    qtest_add_func("/imx95/edma/micfil-cyclic", test_edma_micfil_cyclic);
    qtest_add_func("/imx95/edma/tcd64-playback", test_edma_tcd64_playback);
    qtest_add_func("/imx95/edma/cyclic-erq-gated", test_edma_cyclic_erq_gated);
    qtest_add_func("/imx95/edma/cyclic-needs-request",
                   test_edma_cyclic_needs_request);
    qtest_add_func("/imx95/edma/ch-sbr-reset", test_edma_ch_sbr_reset);
    return g_test_run();
}
