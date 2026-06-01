/*
 * QTest for the NXP ENETC v4 (NETC) Ethernet PF model on the i.MX 95 machine.
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Drives the RX BD-ring DMA path directly, with no kernel: bring the PF's
 * BAR0 up via ECAM config writes, post a small RX ring whose per-buffer size
 * (RBBSR) is deliberately tiny, inject a frame over the socket backend, and
 * verify the model scatters it across the expected number of BDs - each
 * non-final BD full and only the last carrying the F (final) flag. This
 * exercises the multi-buffer RX scatter that a 1500-MTU slirp backend cannot.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/sockets.h"
#include "qemu/iov.h"
#include "libqtest-single.h"

/* NETC ECAM + the modelled PF (devfn 08.0) BAR window. */
#define NETC_ECAM_BASE   0x4ca00000
#define ENETC_DEVFN      0x40            /* PCI_DEVFN(8, 0) */
#define ENETC_CFG_BASE   (NETC_ECAM_BASE + (ENETC_DEVFN << 12))
#define ENETC_BAR0       0x4cc00000      /* where we map BAR0 (in the window) */

/* PCI config offsets. */
#define PCI_CMD          0x04
#define PCI_CMD_MEM_MASTER 0x06
#define PCI_BAR0         0x10

/* ENETC BAR0 register layout (mirrors hw/net/fsl_enetc.c). */
#define BDR_BASE         0x08000
#define RX_BDR           (0x100 + 0 * 0x200)   /* t=1 (RX), i=0 */
#define RBMR             0x00
#define RBBSR            0x08
#define RBCIR            0x0c
#define RBBAR0           0x10
#define RBBAR1           0x14
#define RBPIR            0x18
#define RBLENR           0x20
#define MR_EN            (1u << 31)
#define RXBD_LSTATUS_F   (1u << 31)

#define RXREG(off)       (ENETC_BAR0 + BDR_BASE + RX_BDR + (off))

/* Guest DRAM scratch (0x8000_0000+). Keep BDs and buffers well clear. */
#define RAM_BASE         0x80000000ULL
#define RING_GPA         (RAM_BASE + 0x01000000)   /* BD ring base */
#define BUF_GPA          (RAM_BASE + 0x02000000)   /* RX buffer pool */
#define BD_SIZE          16
#define RING_LEN         64                        /* BDs in the ring */
#define BUFSZ            256                       /* tiny: forces scatter */

static void enetc_cfg_writel(QTestState *qts, uint32_t off, uint32_t val)
{
    qtest_writel(qts, ENETC_CFG_BASE + off, val);
}

static uint32_t enetc_cfg_readl(QTestState *qts, uint32_t off)
{
    return qtest_readl(qts, ENETC_CFG_BASE + off);
}

/* Bring BAR0 up and enable MEM decode + bus mastering. */
static void enetc_setup_bar(QTestState *qts)
{
    uint32_t id = enetc_cfg_readl(qts, 0x00);

    /* PCI 1131:e101 must be present at devfn 08.0. */
    g_assert_cmphex(id, ==, 0xe1011131);

    /* BAR0 is a 64-bit memory BAR; program low/high and enable. */
    enetc_cfg_writel(qts, PCI_BAR0, ENETC_BAR0);
    enetc_cfg_writel(qts, PCI_BAR0 + 4, 0);
    enetc_cfg_writel(qts, PCI_CMD, PCI_CMD_MEM_MASTER);
}

/* Post `n` empty RX BDs pointing at successive BUFSZ-spaced buffers. */
static void enetc_post_rx_ring(QTestState *qts, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < RING_LEN; i++) {
        uint8_t bd[BD_SIZE] = { 0 };
        uint64_t buf = BUF_GPA + (uint64_t)i * BUFSZ;

        /* RX BD posted form: { __le64 addr, resv[8] }. */
        stq_le_p(bd, buf);
        qtest_memwrite(qts, RING_GPA + (uint64_t)i * BD_SIZE, bd, sizeof(bd));
    }

    /* Ring base, length, per-buffer size, then enable; post n buffers. */
    qtest_writel(qts, RXREG(RBBAR0), (uint32_t)RING_GPA);
    qtest_writel(qts, RXREG(RBBAR1), (uint32_t)(RING_GPA >> 32));
    qtest_writel(qts, RXREG(RBLENR), RING_LEN);
    qtest_writel(qts, RXREG(RBBSR), BUFSZ);
    qtest_writel(qts, RXREG(RBMR), MR_EN);
    /* RBCIR = driver's next_to_use, one past the last posted buffer. */
    qtest_writel(qts, RXREG(RBCIR), n);
}

