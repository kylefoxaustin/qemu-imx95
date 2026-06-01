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
#include "qemu/cutils.h"
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

/*
 * High-volume TX->RX: blast many frames FC1 -> FC2 through the shared CAN bus,
 * each with a distinct ID and payload, re-arming the RX mailbox (W1C clear)
 * between frames. Verify every frame arrives intact, in order, with no loss or
 * corruption across the run. This stresses the mailbox handoff + W1C re-arm
 * path at volume - the FlexCAN analog of a sustained-traffic test (FlexCAN is
 * mailbox-based with no DMA ring, so there is no index to wrap; the risk this
 * guards is a stuck flag or a stale mailbox after many back-to-back frames).
 *
 * Count is configurable via FLEXCAN_STRESS_FRAMES (default 1000, CI-fast).
 */
static void test_tx_rx_volume(void)
{
    QTestState *qts = qtest_init("-machine imx95-19x19-evk -accel qtest "
                                 "-object can-bus,id=cb "
                                 "-machine canbus0=cb,canbus1=cb");
    const char *env = getenv("FLEXCAN_STRESS_FRAMES");
    unsigned long n = 1000;
    unsigned long f;

    if (env) {
        qemu_strtoul(env, NULL, 10, &n);
    }

    /* Arm RX mailbox 1 as EMPTY once; we re-arm it via W1C after each frame. */
    qtest_writel(qts, FC2 + MB_CS(1), CODE_RX_EMPTY);

    for (f = 0; f < n; f++) {
        uint32_t can_id = 0x100 + (f & 0x6ff);  /* 11-bit standard ID */
        uint32_t payload = 0xc0000000u ^ (uint32_t)(f * 2654435761u);
        uint32_t cs, id, d0;

        /* Sender: distinct ID + payload; CS write fires the TX. */
        qtest_writel(qts, FC1 + TX_MB + 8, payload);
        qtest_writel(qts, FC1 + TX_MB + 4, can_id << 18);
        qtest_writel(qts, FC1 + TX_MB + 0, CODE_TX_DATA | (4u << 16));

        /* Sender TX-complete flag set. */
        g_assert_true(qtest_readl(qts, FC1 + IFLAG2) & (1u << 31));
        /* clear it for the next iteration (W1C) */
        qtest_writel(qts, FC1 + IFLAG2, 1u << 31);

        /* Receiver got exactly this frame. */
        if (!(qtest_readl(qts, FC2 + IFLAG1) & (1u << 1))) {
            g_test_message("frame %lu: RX flag not set (frame lost)", f);
            g_assert_not_reached();
        }
        cs = qtest_readl(qts, FC2 + MB_CS(1));
        g_assert_cmphex(cs & CODE_MASK, ==, CODE_RX_FULL);
        id = qtest_readl(qts, FC2 + MB_ID(1));
        d0 = qtest_readl(qts, FC2 + MB_D0(1));
        if (((id >> 18) & 0x7ff) != can_id || d0 != payload) {
            g_test_message("frame %lu: id/payload mismatch "
                           "(got id 0x%x data 0x%x, want id 0x%x data 0x%x)",
                           f, (id >> 18) & 0x7ff, d0, can_id, payload);
            g_assert_not_reached();
        }

        /* Re-arm: W1C the RX flag, mailbox returns to EMPTY for the next. */
        qtest_writel(qts, FC2 + IFLAG1, 1u << 1);
        g_assert_cmphex(qtest_readl(qts, FC2 + MB_CS(1)) & CODE_MASK, ==,
                        CODE_RX_EMPTY);
    }

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/flexcan/mcr-handshake", test_mcr_handshake);
    qtest_add_func("/flexcan/tx-rx", test_tx_rx);
    qtest_add_func("/flexcan/tx-rx-volume", test_tx_rx_volume);
    return g_test_run();
}
