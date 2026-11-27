/*
 * QTest for the i.MX 95 AONMIX A55 CPU-WAIT gates (hw/misc/imx95_aonmix.c)
 * and the secondary-core release they drive in hw/arm/fsl-imx95.c.
 *
 * Copyright (c) 2026, Schreibikus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The System Manager starts A55 cores 1-5 the way it does on silicon: it
 * writes the boot address into CA55_RVBARADDRn_L/H and clears
 * CA55_CPUWAIT.CPUn_WAIT. A core that is powered off must then come up at
 * that address, at EL3 in AArch64. Setting the bit again (a hold) is
 * ignored: CPUWAIT gates a core coming out of reset, it does not stop a
 * running one, so a later release leaves a running core where it is.
 *
 * The start runs in a main-loop BH. A vCPU's PC and PSTATE are read with
 * HMP "info registers" (through human-monitor-command with a cpu-index);
 * each QMP command is handled by the main loop, so polling that way gives
 * the BH the chance to run, bounded by POLL_TRIES.
 *
 * Under qtest the board skips arm_load_kernel(), which is what registers
 * the A55 reset hooks, so a system reset resets the AONMIX registers but
 * not the A55 vCPUs: a core started before the reset stays powered on.
 *
 * A release that is still pending when a system reset happens must be
 * dropped. That needs the reset to be requested while the release BH is
 * queued, which libqtest cannot arrange deterministically (every qtest
 * command waits for its reply, and the main loop runs the BH in between),
 * so it is not tested here.
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

#define AONMIX              0x444f0000ULL
#define CA55_CPUWAIT        (AONMIX + 0x160)
#define CA55_RVBAR_L(n)     (AONMIX + 0x164 + 8 * (n))  /* address >> 2 */
#define CA55_RVBAR_H(n)     (AONMIX + 0x168 + 8 * (n))  /* address >> 34 */
#define CA55_WAIT_ALL       0x3f
#define CA55_NUM_CORES      6

#define POLL_TRIES          200
#define POLL_DELAY_US       5000

#define PSTATE_NRW          (1u << 4)   /* 0 = AArch64 */
#define PSTATE_EL(p)        (((p) >> 2) & 3)

static QTestState *aonmix_init(bool el3_firmware)
{
    return qtest_initf("-machine imx95-19x19-evk,el3-firmware=%s -m 1G "
                       "-accel qtest", el3_firmware ? "on" : "off");
}

/* cpu-index of A55 core @n; the M33 and M7 are vCPUs too. */
static int a55_cpu_index(QTestState *qts, int n)
{
    g_autofree char *path = g_strdup_printf("/machine/soc/cpu%d", n);
    QDict *resp = qtest_qmp(qts, "{ 'execute': 'query-cpus-fast' }");
    QListEntry *e;
    int index = -1;

    g_assert(qdict_haskey(resp, "return"));
    QLIST_FOREACH_ENTRY(qdict_get_qlist(resp, "return"), e) {
        QDict *cpu = qobject_to(QDict, qlist_entry_obj(e));

        if (!strcmp(qdict_get_str(cpu, "qom-path"), path)) {
            index = qdict_get_int(cpu, "cpu-index");
        }
    }
    qobject_unref(resp);
    g_assert_cmpint(index, >=, 0);
    return index;
}

/* Value of "@name=<hex>" in the "info registers" dump of vCPU @index. */
static uint64_t cpu_reg(QTestState *qts, int index, const char *name)
{
    g_autofree char *key = g_strdup_printf("%s=", name);
    QDict *resp = qtest_qmp(qts,
        "{ 'execute': 'human-monitor-command', 'arguments': "
        "{ 'command-line': 'info registers', 'cpu-index': %d } }", index);
    const char *out, *p;
    uint64_t val;

    g_assert(qdict_haskey(resp, "return"));
    out = qdict_get_str(resp, "return");
    p = strstr(out, key);
    g_assert_nonnull(p);
    val = g_ascii_strtoull(p + strlen(key), NULL, 16);
    qobject_unref(resp);
    return val;
}

/* Wait for A55 core @n to reach @pc, then check it is AArch64 EL3. */
static void wait_core_at(QTestState *qts, int n, uint64_t pc)
{
    int index = a55_cpu_index(qts, n);
    uint64_t now = cpu_reg(qts, index, " PC");
    uint32_t pstate;

    for (int i = 0; i < POLL_TRIES && now != pc; i++) {
        g_usleep(POLL_DELAY_US);
        now = cpu_reg(qts, index, " PC");
    }
    g_assert_cmphex(now, ==, pc);

    pstate = cpu_reg(qts, index, "PSTATE");
    g_assert_cmpuint(pstate & PSTATE_NRW, ==, 0);
    g_assert_cmpuint(PSTATE_EL(pstate), ==, 3);
}

/* A55 core @n must stay at @pc for a while (the BH has had its chance). */
static void check_core_stays_at(QTestState *qts, int n, uint64_t pc)
{
    int index = a55_cpu_index(qts, n);

    for (int i = 0; i < 20; i++) {
        g_assert_cmphex(cpu_reg(qts, index, " PC"), ==, pc);
        g_usleep(POLL_DELAY_US);
    }
}

