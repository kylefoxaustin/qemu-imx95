/*
 * QTest for the i.MX 95 Neutron NPU bring-up model (hw/misc/imx95_neutron.c).
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Drives the remoteproc + mailbox handshake with no kernel: turn the NPU clock
 * on via RESETCTRL and check the firmware "started" signal (APPCTRL[31:16] =
 * 0xF807), then send a command through the mailbox doorbell and check the model
 * answers DONE with the completion status set. The proprietary NPU compute is
 * not modelled; this validates the bring-up handshake the driver depends on.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define RCTL          0x4ab00000ULL   /* remoteproc RESETCTRL window */
#define DEV           0x4ab00004ULL   /* device/mailbox window base */

#define N_INTENA      0x004
#define N_APPCTRL     0x1fc
#define N_APPSTATUS   0x200
#define N_MBOX0       0x23c           /* response retcode */
#define N_MBOX1       0x240           /* response error_code -> guest app */
#define N_MBOX3       0x248           /* command word */

#define RCTL_ZENV_CLK_ON   0x1
#define APPCTRL_DOORBELL   (1u << 2)
#define APPSTATUS_INFDONE  (1u << 0)
#define APPSTATUS_MBOX     (1u << 4)
#define CMD_RUN            0x269
#define RET_DONE           0xAD0

static void test_neutron_bringup(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest");
    uint32_t appctrl, appstatus;

    /* Turn the NPU clock on (rproc start) -> firmware "up" handshake. */
    qtest_writel(qts, RCTL, RCTL_ZENV_CLK_ON);
    appctrl = qtest_readl(qts, DEV + N_APPCTRL);
    g_assert_cmphex(appctrl >> 16, ==, 0xF807);

    /* Enable IRQ sources, send a RUN command, ring the doorbell. */
    qtest_writel(qts, DEV + N_INTENA, 0x5);
    qtest_writel(qts, DEV + N_MBOX3, CMD_RUN);
    qtest_writel(qts, DEV + N_APPCTRL, APPCTRL_DOORBELL);

    /* The model answers DONE and flags completion. */
    g_assert_cmphex(qtest_readl(qts, DEV + N_MBOX0), ==, RET_DONE);
    appstatus = qtest_readl(qts, DEV + N_APPSTATUS);
    g_assert_cmphex(appstatus & (APPSTATUS_INFDONE | APPSTATUS_MBOX), ==,
                    APPSTATUS_INFDONE | APPSTATUS_MBOX);

    /* The doorbell write preserves the firmware MBWR handshake in [31:16]. */
    g_assert_cmphex(qtest_readl(qts, DEV + N_APPCTRL) >> 16, ==, 0xF807);

    qtest_quit(qts);
}

/* Clock the NPU on and ring a RUN-inference doorbell. */
static void run_inference(QTestState *qts)
{
    qtest_writel(qts, RCTL, RCTL_ZENV_CLK_ON);
    qtest_writel(qts, DEV + N_MBOX3, CMD_RUN);
    qtest_writel(qts, DEV + N_APPCTRL, APPCTRL_DOORBELL);
}

/*
 * Default (no operator opt-in): the model is faithful to real silicon - an
 * uncomputed inference still completes DONE and reports error_code 0 (success)
 * in MBOX1. (The output is uncomputed; honesty is operator-side via QMP.)
 */
static void test_neutron_errcode_faithful(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest");

    run_inference(qts);
    g_assert_cmphex(qtest_readl(qts, DEV + N_MBOX0), ==, RET_DONE);
    g_assert_cmphex(qtest_readl(qts, DEV + N_MBOX1), ==, 0);

    qtest_quit(qts);
}

/*
 * Operator opt-in honest-fault: after the operator sets neutron-uncomputed-
 * errcode (via qom-set, as the farm control-plane would), the model returns
 * DONE in MBOX0 (so the driver never hangs) AND the chosen non-zero error_code
 * in MBOX1, which the driver surfaces to the guest app via
 * NEUTRON_IOCTL_INFERENCE_STATE - a guest-visible "did not compute" fault.
 */
static void test_neutron_errcode_honest(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest");

    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/soc/neutron',"
        "'property':'neutron-uncomputed-errcode','value':38368}}");  /* 0x95e0 */

    run_inference(qts);
    g_assert_cmphex(qtest_readl(qts, DEV + N_MBOX0), ==, RET_DONE);
    g_assert_cmphex(qtest_readl(qts, DEV + N_MBOX1), ==, 0x95e0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("imx95/neutron/bringup", test_neutron_bringup);
    qtest_add_func("imx95/neutron/errcode-faithful",
                   test_neutron_errcode_faithful);
    qtest_add_func("imx95/neutron/errcode-honest", test_neutron_errcode_honest);
    return g_test_run();
}
