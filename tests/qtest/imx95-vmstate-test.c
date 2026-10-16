/*
 * QTest for i.MX 95 device migration state (hw/misc/imx95_dpu.c,
 * hw/misc/imx95_neutron.c).
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Saves a machine to a file and restores it into a second QEMU, then reads the
 * devices back through MMIO. A device with no VMStateDescription restores
 * zeroed, so each check below fails loudly on a missing or incomplete vmsd
 * instead of silently losing display and NPU state.
 *
 * The DPU case is deliberately not just a register compare: it leaves the 2D
 * blit engine's command decoder mid-burst, migrates, and then feeds the
 * remaining words to the restored machine. The burst has to complete at the
 * right offsets on the other side, which only holds if the decoder's own state
 * travelled with it.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define DPU_BASE          0x4b400000ULL

#define BLIT_CMDSEQ_HIF   0x10000
#define BLIT_HIF_OP_WRITE 0x14

#define FD9_BASEADDRESS0  0x90028      /* first word of the test burst */

#define DISP_IRQ0_BASE    0x381000
#define INT_ENABLE0       (DISP_IRQ0_BASE + 0x08)
#define INT_PRESET0       (DISP_IRQ0_BASE + 0x14)
#define INT_STATUS0       (DISP_IRQ0_BASE + 0x2c)

#define DISP_IRQ2_BASE    0x3a1000
#define INT2_ENABLE(n)    (DISP_IRQ2_BASE + 0x08 + 4 * (n))
#define INT2_PRESET(n)    (DISP_IRQ2_BASE + 0x14 + 4 * (n))
#define INT2_STATUS(n)    (DISP_IRQ2_BASE + 0x2c + 4 * (n))

/* A scanout register that is plain storage in the 4 MiB register file. */
#define FG0_HTCFG1        (0x2b0000 + 0x0c)

#define RCTL              0x4ab00000ULL  /* neutron RESETCTRL window */
#define DEV               0x4ab00004ULL  /* neutron device/mailbox window */
#define N_INTENA          0x004
#define N_BASEDDRL        0x204
#define N_BASEDDRH        0x208
#define RCTL_ZENV_CLK_ON  0x1

/* Values chosen so a zeroed (unmigrated) device cannot pass by accident. */
#define PAT_ENABLE0       0x00001000u
#define PAT_STATUS0       0x00000040u
#define PAT_ENABLE2_0     0x00002000u
#define PAT_ENABLE2_1     0x00004000u
#define PAT_STATUS2_0     0x00000080u
#define PAT_STATUS2_1     0x00000100u
#define PAT_HTCFG1        0x05a05a5au
#define PAT_BURST_W0      0x11223344u
#define PAT_BURST_W1      0x55667788u
#define PAT_BURST_W2      0x99aabbccu
#define PAT_BASEDDRL      0xcafe0000u
#define PAT_BASEDDRH      0x0000beefu
#define PAT_INTENA        0x00000001u

static void wait_for_migration_status(QTestState *who, const char *goal)
{
    /*
     * Poll rather than wait on an event: the destination finishes its load
     * without emitting MIGRATION on its own monitor in every QEMU version,
     * and query-migrate reports terminal state on both ends.
     */
    for (int i = 0; i < 2000; i++) {
        QDict *rsp = qtest_qmp(who, "{'execute': 'query-migrate'}");
        QDict *ret = qdict_get_qdict(rsp, "return");
        const char *status = ret ? qdict_get_try_str(ret, "status") : NULL;

        if (status && g_str_equal(status, goal)) {
            qobject_unref(rsp);
            return;
        }
        if (status && g_str_equal(status, "failed")) {
            g_test_message("migration failed");
            qobject_unref(rsp);
            g_assert_not_reached();
        }
        qobject_unref(rsp);
        g_usleep(1000);
    }
    g_test_message("migration did not reach '%s'", goal);
    g_assert_not_reached();
}

/*
 * Save src to path and shut it down, then bring up a second machine that loads
 * it. The two never run at once on purpose: libqtest derives both the qtest and
 * the QMP socket path from the test process's pid alone
 * (tests/qtest/libqtest.c:qtest_socket_path), so two live instances in one test
 * would share sockets and the reads below would land on the wrong machine. A
 * file: URI needs no live peer, so sequential is also sufficient.
 */
