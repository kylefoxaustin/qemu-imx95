/*
 * QTest for the i.MX 95 ELE responder (hw/misc/imx95_ele_server.c) and the
 * fsl-imx95 "soc-rev" property.
 *
 * Copyright (c) 2026, Schreibikus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Talks to the EdgeLock Enclave responders the way U-Boot and TF-A do: write
 * the request words into the MU's TR registers, then poll RSR and read the
 * reply from RR. GET_INFO must write the silicon revision chosen with
 * "soc-rev" into the guest's ele_get_info_data buffer, on every ELE MU the
 * machine wires a responder to. TRNG_STATE must report the TRNG and the ELE
 * RNG context ready in a 3-word reply, or OP-TEE panics ("ELE RNG is busy").
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* ELE MUs with a responder; 0x47540000 is TF-A's secure channel. */
#define ELE_MU_TFA      0x47540000ULL
static const uint64_t ele_mus[] = {
    0x47520000ULL, 0x47530000ULL, ELE_MU_TFA, 0x47550000ULL,
};

/* MU V2 registers (include/hw/misc/imx_mu.h). */
#define MU_RSR          0x12c
#define MU_TR(n)        (0x200 + 4 * (n))
#define MU_RR(n)        (0x280 + 4 * (n))

/* ELE header: version[7:0] | size[15:8] | command[23:16] | tag[31:24]. */
#define ELE_HDR(ver, size, cmd, tag) \
    ((uint32_t)(ver) | ((size) << 8) | ((cmd) << 16) | ((uint32_t)(tag) << 24))
#define ELE_VER             0x06
#define ELE_CMD_TAG         0x17
#define ELE_RESP_TAG        0xe1
#define ELE_SUCCESS_IND     0xd6
#define ELE_GET_INFO_REQ    0xda
#define ELE_TRNG_STATE_REQ  0xa4

/* struct ele_get_info_data: 64 words; soc at word 1, lc at word 2. */
#define INFO_BUF        0x90000000ULL
#define INFO_SIZE       256
#define INFO_SOC        4
#define INFO_LC         8
#define INFO_LC_OEM_OPEN 0x80

#define SOC_WORD(rev)   (((uint32_t)(rev) << 24) | 0x9500)

static QTestState *ele_init(const char *soc_rev)
{
    return qtest_initf("-machine imx95-19x19-evk -m 1G -accel qtest%s%s",
                       soc_rev ? " -global fsl-imx95.soc-rev=" : "",
                       soc_rev ? soc_rev : "");
}

/* Send GET_INFO on @mu and return the soc word the ELE wrote back. */
static uint32_t ele_get_info(QTestState *qts, uint64_t mu)
{
    uint8_t fill[INFO_SIZE];

    /* Poison the buffer so a missing write cannot pass as a zero field. */
    memset(fill, 0xa5, sizeof(fill));
    qtest_memwrite(qts, INFO_BUF, fill, sizeof(fill));

    qtest_writel(qts, mu + MU_TR(0),
                 ELE_HDR(ELE_VER, 4, ELE_GET_INFO_REQ, ELE_CMD_TAG));
    qtest_writel(qts, mu + MU_TR(1), INFO_BUF >> 32);
    qtest_writel(qts, mu + MU_TR(2), (uint32_t)INFO_BUF);
    qtest_writel(qts, mu + MU_TR(3), INFO_SIZE);

    /* 2-word reply: header + status. */
    g_assert_cmphex(qtest_readl(qts, mu + MU_RSR) & 0x3, ==, 0x3);
    g_assert_cmphex(qtest_readl(qts, mu + MU_RR(0)), ==,
                    ELE_HDR(ELE_VER, 2, ELE_GET_INFO_REQ, ELE_RESP_TAG));
    g_assert_cmphex(qtest_readl(qts, mu + MU_RR(1)), ==, ELE_SUCCESS_IND);
    /* Reading RR drains RSR.RFn. */
    g_assert_cmphex(qtest_readl(qts, mu + MU_RSR) & 0x3, ==, 0);

    g_assert_cmphex(qtest_readl(qts, INFO_BUF), ==, 0);
    g_assert_cmphex(qtest_readl(qts, INFO_BUF + INFO_LC), ==,
                    INFO_LC_OEM_OPEN);
    return qtest_readl(qts, INFO_BUF + INFO_SOC);
}

static void test_get_info_default(void)
{
    QTestState *qts = ele_init(NULL);

    /* A1 when soc-rev is not given. */
    g_assert_cmphex(ele_get_info(qts, ELE_MU_TFA), ==, SOC_WORD(0xa1));
    qtest_quit(qts);
}

static void test_get_info_a0(void)
{
    QTestState *qts = ele_init("0xa0");

    g_assert_cmphex(ele_get_info(qts, ELE_MU_TFA), ==, SOC_WORD(0xa0));
    qtest_quit(qts);
}

static void test_get_info_b0(void)
{
    QTestState *qts = ele_init("0xb0");

    /* Every ELE responder reports the same revision. */
    for (int i = 0; i < ARRAY_SIZE(ele_mus); i++) {
        g_assert_cmphex(ele_get_info(qts, ele_mus[i]), ==, SOC_WORD(0xb0));
    }
    qtest_quit(qts);
}

static void test_trng_state(void)
{
    QTestState *qts = ele_init(NULL);

    qtest_writel(qts, ELE_MU_TFA + MU_TR(0),
                 ELE_HDR(ELE_VER, 1, ELE_TRNG_STATE_REQ, ELE_CMD_TAG));

    /* 3-word reply: header + status + state. */
    g_assert_cmphex(qtest_readl(qts, ELE_MU_TFA + MU_RSR) & 0x7, ==, 0x7);
    g_assert_cmphex(qtest_readl(qts, ELE_MU_TFA + MU_RR(0)), ==,
                    ELE_HDR(ELE_VER, 3, ELE_TRNG_STATE_REQ, ELE_RESP_TAG));
    g_assert_cmphex(qtest_readl(qts, ELE_MU_TFA + MU_RR(1)), ==,
                    ELE_SUCCESS_IND);
    /* trng_state[7:0] = 0x3 (ready), ele_rng_ctx_state[15:8] = 0x2 (ready) */
    g_assert_cmphex(qtest_readl(qts, ELE_MU_TFA + MU_RR(2)), ==, 0x0203);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx95/ele/get-info/default-a1", test_get_info_default);
    qtest_add_func("/imx95/ele/get-info/soc-rev-a0", test_get_info_a0);
    qtest_add_func("/imx95/ele/get-info/soc-rev-b0", test_get_info_b0);
    qtest_add_func("/imx95/ele/trng-state", test_trng_state);
    return g_test_run();
}
