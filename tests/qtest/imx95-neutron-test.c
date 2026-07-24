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
#define N_BASEDDRL    0x204
#define N_BASEDDRH    0x208
#define N_MBOX0       0x23c           /* response retcode */
#define N_MBOX1       0x240           /* response error_code -> guest app */
#define N_MBOX3       0x248           /* command word */
#define N_MBOX4       0x24c           /* RUN arg0 tensor_offset */
#define N_MBOX5       0x250           /* RUN arg1 microcode_offset */
#define N_MBOX6       0x254           /* RUN arg2 tensor_count */

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

/*
 * Runner backend bit-exact case (M4). Opt-in via env vars:
 *
 *   NEUTRON_RUNNER_PATH  - absolute path to the operator-supplied neutron-runner
 *                          ELF (from the user's eIQ Neutron SDK).
 *   NEUTRON_RUNNER_DROP  - directory containing the mobilenet .tflite, inputs/
 *                          (0000.bin..0009.bin, 150528 B each) and
 *                          neutron-runner-outputs/ (..results_N_0.bin, 1001 B).
 *
 * With those set, stage each of the 10 inputs into a carveout in guest DRAM,
 * drive the RUN mailbox with keys the fixture manifest expects, wait for DONE,
 * DMA-read the output span, and memcmp against the golden file. If any of the
 * env vars are unset (or the files are missing), skip cleanly - upstream CI
 * stays green.
 *
 * The (microcode_offset, tensor_count) keys are chosen synthetically here; the
 * qtest and the on-the-fly-generated fixture agree on them, so the runner
 * backend has a valid layout to DMA against without depending on driver-
 * observed offsets (which are still an open question, see the M3.4 handoff).
 */
#define BR_CARVEOUT_BASE      0x80000000ULL     /* FSL_IMX95_RAM_START */
#define BR_INPUT_OFFSET       0x00100000ULL     /* 1 MiB in - clear of boot */
#define BR_INPUT_SIZE         150528u
#define BR_OUTPUT_OFFSET      0x00200000ULL     /* well past input */
#define BR_OUTPUT_SIZE        1001u
#define BR_MICROCODE_OFFSET   0x1u
#define BR_TENSOR_COUNT       1u
#define BR_NUM_VECTORS        10
/* Runner spawn + inference wall-clock; timer fires inside clock_step. */
#define BR_STEP_NS            (1ULL * 1000 * 1000 * 1000)  /* 1 s virtual */

static char *br_write_fixtures_json(const char *tmpdir, const char *tflite)
{
    char *path = g_build_filename(tmpdir, "fixtures.json", NULL);
    g_autofree char *body = g_strdup_printf(
        "{\n"
        "  \"fixtures\": [\n"
        "    {\n"
        "      \"name\": \"mobilenet_v1_qtest\",\n"
        "      \"tflite_path\": \"%s\",\n"
        "      \"microcode_offset\": %u,\n"
        "      \"tensor_count\": %u,\n"
        "      \"input_offset\": %u,\n"
        "      \"input_size\": %u,\n"
        "      \"output_offset\": %u,\n"
        "      \"output_size\": %u\n"
        "    }\n"
        "  ]\n"
        "}\n",
        tflite,
        BR_MICROCODE_OFFSET, BR_TENSOR_COUNT,
        (unsigned)BR_INPUT_OFFSET, BR_INPUT_SIZE,
        (unsigned)BR_OUTPUT_OFFSET, BR_OUTPUT_SIZE);
    GError *err = NULL;
    if (!g_file_set_contents(path, body, -1, &err)) {
        g_test_message("cannot write fixtures json %s: %s",
                       path, err ? err->message : "?");
        g_clear_error(&err);
        g_free(path);
        return NULL;
    }
    return path;
}

