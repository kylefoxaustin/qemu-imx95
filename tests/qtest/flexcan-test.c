/*
 * QTest for the NXP FlexCAN controller model (on the i.MX 95 machine).
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Exercises the two driver-critical behaviours of hw/net/can/flexcan.c
 * without a kernel: the MCR freeze/halt/soft-reset ack handshake, and a
 * real frame TX from one FlexCAN to RX on a second FlexCAN sharing one
 * emulated CAN bus.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

/* i.MX 95 FlexCAN1/FlexCAN2 register bases. */
#define FC1   0x443a0000
#define FC2   0x425b0000

/* Register offsets. */
#define MCR       0x00
#define IFLAG2    0x2c
#define IFLAG1    0x30
#define MB(i)     (0x80 + (i) * 16)   /* classic: 16-byte MBs, bank0 */
#define MB_CS(i)  (MB(i) + 0)
#define MB_ID(i)  (MB(i) + 4)
#define MB_D0(i)  (MB(i) + 8)
#define TX_MB     0x470               /* classic TX mailbox (index 63) */

/* MCR bits. */
#define MCR_MDIS     (1u << 31)
#define MCR_FRZ      (1u << 30)
#define MCR_HALT     (1u << 28)
#define MCR_SOFTRST  (1u << 25)
#define MCR_FRZ_ACK  (1u << 24)
#define MCR_LPM_ACK  (1u << 20)

/* MB CS CODE field. */
#define CODE_RX_EMPTY  (0x4u << 24)
#define CODE_RX_FULL   (0x2u << 24)
#define CODE_TX_DATA   (0xcu << 24)
#define CODE_MASK      (0xfu << 24)

static void test_mcr_handshake(void)
{
    QTestState *qts = qtest_init("-machine imx95-19x19-evk -accel qtest");

    /* MDIS -> LPM_ACK tracks it (low-power enter/exit ack). */
    qtest_writel(qts, FC1 + MCR, MCR_MDIS);
    g_assert_true(qtest_readl(qts, FC1 + MCR) & MCR_LPM_ACK);
    qtest_writel(qts, FC1 + MCR, 0);
    g_assert_false(qtest_readl(qts, FC1 + MCR) & MCR_LPM_ACK);

    /* SOFTRST is self-clearing. */
    qtest_writel(qts, FC1 + MCR, MCR_SOFTRST);
    g_assert_false(qtest_readl(qts, FC1 + MCR) & MCR_SOFTRST);

    /* FRZ+HALT -> FRZ_ACK set; clearing HALT -> FRZ_ACK clears. */
    qtest_writel(qts, FC1 + MCR, MCR_FRZ | MCR_HALT);
    g_assert_true(qtest_readl(qts, FC1 + MCR) & MCR_FRZ_ACK);
    qtest_writel(qts, FC1 + MCR, MCR_FRZ);
    g_assert_false(qtest_readl(qts, FC1 + MCR) & MCR_FRZ_ACK);

    qtest_quit(qts);
}

static void test_tx_rx(void)
{
    /* Put FlexCAN1 and FlexCAN2 on the same emulated CAN bus. */
    QTestState *qts = qtest_init("-machine imx95-19x19-evk -accel qtest "
                                 "-object can-bus,id=cb "
                                 "-machine canbus0=cb,canbus1=cb");
    uint32_t cs, id, d0;

    /* Receiver: arm RX mailbox 1 as EMPTY. */
    qtest_writel(qts, FC2 + MB_CS(1), CODE_RX_EMPTY);

    /* Sender: standard ID 0x123, 4 data bytes 0xDEADBEEF; CS write fires TX. */
    qtest_writel(qts, FC1 + TX_MB + 8, 0xdeadbeef);
    qtest_writel(qts, FC1 + TX_MB + 4, 0x123u << 18);
    qtest_writel(qts, FC1 + TX_MB + 0, CODE_TX_DATA | (4u << 16));

    /* Sender latched its TX-complete flag (TX mailbox 63 -> IFLAG2 bit 31). */
    g_assert_true(qtest_readl(qts, FC1 + IFLAG2) & (1u << 31));

    /* Receiver got it in mailbox 1: IFLAG1 bit 1, CODE=RX_FULL, id+data. */
    g_assert_true(qtest_readl(qts, FC2 + IFLAG1) & (1u << 1));
    cs = qtest_readl(qts, FC2 + MB_CS(1));
    g_assert_cmphex(cs & CODE_MASK, ==, CODE_RX_FULL);
    id = qtest_readl(qts, FC2 + MB_ID(1));
    g_assert_cmphex((id >> 18) & 0x7ff, ==, 0x123);
    d0 = qtest_readl(qts, FC2 + MB_D0(1));
    g_assert_cmphex(d0, ==, 0xdeadbeef);

    /* Clearing the RX flag (W1C) re-arms the mailbox to EMPTY. */
    qtest_writel(qts, FC2 + IFLAG1, 1u << 1);
    g_assert_false(qtest_readl(qts, FC2 + IFLAG1) & (1u << 1));
    g_assert_cmphex(qtest_readl(qts, FC2 + MB_CS(1)) & CODE_MASK, ==,
                    CODE_RX_EMPTY);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/flexcan/mcr-handshake", test_mcr_handshake);
    qtest_add_func("/flexcan/tx-rx", test_tx_rx);
    return g_test_run();
}
