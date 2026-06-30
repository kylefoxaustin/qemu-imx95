/*
 * QTest for the i.MX 95 Mali GPU identify-tier model (hw/misc/imx95_mali.c).
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The model exists so Linux's kbase DDK *identifies* the Mali-G310 (rather than
 * rejecting an arch-0 stub) and then fails the un-modelled CSF bring-up cleanly.
 * The one value that has to be exactly right is GPU_ID at offset 0: a wrong
 * arch_minor builds an incomplete kbase regmap and oopses the probe. Assert the
 * real Mali-G310 / TVAX id (arch 10.12.0, product 4).
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define MALI_BASE     0x4d900000ULL
#define MALI_GPU_ID   0x0

/* Mali-G310 = TVAX: arch_major=10, arch_minor=12, arch_rev=0, product_major=4. */
#define GPU_ID_G310   0xAC040000u

static void test_mali_gpu_id(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest");

    /* kbase reads GPU_ID first; it must identify a known Valhall product. */
    g_assert_cmphex(qtest_readl(qts, MALI_BASE + MALI_GPU_ID), ==, GPU_ID_G310);

    /* arch fields decode to 10.12.0 (the real TVAX revision). */
    uint32_t id = qtest_readl(qts, MALI_BASE + MALI_GPU_ID);
    g_assert_cmpuint((id >> 28) & 0xf, ==, 10);   /* arch_major */
    g_assert_cmpuint((id >> 24) & 0xf, ==, 12);   /* arch_minor */
    g_assert_cmpuint((id >> 16) & 0xf, ==, 4);    /* product_major (TVAX) */

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("imx95/mali/gpu-id", test_mali_gpu_id);
    return g_test_run();
}
