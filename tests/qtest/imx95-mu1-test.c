/*
 * QTest for the i.MX 95 MU1 AP-S SCMI channel (hw/misc/imx_mu.c, wired in
 * hw/arm/fsl-imx95.c).
 *
 * Copyright (c) 2026, Schreibikus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * TF-A's SCMI channel to the System Manager runs over MU1: TF-A uses the
 * A side (0x44220000) and its SMT page at 0x44221000, the SM the B side
 * (0x44230000) and the SMT page at 0x44231000. Both pages must be the same
 * RAM, and a GCR.GIRn doorbell on one side must latch GSR.GIPn on the other,
 * or neither end ever sees the other's messages.
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "libqtest.h"

#define MU1_A           0x44220000ULL
#define MU1_B           0x44230000ULL
#define MU1_SHMEM_A     0x44221000ULL
#define MU1_SHMEM_B     0x44231000ULL

/* MU V2 registers (include/hw/misc/imx_mu.h). */
#define MU_GCR          0x114
#define MU_GSR          0x118
#define MU_TSR          0x124
#define MU_RSR          0x12c
#define MU_TR(n)        (0x200 + 4 * (n))
#define MU_RR(n)        (0x280 + 4 * (n))

/* SMT layout: msg_header at +0x18; the AP semaphore sits at +0x3f8. */
#define SMT_MSG_HDR     0x18
#define SMT_AP_SEMA     0x3f8

static QTestState *mu1_init(void)
{
    return qtest_init("-machine imx95-19x19-evk -m 1G -accel qtest");
}

static void test_shmem_alias(void)
{
    QTestState *qts = mu1_init();

    /* A-side write, B-side read. */
    qtest_writel(qts, MU1_SHMEM_A + SMT_MSG_HDR, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, MU1_SHMEM_B + SMT_MSG_HDR), ==,
                    0x12345678);

    /* B-side write, A-side read. */
    qtest_writel(qts, MU1_SHMEM_B + SMT_AP_SEMA, 0xcafef00d);
    g_assert_cmphex(qtest_readl(qts, MU1_SHMEM_A + SMT_AP_SEMA), ==,
                    0xcafef00d);

    /* Last word of the 4 KiB page, both ways round. */
    qtest_writel(qts, MU1_SHMEM_A + 0xffc, 0x0badc0de);
    g_assert_cmphex(qtest_readl(qts, MU1_SHMEM_B + 0xffc), ==, 0x0badc0de);
    qtest_writel(qts, MU1_SHMEM_B + 0xffc, 0x5a5a5a5a);
    g_assert_cmphex(qtest_readl(qts, MU1_SHMEM_A + 0xffc), ==, 0x5a5a5a5a);

    qtest_quit(qts);
}

/* GCR.GIRn on @from must set GSR.GIPn on @to only. */
static void check_doorbell(QTestState *qts, uint64_t from, uint64_t to,
                           unsigned int n)
{
    g_assert_cmphex(qtest_readl(qts, to + MU_GSR), ==, 0);

    qtest_writel(qts, from + MU_GCR, BIT(n));
    g_assert_cmphex(qtest_readl(qts, to + MU_GSR), ==, BIT(n));
    g_assert_cmphex(qtest_readl(qts, from + MU_GSR), ==, 0);
    /* The request bit self-clears once delivered. */
    g_assert_cmphex(qtest_readl(qts, from + MU_GCR), ==, 0);

    /* GSR.GIPn is write-1-to-clear. */
    qtest_writel(qts, to + MU_GSR, BIT(n));
    g_assert_cmphex(qtest_readl(qts, to + MU_GSR), ==, 0);
}

static void test_doorbell(void)
{
    QTestState *qts = mu1_init();

    check_doorbell(qts, MU1_A, MU1_B, 0);   /* TF-A -> SM, channel 0 */
    check_doorbell(qts, MU1_A, MU1_B, 1);   /* TF-A -> SM, channel 1 */
    check_doorbell(qts, MU1_B, MU1_A, 0);   /* SM -> TF-A */

    qtest_quit(qts);
}

static void test_tr_to_peer_rr(void)
{
    QTestState *qts = mu1_init();

    /* A TR[0] lands in B RR[0]; A's TX slot stays full until B drains it. */
    qtest_writel(qts, MU1_A + MU_TR(0), 0xa5a50001);
    g_assert_cmphex(qtest_readl(qts, MU1_A + MU_TSR) & BIT(0), ==, 0);
    g_assert_cmphex(qtest_readl(qts, MU1_B + MU_RSR) & BIT(0), ==, BIT(0));
    g_assert_cmphex(qtest_readl(qts, MU1_B + MU_RR(0)), ==, 0xa5a50001);
    g_assert_cmphex(qtest_readl(qts, MU1_B + MU_RSR) & BIT(0), ==, 0);
    g_assert_cmphex(qtest_readl(qts, MU1_A + MU_TSR) & BIT(0), ==, BIT(0));

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx95/mu1/shmem-alias", test_shmem_alias);
    qtest_add_func("/imx95/mu1/doorbell", test_doorbell);
    qtest_add_func("/imx95/mu1/tr-to-peer-rr", test_tr_to_peer_rr);
    return g_test_run();
}
