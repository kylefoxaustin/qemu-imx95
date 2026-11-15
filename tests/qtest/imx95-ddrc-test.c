/*
 * QTest for the i.MX 95 DDR controller geometry (hw/misc/imx95_ddrc.c).
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * These four registers are the whole reason the block exists: the System
 * Manager reads the DRAM geometry out of them (imx-sm, DDR_GetDRAMInfo()) and
 * answers SCMI MISC_DDR_INFO_GET from it, which is where U-Boot's DRAM size
 * comes from on this SoC. Get them wrong and nothing faults - U-Boot just
 * prints a size that is not this machine's.
 *
 * So the test checks the geometry tracks -m rather than being a constant, and
 * that the one bit the SM uses to decide DDR was initialised at all - the DRAM
 * PLL's CTRL.POWERUP - is set. Without that bit the SM powers the DDRMIX
 * domain down and never reads these registers in the first place.
 *
 * Note that the released SM still powers that domain off by another route, so
 * U-Boot's reported DRAM size is not yet correct; see hw/misc/imx95_ddrc.c.
 * What is asserted here is what the machine presents, which is the half of it
 * this tree owns.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define DDRC_BASE           0x4e080000ULL
#define DDRC_CS_BNDS0       0x000
#define DDRC_CS_CONFIG0     0x080
#define DDRC_CS_CONFIG1     0x084
#define DDRC_DDR_SDRAM_CFG  0x110

/* ANATOP's DRAM_PLL instance, whose CTRL.POWERUP gates all of the above. */
#define DRAM_PLL_CTRL       0x44481700ULL
#define PLL_CTRL_POWERUP    0x1u
#define PLL_CTRL_BYPASS     0x4u

#define CS_EN               0x80000000u
#define BNDS_UNIT           (16ULL * 1024 * 1024)

/*
 * The SM's arithmetic, so the expectation here is derived the same way the
 * consumer derives it rather than restated: start = SA * 16 MiB + 0x80000000,
 * end = EA * 16 MiB + 16 MiB - 1 + 0x80000000, and U-Boot's size is
 * end + 1 - start.
 */
static uint64_t sm_reported_size(uint32_t cs_bnds)
{
    uint64_t sa = (cs_bnds >> 16) & 0xffff;
    uint64_t ea = cs_bnds & 0xffff;

    return (ea + 1 - sa) * BNDS_UNIT;
}

static void check_geometry(const char *mem, uint64_t ram_size)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest "
                                  "-m %s", mem);
    uint32_t bnds = qtest_readl(qts, DDRC_BASE + DDRC_CS_BNDS0);
    uint32_t cfg = qtest_readl(qts, DDRC_BASE + DDRC_DDR_SDRAM_CFG);
    uint32_t ctrl = qtest_readl(qts, DRAM_PLL_CTRL);
    uint64_t size = sm_reported_size(bnds);

    /* One rank, starting at the base of DRAM. */
    g_assert_cmpuint((bnds >> 16) & 0xffff, ==, 0);
    g_assert_cmphex(qtest_readl(qts, DDRC_BASE + DDRC_CS_CONFIG0) & CS_EN,
                    ==, CS_EN);
    g_assert_cmphex(qtest_readl(qts, DDRC_BASE + DDRC_CS_CONFIG1) & CS_EN,
                    ==, 0);

    /*
     * The size the SM would report must fit inside the DRAM the machine has,
     * and must be the largest whole 16 MiB that does - a -m that is not a
     * multiple of the unit is under-reported, never over-.
     */
    g_assert_cmpuint(size, <=, ram_size);
    g_assert_cmpuint(size + BNDS_UNIT, >, ram_size);

    /* LPDDR5 (001b), and a data bus the SM reads back as 32 bits. */
    g_assert_cmpuint((cfg >> 24) & 0x7, ==, 1);
    g_assert_cmphex(cfg & 0x180000, !=, 0x100000);

    /* FRACTPLL_GetEnable(): powered up, and not reported as bypassed. */
    g_assert_cmphex(ctrl & PLL_CTRL_POWERUP, ==, PLL_CTRL_POWERUP);
    g_assert_cmphex(ctrl & PLL_CTRL_BYPASS, ==, 0);

    qtest_quit(qts);
}

static void test_ddrc_2g(void)
{
    check_geometry("2G", 2ULL * 1024 * 1024 * 1024);
}

static void test_ddrc_1g(void)
{
    /* A different -m must move the geometry, or it is a constant. */
    check_geometry("1G", 1ULL * 1024 * 1024 * 1024);
}

static void test_ddrc_unaligned(void)
{
    /* Not a whole number of 16 MiB units: rounds down, never up. */
    check_geometry("1000M", 1000ULL * 1024 * 1024);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("imx95/ddrc/geometry-2g", test_ddrc_2g);
    qtest_add_func("imx95/ddrc/geometry-1g", test_ddrc_1g);
    qtest_add_func("imx95/ddrc/geometry-unaligned", test_ddrc_unaligned);
    return g_test_run();
}
