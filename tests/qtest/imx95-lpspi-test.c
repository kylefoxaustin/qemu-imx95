/*
 * QTest for the i.MX 95 LPSPI master (hw/ssi/imx95_lpspi.c).
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Attaches an ISSI serial-NOR (is25lp064) to the lpspi1 SSI bus and reads its
 * JEDEC ID (RDID, 0x9F) through the controller: enable, set an 8-bit frame,
 * push the command + three clocking bytes via TDR, and pop the shifted-in ID
 * bytes from RDR. A correct manufacturer byte proves the TDR -> ssi_transfer
 * -> RDR datapath end to end, with no kernel.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define LPSPI1_BASE   0x44360000ULL

#define LPSPI_CR      0x10
#define LPSPI_SR      0x14
#define LPSPI_TCR     0x60
#define LPSPI_TDR     0x64
#define LPSPI_RSR     0x70
#define LPSPI_RDR     0x74

#define CR_MEN        (1u << 0)
#define RSR_RXEMPTY   (1u << 1)

#define RDID          0x9f
#define ISSI_MFG      0x9d      /* is25* manufacturer id */

static uint32_t xfer(QTestState *qts, uint8_t byte)
{
    qtest_writel(qts, LPSPI1_BASE + LPSPI_TDR, byte);
    /* RX FIFO now holds the shifted-in word; pop it. */
    if (qtest_readl(qts, LPSPI1_BASE + LPSPI_RSR) & RSR_RXEMPTY) {
        return 0xffffffff;
    }
    return qtest_readl(qts, LPSPI1_BASE + LPSPI_RDR);
}

static void test_lpspi_jedec(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest "
                                  "-device is25lp064,bus=lpspi1");
    uint32_t mfg, type, cap;

    qtest_writel(qts, LPSPI1_BASE + LPSPI_CR, CR_MEN);
    qtest_writel(qts, LPSPI1_BASE + LPSPI_TCR, 7);      /* 8-bit frames */

    xfer(qts, RDID);                /* command; response byte is don't-care */
    mfg  = xfer(qts, 0x00) & 0xff;
    type = xfer(qts, 0x00) & 0xff;
    cap  = xfer(qts, 0x00) & 0xff;

    g_assert_cmphex(mfg, ==, ISSI_MFG);
    g_assert_cmpuint(type, !=, 0);
    g_assert_cmpuint(cap, !=, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx95/lpspi/jedec", test_lpspi_jedec);
    return g_test_run();
}