static void migrate_to_file(QTestState *src, const char *path)
{
    QDict *rsp;
    char *uri = g_strdup_printf("file:%s", path);

    rsp = qtest_qmp(src, "{'execute': 'migrate', 'arguments': {'uri': %s}}",
                    uri);
    g_assert(!qdict_haskey(rsp, "error"));
    qobject_unref(rsp);
    g_free(uri);

    wait_for_migration_status(src, "completed");
}

static QTestState *restore_from_file(const char *path)
{
    QTestState *dst = qtest_initf("-machine imx95-19x19-evk -accel qtest "
                                  "-incoming file:%s", path);

    wait_for_migration_status(dst, "completed");
    return dst;
}

/*
 * The DPU: interrupt blocks for both streams, a scanout register out of the
 * 4 MiB register file, and a blit command burst split across the migration.
 */
static void test_dpu_state_survives_migration(void)
{
    QTestState *src, *dst;
    char *path = g_strdup_printf("%s/imx95-vmstate-dpu.img", g_get_tmp_dir());

    src = qtest_initf("-machine imx95-19x19-evk -accel qtest");

    /* stream-0 and stream-1 display interrupt state */
    qtest_writel(src, DPU_BASE + INT_ENABLE0, PAT_ENABLE0);
    qtest_writel(src, DPU_BASE + INT_PRESET0, PAT_STATUS0);
    qtest_writel(src, DPU_BASE + INT2_ENABLE(0), PAT_ENABLE2_0);
    qtest_writel(src, DPU_BASE + INT2_ENABLE(1), PAT_ENABLE2_1);
    qtest_writel(src, DPU_BASE + INT2_PRESET(0), PAT_STATUS2_0);
    qtest_writel(src, DPU_BASE + INT2_PRESET(1), PAT_STATUS2_1);

    /* a plain register-file write */
    qtest_writel(src, DPU_BASE + FG0_HTCFG1, PAT_HTCFG1);

    /*
     * Open a 3-word HIF write burst at FD9_BASEADDRESS0 and send only the
     * first value. The decoder is now mid-burst: 2 words still owed, the next
     * one belongs at FD9_BASEADDRESS0 + 4.
     */
    qtest_writel(src, DPU_BASE + BLIT_CMDSEQ_HIF, (BLIT_HIF_OP_WRITE << 24) | 3);
    qtest_writel(src, DPU_BASE + BLIT_CMDSEQ_HIF, FD9_BASEADDRESS0);
    qtest_writel(src, DPU_BASE + BLIT_CMDSEQ_HIF, PAT_BURST_W0);

    /*
     * Control: every value has to read back on the SOURCE first. Without this,
     * a register the model never stored would look exactly like a register
     * migration lost, and the failure would point at the wrong code.
     */
    g_assert_cmphex(qtest_readl(src, DPU_BASE + INT_ENABLE0), ==, PAT_ENABLE0);
    g_assert_cmphex(qtest_readl(src, DPU_BASE + INT_STATUS0), ==, PAT_STATUS0);
    g_assert_cmphex(qtest_readl(src, DPU_BASE + INT2_ENABLE(0)), ==,
                    PAT_ENABLE2_0);
    g_assert_cmphex(qtest_readl(src, DPU_BASE + INT2_ENABLE(1)), ==,
                    PAT_ENABLE2_1);
    g_assert_cmphex(qtest_readl(src, DPU_BASE + INT2_STATUS(0)), ==,
                    PAT_STATUS2_0);
    g_assert_cmphex(qtest_readl(src, DPU_BASE + INT2_STATUS(1)), ==,
                    PAT_STATUS2_1);
    g_assert_cmphex(qtest_readl(src, DPU_BASE + FG0_HTCFG1), ==, PAT_HTCFG1);
    g_assert_cmphex(qtest_readl(src, DPU_BASE + FD9_BASEADDRESS0), ==,
                    PAT_BURST_W0);

    migrate_to_file(src, path);
    qtest_quit(src);
    dst = restore_from_file(path);

    /* the interrupt blocks and the register file came across */
    g_assert_cmphex(qtest_readl(dst, DPU_BASE + INT_ENABLE0), ==, PAT_ENABLE0);
    g_assert_cmphex(qtest_readl(dst, DPU_BASE + INT_STATUS0), ==, PAT_STATUS0);
    g_assert_cmphex(qtest_readl(dst, DPU_BASE + INT2_ENABLE(0)), ==,
                    PAT_ENABLE2_0);
    g_assert_cmphex(qtest_readl(dst, DPU_BASE + INT2_ENABLE(1)), ==,
                    PAT_ENABLE2_1);
    g_assert_cmphex(qtest_readl(dst, DPU_BASE + INT2_STATUS(0)), ==,
                    PAT_STATUS2_0);
    g_assert_cmphex(qtest_readl(dst, DPU_BASE + INT2_STATUS(1)), ==,
                    PAT_STATUS2_1);
    g_assert_cmphex(qtest_readl(dst, DPU_BASE + FG0_HTCFG1), ==, PAT_HTCFG1);
    g_assert_cmphex(qtest_readl(dst, DPU_BASE + FD9_BASEADDRESS0), ==,
                    PAT_BURST_W0);

    /*
     * Finish the burst on the restored machine. If hif_count/hif_off/
     * hif_have_off had not migrated, these two words would be read as a fresh
     * opcode and an offset and would land nowhere.
     */
    qtest_writel(dst, DPU_BASE + BLIT_CMDSEQ_HIF, PAT_BURST_W1);
    qtest_writel(dst, DPU_BASE + BLIT_CMDSEQ_HIF, PAT_BURST_W2);
    g_assert_cmphex(qtest_readl(dst, DPU_BASE + FD9_BASEADDRESS0 + 4), ==,
                    PAT_BURST_W1);
    g_assert_cmphex(qtest_readl(dst, DPU_BASE + FD9_BASEADDRESS0 + 8), ==,
                    PAT_BURST_W2);

    qtest_quit(dst);
    unlink(path);
    g_free(path);
}

