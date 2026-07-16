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
#define RET_RUN_ACK        0xA3
#define RET_DONE           0xAD0

/* Past the model's RUN_ACK -> DONE servicing delay (NEUTRON_RUN_NS = 1000). */
#define NEUTRON_STEP_NS    2000

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

    /*
     * Two-phase mailbox: the doorbell is acked SYNCHRONOUSLY with RUN_ACK (so
     * the driver's MBOX0 != 0 poll unblocks), and completion has NOT landed yet
     * - MBOX0 is RUN_ACK, not DONE, and the event flags are still clear.
     */
    g_assert_cmphex(qtest_readl(qts, DEV + N_MBOX0), ==, RET_RUN_ACK);
    appstatus = qtest_readl(qts, DEV + N_APPSTATUS);
    g_assert_cmphex(appstatus & (APPSTATUS_INFDONE | APPSTATUS_MBOX), ==, 0);

    /* Advance past the servicing delay: now DONE lands with the event flags. */
    qtest_clock_step(qts, NEUTRON_STEP_NS);
    g_assert_cmphex(qtest_readl(qts, DEV + N_MBOX0), ==, RET_DONE);
    appstatus = qtest_readl(qts, DEV + N_APPSTATUS);
    g_assert_cmphex(appstatus & (APPSTATUS_INFDONE | APPSTATUS_MBOX), ==,
                    APPSTATUS_INFDONE | APPSTATUS_MBOX);

    /* The doorbell write preserves the firmware MBWR handshake in [31:16]. */
    g_assert_cmphex(qtest_readl(qts, DEV + N_APPCTRL) >> 16, ==, 0xF807);

    qtest_quit(qts);
}

/*
 * Clock the NPU on, ring a RUN-inference doorbell, and advance past the
 * two-phase servicing delay so the DONE completion (and MBOX1 error_code) have
 * landed by the time the caller reads them.
 */
static void run_inference(QTestState *qts)
{
    qtest_writel(qts, RCTL, RCTL_ZENV_CLK_ON);
    qtest_writel(qts, DEV + N_MBOX3, CMD_RUN);
    qtest_writel(qts, DEV + N_APPCTRL, APPCTRL_DOORBELL);
    qtest_clock_step(qts, NEUTRON_STEP_NS);
}

/*
 * Default honest-fault: the NPU compute is unmodelled, so an uncomputed
 * inference completes DONE in MBOX0 (the driver never hangs) AND reports a
 * recognisable non-zero error_code (0x95e0) in MBOX1, which the driver surfaces
 * to the guest app via NEUTRON_IOCTL_INFERENCE_STATE - a guest-visible "did not
 * compute" fault by default, rather than a silent wrong answer.
 */
static void test_neutron_errcode_honest_default(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest");

    run_inference(qts);
    g_assert_cmphex(qtest_readl(qts, DEV + N_MBOX0), ==, RET_DONE);
    g_assert_cmphex(qtest_readl(qts, DEV + N_MBOX1), ==, 0x95e0);

    qtest_quit(qts);
}

/*
 * Happy-path opt-out: after the operator sets neutron-uncomputed-errcode to 0
 * (via qom-set, as the farm control-plane would for a guest that tolerates
 * uncomputed output), the model is silicon-faithful - DONE in MBOX0 and
 * error_code 0 (success) in MBOX1.
 */
static void test_neutron_errcode_faithful_optout(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest");

    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/soc/neutron',"
        "'property':'neutron-uncomputed-errcode','value':0}}");

    run_inference(qts);
    g_assert_cmphex(qtest_readl(qts, DEV + N_MBOX0), ==, RET_DONE);
    g_assert_cmphex(qtest_readl(qts, DEV + N_MBOX1), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("imx95/neutron/bringup", test_neutron_bringup);
    qtest_add_func("imx95/neutron/errcode-honest-default",
                   test_neutron_errcode_honest_default);
    qtest_add_func("imx95/neutron/errcode-faithful-optout",
                   test_neutron_errcode_faithful_optout);
    return g_test_run();
}