static void set_rvbar(QTestState *qts, int n, uint64_t addr)
{
    qtest_writel(qts, CA55_RVBAR_L(n), (uint32_t)(addr >> 2));
    qtest_writel(qts, CA55_RVBAR_H(n), (uint32_t)(addr >> 34));
}

static void test_rvbar_regs(void)
{
    QTestState *qts = aonmix_init(true);

    /* All six cores come out of reset held. */
    g_assert_cmphex(qtest_readl(qts, CA55_CPUWAIT), ==, CA55_WAIT_ALL);

    for (int n = 0; n < CA55_NUM_CORES; n++) {
        /* L holds address [33:2], all 32 bits. */
        qtest_writel(qts, CA55_RVBAR_L(n), 0xffffffff);
        g_assert_cmphex(qtest_readl(qts, CA55_RVBAR_L(n)), ==, 0xffffffff);
        /* H holds address [39:34] only; bits [31:6] read as zero. */
        qtest_writel(qts, CA55_RVBAR_H(n), 0xffffffc0);
        g_assert_cmphex(qtest_readl(qts, CA55_RVBAR_H(n)), ==, 0);
        qtest_writel(qts, CA55_RVBAR_H(n), 0xffffffff);
        g_assert_cmphex(qtest_readl(qts, CA55_RVBAR_H(n)), ==, 0x3f);
    }

    qtest_quit(qts);
}

static void test_release(void)
{
    QTestState *qts = aonmix_init(true);

    check_core_stays_at(qts, 1, 0);

    /* Release core 1 at 0x88000000. */
    set_rvbar(qts, 1, 0x88000000);
    qtest_writel(qts, CA55_CPUWAIT, CA55_WAIT_ALL & ~BIT(1));
    wait_core_at(qts, 1, 0x88000000);
    /* Only core 1 was released. */
    check_core_stays_at(qts, 2, 0);

    /* RVBAR_H supplies address [39:34]: core 5 at 0x4_9000_0000. */
    set_rvbar(qts, 5, 0x490000000ULL);
    qtest_writel(qts, CA55_CPUWAIT, CA55_WAIT_ALL & ~(BIT(1) | BIT(5)));
    wait_core_at(qts, 5, 0x490000000ULL);
    /* Core 1 saw no edge and stays where it was. */
    check_core_stays_at(qts, 1, 0x88000000);

    qtest_quit(qts);
}

static void test_hold_ignored(void)
{
    QTestState *qts = aonmix_init(true);

    set_rvbar(qts, 1, 0x88000000);
    qtest_writel(qts, CA55_CPUWAIT, CA55_WAIT_ALL & ~BIT(1));
    wait_core_at(qts, 1, 0x88000000);

    /*
     * Hold, move RVBAR, release: the register takes every write, but a
     * running core is neither stopped by the hold nor restarted by the
     * release.
     */
    set_rvbar(qts, 1, 0x90000000);
    qtest_writel(qts, CA55_CPUWAIT, CA55_WAIT_ALL);
    g_assert_cmphex(qtest_readl(qts, CA55_CPUWAIT), ==, CA55_WAIT_ALL);
    check_core_stays_at(qts, 1, 0x88000000);
    qtest_writel(qts, CA55_CPUWAIT, CA55_WAIT_ALL & ~BIT(1));
    g_assert_cmphex(qtest_readl(qts, CA55_CPUWAIT), ==,
                    CA55_WAIT_ALL & ~BIT(1));
    check_core_stays_at(qts, 1, 0x88000000);

    qtest_quit(qts);
}

static void test_release_after_reset(void)
{
    QTestState *qts = aonmix_init(true);

    set_rvbar(qts, 1, 0x88000000);
    qtest_writel(qts, CA55_CPUWAIT, CA55_WAIT_ALL & ~BIT(1));
    wait_core_at(qts, 1, 0x88000000);

    /* A system reset holds every core again and clears the vectors. */
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, CA55_CPUWAIT), ==, CA55_WAIT_ALL);
    g_assert_cmphex(qtest_readl(qts, CA55_RVBAR_L(1)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, CA55_RVBAR_H(1)), ==, 0);

    /* A powered-off core can be released after the reset. */
    check_core_stays_at(qts, 2, 0);
    set_rvbar(qts, 2, 0x90000000);
    qtest_writel(qts, CA55_CPUWAIT, CA55_WAIT_ALL & ~BIT(2));
    wait_core_at(qts, 2, 0x90000000);

    qtest_quit(qts);
}

static void test_release_el3_firmware_off(void)
{
    QTestState *qts = aonmix_init(false);

    /*
     * The gate is SoC hardware and does not depend on who implements PSCI:
     * with QEMU's PSCI in use a release still starts a powered-off core.
     */
    set_rvbar(qts, 1, 0x88000000);
    qtest_writel(qts, CA55_CPUWAIT, CA55_WAIT_ALL & ~BIT(1));
    wait_core_at(qts, 1, 0x88000000);
    check_core_stays_at(qts, 2, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx95/aonmix/rvbar-regs", test_rvbar_regs);
    qtest_add_func("/imx95/aonmix/release", test_release);
    qtest_add_func("/imx95/aonmix/hold-ignored", test_hold_ignored);
    qtest_add_func("/imx95/aonmix/release-after-reset",
                   test_release_after_reset);
    qtest_add_func("/imx95/aonmix/release-el3-firmware-off",
                   test_release_el3_firmware_off);
    return g_test_run();
}