static void test_neutron_runner_bitexact(void)
{
    const char *runner = g_getenv("NEUTRON_RUNNER_PATH");
    const char *drop   = g_getenv("NEUTRON_RUNNER_DROP");

    if (!runner || !*runner || !drop || !*drop) {
        g_test_skip("NEUTRON_RUNNER_PATH / NEUTRON_RUNNER_DROP not set");
        return;
    }
    if (!g_file_test(runner, G_FILE_TEST_IS_EXECUTABLE)) {
        g_test_skip("neutron-runner not executable");
        return;
    }

    g_autofree char *tflite = g_build_filename(
        drop, "mobilenet_v1_1.0_224_int8_imx95_converted.tflite", NULL);
    if (!g_file_test(tflite, G_FILE_TEST_EXISTS)) {
        g_test_skip("tflite model missing from drop directory");
        return;
    }

    GError *err = NULL;
    g_autofree char *tmpdir = g_dir_make_tmp("imx95-neutron-XXXXXX", &err);
    if (!tmpdir) {
        g_test_skip("cannot create tmpdir");
        g_clear_error(&err);
        return;
    }
    g_autofree char *fixtures = br_write_fixtures_json(tmpdir, tflite);
    if (!fixtures) {
        g_test_skip("cannot stage fixtures manifest");
        return;
    }
    g_autofree char *scratch = g_build_filename(tmpdir, "scratch", NULL);
    g_mkdir_with_parents(scratch, 0700);

    QTestState *qts = qtest_initf(
        "-machine imx95-19x19-evk -accel qtest -m 512M "
        "-global driver=imx95.neutron,property=compute-backend,value=runner "
        "-global driver=imx95.neutron,property=neutron-runner-path,value=%s "
        "-global driver=imx95.neutron,property=neutron-runner-fixtures,value=%s "
        "-global driver=imx95.neutron,property=neutron-runner-scratch-dir,"
        "value=%s",
        runner, fixtures, scratch);

    /* Bring rproc up once; subsequent RUNs reuse the same qts. */
    qtest_writel(qts, RCTL, RCTL_ZENV_CLK_ON);

    /* Program BASEDDR + RUN args (constant across all 10 vectors). */
    qtest_writel(qts, DEV + N_BASEDDRL,
                 (uint32_t)(BR_CARVEOUT_BASE & 0xffffffffu));
    qtest_writel(qts, DEV + N_BASEDDRH,
                 (uint32_t)(BR_CARVEOUT_BASE >> 32));
    qtest_writel(qts, DEV + N_MBOX4, 0);                       /* tensor_off */
    qtest_writel(qts, DEV + N_MBOX5, BR_MICROCODE_OFFSET);
    qtest_writel(qts, DEV + N_MBOX6, BR_TENSOR_COUNT);

    for (int i = 0; i < BR_NUM_VECTORS; i++) {
        g_autofree char *in_name  = g_strdup_printf("%04d.bin", i);
        g_autofree char *in_path  = g_build_filename(drop, "inputs",
                                                     in_name, NULL);
        g_autofree char *out_name = g_strdup_printf(
            "mobilenet_v1_1.0_224_int8_imx95_converted_results_%d_0.bin", i);
        g_autofree char *out_path = g_build_filename(
            drop, "neutron-runner-outputs", out_name, NULL);

        g_autofree char *in_buf  = NULL;
        gsize in_len = 0;
        if (!g_file_get_contents(in_path, &in_buf, &in_len, NULL) ||
            in_len != BR_INPUT_SIZE) {
            g_test_message("skip vector %d: cannot read %s (len=%zu)",
                           i, in_path, in_len);
            continue;
        }
        g_autofree char *golden = NULL;
        gsize golden_len = 0;
        if (!g_file_get_contents(out_path, &golden, &golden_len, NULL) ||
            golden_len != BR_OUTPUT_SIZE) {
            g_test_message("skip vector %d: cannot read golden %s (len=%zu)",
                           i, out_path, golden_len);
            continue;
        }

        /* Stage input into the guest carveout. */
        qtest_bufwrite(qts, BR_CARVEOUT_BASE + BR_INPUT_OFFSET, in_buf, in_len);

        /* Ring RUN doorbell; RUN_ACK is synchronous. */
        qtest_writel(qts, DEV + N_MBOX3, CMD_RUN);
        qtest_writel(qts, DEV + N_APPCTRL, APPCTRL_DOORBELL);
        g_assert_cmphex(qtest_readl(qts, DEV + N_MBOX0), ==, RET_RUN_ACK);

        /*
         * The DONE deferral timer fires inside qtest_clock_step; the runner
         * subprocess spawn + wait happens synchronously in that timer callback
         * under the BQL, so clock_step returns only after the runner has
         * produced its output and the backend DMA'd it back into the carveout.
         */
        qtest_clock_step(qts, BR_STEP_NS);

        g_assert_cmphex(qtest_readl(qts, DEV + N_MBOX0), ==, RET_DONE);
        g_assert_cmphex(qtest_readl(qts, DEV + N_MBOX1), ==, 0);

        g_autofree uint8_t *got = g_malloc(BR_OUTPUT_SIZE);
        qtest_bufread(qts, BR_CARVEOUT_BASE + BR_OUTPUT_OFFSET,
                      got, BR_OUTPUT_SIZE);
        if (memcmp(got, golden, BR_OUTPUT_SIZE) != 0) {
            g_error("vector %d: runner output does not match golden", i);
        }
    }

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
    qtest_add_func("imx95/neutron/runner-bitexact",
                   test_neutron_runner_bitexact);
    return g_test_run();
}