/* Inject one L2 frame into the device via the socket backend. */
static void enetc_inject_frame(int fd, const uint8_t *frame, size_t flen)
{
    uint32_t be_len = htonl(flen);
    struct iovec iov[] = {
        { .iov_base = &be_len, .iov_len = sizeof(be_len) },
        { .iov_base = (void *)frame, .iov_len = flen },
    };
    ssize_t ret = iov_send(fd, iov, 2, 0, sizeof(be_len) + flen);

    g_assert_cmpint(ret, ==, sizeof(be_len) + flen);
}

/*
 * Inject one frame, then verify the model wrote it back across
 * ceil(flen / BUFSZ) BDs: each non-final BD full (buf_len == BUFSZ, no F),
 * the final BD holding the remainder with F set, and the concatenated buffers
 * reproducing the frame byte for byte.
 *
 * flen <= BUFSZ exercises the single-BD common path; flen > BUFSZ exercises
 * the multi-buffer scatter that a 1500-MTU slirp backend cannot reach. The
 * buf_len check at BD offset 8 (not 12) also guards the RX writeback layout.
 */
static void check_rx_frame(uint32_t flen)
{
    int *sv = g_new(int, 2);
    QTestState *qts;
    uint8_t frame[2048];
    uint32_t expect_bds = (flen + BUFSZ - 1) / BUFSZ;
    uint8_t got[sizeof(frame)];
    uint32_t i, off;

    g_assert_cmpuint(flen, <=, sizeof(frame));
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);

    qts = qtest_initf("-machine imx95-19x19-evk -accel qtest "
                      "-nic socket,fd=%d,model=fsl-enetc", sv[1]);
    close(sv[1]);

    /* Deterministic payload. */
    for (i = 0; i < flen; i++) {
        frame[i] = (uint8_t)(i * 7 + 3);
    }

    enetc_setup_bar(qts);
    enetc_post_rx_ring(qts, RING_LEN - 1);

    enetc_inject_frame(sv[0], frame, flen);

    /* Let the device's bottom half run the receive() DMA. */
    qtest_clock_step(qts, 1000000);

    /* Walk the BDs and reassemble. */
    off = 0;
    for (i = 0; i < expect_bds; i++) {
        uint8_t bd[BD_SIZE];
        uint16_t buf_len;
        uint32_t lstatus;
        uint64_t buf = BUF_GPA + (uint64_t)i * BUFSZ;
        bool final = (i == expect_bds - 1);
        uint32_t want = final ? flen - off : BUFSZ;

        qtest_memread(qts, RING_GPA + (uint64_t)i * BD_SIZE, bd, sizeof(bd));
        buf_len = lduw_le_p(bd + 8);
        lstatus = ldl_le_p(bd + 12);

        g_assert_cmpuint(buf_len, ==, want);
        if (final) {
            g_assert_cmphex(lstatus & RXBD_LSTATUS_F, ==, RXBD_LSTATUS_F);
        } else {
            g_assert_cmphex(lstatus & RXBD_LSTATUS_F, ==, 0);
        }

        qtest_memread(qts, buf, got + off, want);
        off += want;
    }
    g_assert_cmpuint(off, ==, flen);
    g_assert_false(memcmp(got, frame, flen));

    qtest_quit(qts);
    close(sv[0]);
    g_free(sv);
}

/* Single-buffer frame: the common path (flen < RBBSR). */
static void test_rx_single(void)
{
    check_rx_frame(128);
}