/* The Neutron NPU, settled (IDLE): clock-on, mailbox and carveout registers. */
static void test_neutron_state_survives_migration(void)
{
    QTestState *src, *dst;
    char *path = g_strdup_printf("%s/imx95-vmstate-neutron.img",
                                 g_get_tmp_dir());

    src = qtest_initf("-machine imx95-19x19-evk -accel qtest");

    qtest_writel(src, RCTL, RCTL_ZENV_CLK_ON);
    qtest_writel(src, DEV + N_BASEDDRL, PAT_BASEDDRL);
    qtest_writel(src, DEV + N_BASEDDRH, PAT_BASEDDRH);
    qtest_writel(src, DEV + N_INTENA, PAT_INTENA);

    /* Control: the source has to hold the pattern before migrating it. */
    g_assert_cmphex(qtest_readl(src, RCTL), ==, RCTL_ZENV_CLK_ON);
    g_assert_cmphex(qtest_readl(src, DEV + N_BASEDDRL), ==, PAT_BASEDDRL);
    g_assert_cmphex(qtest_readl(src, DEV + N_BASEDDRH), ==, PAT_BASEDDRH);
    g_assert_cmphex(qtest_readl(src, DEV + N_INTENA), ==, PAT_INTENA);

    migrate_to_file(src, path);
    qtest_quit(src);
    dst = restore_from_file(path);

    g_assert_cmphex(qtest_readl(dst, RCTL), ==, RCTL_ZENV_CLK_ON);
    g_assert_cmphex(qtest_readl(dst, DEV + N_BASEDDRL), ==, PAT_BASEDDRL);
    g_assert_cmphex(qtest_readl(dst, DEV + N_BASEDDRH), ==, PAT_BASEDDRH);
    g_assert_cmphex(qtest_readl(dst, DEV + N_INTENA), ==, PAT_INTENA);

    qtest_quit(dst);
    unlink(path);
    g_free(path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx95/vmstate/dpu", test_dpu_state_survives_migration);
    qtest_add_func("/imx95/vmstate/neutron",
                   test_neutron_state_survives_migration);
    return g_test_run();
}
