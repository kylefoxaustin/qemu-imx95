/*
 * NXP i.MX 95 19x19 Evaluation Kit (LPDDR5) - QEMU machine
 *
 * Modeled on hw/arm/imx8mp-evk.c by Bernhard Beschow
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * v0.0.1 scope: instantiate the SoC, attach DDR, hand control to
 * arm_load_kernel() so -kernel works. No DTB modification, no SD card,
 * no console yet (LPUART model arrives in v0.0.2).
 */

#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "hw/arm/boot.h"
#include "hw/arm/fsl-imx95.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "system/kvm.h"
#include "system/qtest.h"
#include "qemu/error-report.h"
#include "qapi/error.h"

static void imx95_evk_init(MachineState *machine)
{
    static struct arm_boot_info boot_info;
    FslImx95State *s;

    if (machine->ram_size > FSL_IMX95_RAM_SIZE_MAX) {
        error_report("RAM size " RAM_ADDR_FMT
                     " above max supported (0x%" PRIx64 ")",
                     machine->ram_size, (uint64_t)FSL_IMX95_RAM_SIZE_MAX);
        exit(1);
    }

    boot_info = (struct arm_boot_info) {
        .loader_start = FSL_IMX95_RAM_START,
        .board_id     = -1,
        .ram_size     = machine->ram_size,
        .psci_conduit = QEMU_PSCI_CONDUIT_SMC,
    };

    s = FSL_IMX95(object_new(TYPE_FSL_IMX95));
    object_property_add_child(OBJECT(machine), "soc", OBJECT(s));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s), &error_fatal);

    memory_region_add_subregion(get_system_memory(), FSL_IMX95_RAM_START,
                                machine->ram);

    if (!qtest_enabled()) {
        arm_load_kernel(&s->cpu[0], machine, &boot_info);
    }
}

static const char *imx95_evk_get_default_cpu_type(const MachineState *ms)
{
    if (kvm_enabled()) {
        return ARM_CPU_TYPE_NAME("host");
    }
    return ARM_CPU_TYPE_NAME("cortex-a55");
}

static void imx95_19x19_evk_machine_init(MachineClass *mc)
{
    mc->desc                  = "NXP i.MX 95 19x19 EVK (LPDDR5)";
    mc->init                  = imx95_evk_init;
    mc->default_cpus          = FSL_IMX95_NUM_A55_CPUS;
    mc->max_cpus              = FSL_IMX95_NUM_A55_CPUS;
    mc->default_ram_id        = "imx95-19x19-evk.ram";
    mc->default_ram_size      = 8 * GiB;   /* 19x19 EVK ships with 8 GiB LPDDR5 */
    mc->get_default_cpu_type  = imx95_evk_get_default_cpu_type;
}

DEFINE_MACHINE_AARCH64("imx95-19x19-evk", imx95_19x19_evk_machine_init)