/*
 * Ring wraparound under sustained RX. Post the full ring, then inject many
 * single-buffer frames one at a time, advancing RBCIR by one after each (the
 * driver handing the just-consumed buffer back) so a free BD is always
 * available. With N frames > RING_LEN this drives the producer index around
 * the ring several times; verify every frame lands in the BD at the expected
 * wrapping index with its content intact, and that RBPIR ends where the
 * model's modular arithmetic should leave it. This is the failure mode a
 * slow, RTT-bound slirp ping can never reach: a boundary/off-by-one at the
 * ring wrap or a stale F-flag from a prior lap.
 */
static void test_rx_wraparound(void)
{
    int *sv = g_new(int, 2);
    QTestState *qts;
    const uint32_t NFRAMES = RING_LEN * 4 + 5;   /* 4+ full laps of the ring */
    uint32_t flen = 64;                          /* single-buffer frames */
    uint8_t frame[64];
    uint8_t got[64];
    uint32_t f, i, rbcir;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-machine imx95-19x19-evk -accel qtest "
                      "-nic socket,fd=%d,model=fsl-enetc", sv[1]);
    close(sv[1]);

    enetc_setup_bar(qts);
    /*
     * Post a full ring of empty buffers (RBCIR = RING_LEN - 1, one slot held
     * back as the always-empty separator the driver convention keeps).
     */
    enetc_post_rx_ring(qts, RING_LEN - 1);
    rbcir = RING_LEN - 1;

    for (f = 0; f < NFRAMES; f++) {
        uint32_t idx = f % RING_LEN;        /* BD this frame must land in */
        uint64_t bd_addr = RING_GPA + (uint64_t)idx * BD_SIZE;
        uint64_t buf_addr = BUF_GPA + (uint64_t)idx * BUFSZ;
        uint8_t bd[BD_SIZE];
        uint16_t buf_len;
        uint32_t lstatus;

        /* Distinct per-frame payload so a stale buffer would be caught. */
        for (i = 0; i < flen; i++) {
            frame[i] = (uint8_t)(f * 31 + i);
        }
        enetc_inject_frame(sv[0], frame, flen);
        qtest_clock_step(qts, 1000000);

        /* The BD at idx must now show this frame: F set, length, bytes. */
        qtest_memread(qts, bd_addr, bd, sizeof(bd));
        buf_len = lduw_le_p(bd + 8);
        lstatus = ldl_le_p(bd + 12);
        g_assert_cmpuint(buf_len, ==, flen);
        g_assert_cmphex(lstatus & RXBD_LSTATUS_F, ==, RXBD_LSTATUS_F);
        qtest_memread(qts, buf_addr, got, flen);
        if (memcmp(got, frame, flen) != 0) {
            g_test_message("frame %u (ring idx %u) corrupted", f, idx);
            g_assert_not_reached();
        }

        /*
         * Re-post the BD the model just consumed, exactly as the driver does
         * before handing a buffer back: rewrite the posted "empty" form
         * { __le64 addr, resv[8] } so its addr field (offset 0) is the buffer
         * pointer again. The model's receive() reads buf_addr from offset 0;
         * the writeback we just verified clobbered it, so without re-posting,
         * the NEXT lap would read a bogus pointer. Then advance RBCIR by one,
         * wrapping at RING_LEN, staying exactly one ahead of the producer.
         */
        {
            uint8_t empty[BD_SIZE] = { 0 };

            stq_le_p(empty, buf_addr);
            qtest_memwrite(qts, bd_addr, empty, sizeof(empty));
        }
        rbcir = (rbcir + 1) % RING_LEN;
        qtest_writel(qts, RXREG(RBCIR), rbcir);
    }

    /* After NFRAMES, the producer index (RBPIR) is NFRAMES % RING_LEN. */
    g_assert_cmpuint(qtest_readl(qts, RXREG(RBPIR)) & 0xffff,
                     ==, NFRAMES % RING_LEN);

    qtest_quit(qts);
    close(sv[0]);
    g_free(sv);
}

/* Multi-buffer frame: scatter across 3 BDs (256 + 256 + 188). */
static void test_rx_scatter(void)
{
    check_rx_frame(700);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/fsl-enetc/rx-single", test_rx_single);
    qtest_add_func("/fsl-enetc/rx-scatter", test_rx_scatter);
    qtest_add_func("/fsl-enetc/rx-wraparound", test_rx_wraparound);
    return g_test_run();
}
