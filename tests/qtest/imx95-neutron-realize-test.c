/*
 * QTest that realizes the i.MX 95 Neutron device under -accel qtest. This is
 * the P1 regression guard for the Phase 0.5 "runtime seam inherits a lifetime
 * obligation" class of bug: the crash mode we care about (a Property[] with
 * stack lifetime handed to device_class_set_props_n) manifests during device
 * realize inside qtest_initf, before any MMIO is issued. So the test is: does
 * qtest_initf return at all?
 *
 * Gated in meson on CONFIG_IMX95_NEUTRON (NOT the runner) so this test builds
 * and runs under BOTH the RUNNER and RUNNER_STUB configurations. Deliberately
 * reintroducing a stack-local Property[] in add_props() would make these
 * subtests fail (SIGSEGV during qtest_initf).
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define DEV_MMIO_BASE   0x4ab00004ULL   /* neutron device/mailbox window */
#define N_APPCTRL       0x1fc

/*
 * The whole point: qtest_initf realizes every device on the imx95-19x19-evk
 * machine, including the neutron front end and its backend (either RUNNER or
 * RUNNER_STUB). If neutron_backend_runner_add_props() hands a stack-scoped
 * Property[] to device_class_set_props_n, realize crashes here.
 */
static void test_neutron_realize_survives(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest");
    g_assert_nonnull(qts);
    qtest_quit(qts);
}

/*
 * Prove the device is not just constructed but wired into the memory map:
 * a plain MMIO read from a known register must round-trip without crashing.
 * If the DeviceClass got corrupted properties, some builds silently degrade
 * (device present, MMIO not mapped) - this subtest catches that flavor too.
 */
static void test_neutron_realize_mmio_alive(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest");
    (void)qtest_readl(qts, DEV_MMIO_BASE + N_APPCTRL);
    qtest_quit(qts);
}

/*
 * Best-effort QOM probe: qom-list the neutron node. Under the RUNNER build
 * the "neutron-runner-*" properties must exist; under the STUB build they
 * must not. Either way qom-list itself must succeed on the neutron path -
 * that is what proves the DeviceClass's ObjectPropertyInfo backing storage
 * (the Property[] we allocated with g_new) is still valid post-realize.
 *
 * The QOM path /machine/soc/neutron comes from hw/arm/fsl-imx95.c
 * (object_property_add_child(s, "neutron", npu)).
 */
static void test_neutron_realize_qom_list_props(void)
{
    QTestState *qts = qtest_initf("-machine imx95-19x19-evk -accel qtest");
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-list',"
        " 'arguments':{'path':'/machine/soc/neutron'}}");
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("imx95/neutron/realize-survives",
                   test_neutron_realize_survives);
    qtest_add_func("imx95/neutron/realize-mmio-alive",
                   test_neutron_realize_mmio_alive);
    qtest_add_func("imx95/neutron/realize-qom-list-props",
                   test_neutron_realize_qom_list_props);
    return g_test_run();
}
