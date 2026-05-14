/*
 * NXP i.MX 95 SoC Implementation - v0.0.1 scaffold
 *
 * Modeled on hw/arm/fsl-imx8mp.c by Bernhard Beschow
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * v0.0.1 scope:
 *   - 6x Cortex-A55 cluster instantiated
 *   - GIC-600 (GICv3) wired to all cores including timer PPIs
 *   - DDR mapped at 0x8000_0000
 *   - All non-CPU/GIC peripherals are create_unimplemented_device() stubs
 *     so accesses log instead of faulting
 *   - No LPUART model yet (next step in v0.0.2)
 *
 * Addresses marked TODO must be verified against the i.MX 95 RM before
 * production use. They are placeholders sufficient to get the machine to
 * compile and to instantiate without overlap.
 */

#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/arm/bsa.h"
#include "hw/arm/fsl-imx95.h"
#include "hw/core/boards.h"
#include "hw/intc/arm_gicv3.h"
#include "hw/misc/unimp.h"
#include "system/kvm.h"
#include "target/arm/cpu.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/kvm_arm.h"
#include "qapi/error.h"
#include "qobject/qlist.h"

/*
 * Single source of truth for the SoC memory map. Each entry maps a region
 * ID (from enum FslImx95MemoryRegions) to its physical base address, size,
 * and a debug name. v0.0.1 uses placeholders for peripheral addresses -
 * see the TODO comments. The DDR, OCRAM, and GIC addresses are the only
 * ones we count on being correct at this stage.
 */
static const struct {
    hwaddr      addr;
    size_t      size;
    const char *name;
} fsl_imx95_memmap[FSL_IMX95_NUM_REGIONS] = {
    [FSL_IMX95_RAM]                  = { FSL_IMX95_RAM_START, FSL_IMX95_RAM_SIZE_MAX, "ram" },

    /* GIC-600. TODO: confirm distributor/redistributor bases from RM. */
    [FSL_IMX95_GIC_DIST]             = { 0x48000000, 64 * KiB,  "gic_dist" },
    [FSL_IMX95_GIC_REDIST]           = { 0x48060000, 1 * MiB,   "gic_redist" },
    [FSL_IMX95_GIC_ITS]              = { 0x48040000, 128 * KiB, "gic_its" },

    /* On-chip RAM. TODO: confirm OCRAM base and size from RM. */
    [FSL_IMX95_OCRAM]                = { 0x20480000, 1 * MiB,   "ocram" },

    /*
     * LPUARTs in the Wakeup / AON domains.
     * TODO: pull exact bases from RM. Placeholder bases below are spaced
     * 0x10000 apart in a plausible Wakeup-domain window so accesses log
     * cleanly during early bring-up.
     */
    [FSL_IMX95_LPUART1]              = { 0x44380000, 64 * KiB,  "lpuart1" },
    [FSL_IMX95_LPUART2]              = { 0x44390000, 64 * KiB,  "lpuart2" },
    [FSL_IMX95_LPUART3]              = { 0x42570000, 64 * KiB,  "lpuart3" },

    /* Clock / reset / pinmux. TODO: verify all bases. */
    [FSL_IMX95_CCM]                  = { 0x44450000, 64 * KiB,  "ccm" },
    [FSL_IMX95_ANATOP]               = { 0x44480000, 64 * KiB,  "anatop" },
    [FSL_IMX95_IOMUXC]               = { 0x443c0000, 64 * KiB,  "iomuxc" },
    [FSL_IMX95_SRC]                  = { 0x44460000, 64 * KiB,  "src" },
    [FSL_IMX95_GPC]                  = { 0x44470000, 64 * KiB,  "gpc" },

    /* BLK_CTRL regions per power domain. TODO: verify. */
    [FSL_IMX95_BLK_CTRL_AONMIX]      = { 0x44410000, 64 * KiB,  "blk_ctrl_aonmix" },
    [FSL_IMX95_BLK_CTRL_WAKEUPMIX]   = { 0x42420000, 64 * KiB,  "blk_ctrl_wakeupmix" },
    [FSL_IMX95_BLK_CTRL_NETCMIX]     = { 0x4c810000, 64 * KiB,  "blk_ctrl_netcmix" },

    /* System Manager mailbox / shared memory (Cortex-M33 firmware target). */
    [FSL_IMX95_SM_MU]                = { 0x47540000, 64 * KiB,  "sm_mu" },
    [FSL_IMX95_SM_SHMEM]             = { 0x445b1000, 4 * KiB,   "sm_shmem" },

    /* EdgeLock Secure Enclave mailbox. */
    [FSL_IMX95_ELE_MU]               = { 0x47520000, 64 * KiB,  "ele_mu" },

    /* TRDC central. */
    [FSL_IMX95_TRDC]                 = { 0x44270000, 64 * KiB,  "trdc" },

    /* Watchdogs. */
    [FSL_IMX95_WDOG1]                = { 0x442d0000, 64 * KiB,  "wdog1" },
    [FSL_IMX95_WDOG2]                = { 0x442e0000, 64 * KiB,  "wdog2" },
};

/*
 * For every peripheral region we don't yet model, install a stub that
 * traces accesses but doesn't fault. This lets U-Boot / Linux probe
 * registers safely while we iterate.
 */
static void fsl_imx95_install_unimplemented(FslImx95State *s)
{
    static const int unimplemented_regions[] = {
        FSL_IMX95_LPUART1, FSL_IMX95_LPUART2, FSL_IMX95_LPUART3,
        FSL_IMX95_CCM, FSL_IMX95_ANATOP, FSL_IMX95_IOMUXC,
        FSL_IMX95_SRC, FSL_IMX95_GPC,
        FSL_IMX95_BLK_CTRL_AONMIX, FSL_IMX95_BLK_CTRL_WAKEUPMIX,
        FSL_IMX95_BLK_CTRL_NETCMIX,
        FSL_IMX95_SM_MU, FSL_IMX95_SM_SHMEM,
        FSL_IMX95_ELE_MU, FSL_IMX95_TRDC,
        FSL_IMX95_WDOG1, FSL_IMX95_WDOG2,
    };

    for (size_t i = 0; i < ARRAY_SIZE(unimplemented_regions); i++) {
        int r = unimplemented_regions[i];
        create_unimplemented_device(fsl_imx95_memmap[r].name,
                                    fsl_imx95_memmap[r].addr,
                                    fsl_imx95_memmap[r].size);
    }
}

static void fsl_imx95_realize(DeviceState *dev, Error **errp)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    FslImx95State *s = FSL_IMX95(dev);
    DeviceState *gicdev = DEVICE(&s->gic);
    const char *cpu_type = ms->cpu_type ?: ARM_CPU_TYPE_NAME("cortex-a55");
    int i;

    if (ms->smp.cpus > FSL_IMX95_NUM_A55_CPUS) {
        error_setg(errp, "%s: only %d A55 CPUs are supported (%d requested)",
                   TYPE_FSL_IMX95, FSL_IMX95_NUM_A55_CPUS, (int)ms->smp.cpus);
        return;
    }

    /* Instantiate the A55 cluster. */
    for (i = 0; i < ms->smp.cpus; i++) {
        g_autofree char *name = g_strdup_printf("cpu%d", i);
        object_initialize_child(OBJECT(dev), name, &s->cpu[i], cpu_type);
    }

    for (i = 0; i < ms->smp.cpus; i++) {
        if (ms->smp.cpus > 1 &&
            object_property_find(OBJECT(&s->cpu[i]), "reset-cbar")) {
            object_property_set_int(OBJECT(&s->cpu[i]), "reset-cbar",
                                    fsl_imx95_memmap[FSL_IMX95_GIC_DIST].addr,
                                    &error_abort);
        }

        /*
         * CNTFRQ for the ARM generic timer. i.MX 95 system counter
         * typically runs at 24 MHz; TODO: confirm from RM's
         * "System Counter" section.
         */
        object_property_set_int(OBJECT(&s->cpu[i]), "cntfrq", 24000000,
                                &error_abort);

        if (object_property_find(OBJECT(&s->cpu[i]), "has_el2")) {
            object_property_set_bool(OBJECT(&s->cpu[i]), "has_el2",
                                     !kvm_enabled(), &error_abort);
        }
        if (object_property_find(OBJECT(&s->cpu[i]), "has_el3")) {
            object_property_set_bool(OBJECT(&s->cpu[i]), "has_el3",
                                     !kvm_enabled(), &error_abort);
        }

        if (i) {
            /* Secondary CPUs come up via PSCI / SRC. */
            object_property_set_bool(OBJECT(&s->cpu[i]), "start-powered-off",
                                     true, &error_abort);
        }

        if (!qdev_realize(DEVICE(&s->cpu[i]), NULL, errp)) {
            return;
        }
    }

    /* GIC-600 (GICv3) */
    {
        SysBusDevice *gicsbd = SYS_BUS_DEVICE(&s->gic);
        QList *redist_region_count;

        qdev_prop_set_uint32(gicdev, "num-cpu", ms->smp.cpus);
        qdev_prop_set_uint32(gicdev, "num-irq",
                             FSL_IMX95_NUM_IRQS + GIC_INTERNAL);

        redist_region_count = qlist_new();
        qlist_append_int(redist_region_count, ms->smp.cpus);
        qdev_prop_set_array(gicdev, "redist-region-count", redist_region_count);

        object_property_set_link(OBJECT(&s->gic), "sysmem",
                                 OBJECT(get_system_memory()), &error_fatal);
        if (!sysbus_realize(gicsbd, errp)) {
            return;
        }
        sysbus_mmio_map(gicsbd, 0, fsl_imx95_memmap[FSL_IMX95_GIC_DIST].addr);
        sysbus_mmio_map(gicsbd, 1, fsl_imx95_memmap[FSL_IMX95_GIC_REDIST].addr);

        /* Wire the per-CPU timer PPIs and IRQ/FIQ lines (same pattern as 8MP). */
        for (i = 0; i < ms->smp.cpus; i++) {
            DeviceState *cpudev = DEVICE(&s->cpu[i]);
            int intidbase = FSL_IMX95_NUM_IRQS + i * GIC_INTERNAL;
            qemu_irq irq;

            static const int timer_irqs[] = {
                [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
                [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
                [GTIMER_HYP]  = ARCH_TIMER_NS_EL2_IRQ,
                [GTIMER_SEC]  = ARCH_TIMER_S_EL1_IRQ,
            };
            for (size_t j = 0; j < ARRAY_SIZE(timer_irqs); j++) {
                irq = qdev_get_gpio_in(gicdev, intidbase + timer_irqs[j]);
                qdev_connect_gpio_out(cpudev, j, irq);
            }

            sysbus_connect_irq(gicsbd, i,
                qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
            sysbus_connect_irq(gicsbd, i + ms->smp.cpus,
                qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
            sysbus_connect_irq(gicsbd, i + 2 * ms->smp.cpus,
                qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
            sysbus_connect_irq(gicsbd, i + 3 * ms->smp.cpus,
                qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
        }
    }

    /* On-chip RAM. */
    memory_region_init_ram(&s->ocram, NULL, "imx95-ocram",
                           fsl_imx95_memmap[FSL_IMX95_OCRAM].size,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                                fsl_imx95_memmap[FSL_IMX95_OCRAM].addr,
                                &s->ocram);

    /* All peripherals not yet modeled get logging stubs. */
    fsl_imx95_install_unimplemented(s);
}

static void fsl_imx95_init(Object *obj)
{
    FslImx95State *s = FSL_IMX95(obj);

    object_initialize_child(obj, "gic", &s->gic, TYPE_ARM_GICV3);
}

static void fsl_imx95_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = fsl_imx95_realize;
    /* This is an SoC, not user-creatable. */
    dc->user_creatable = false;
}

static const TypeInfo fsl_imx95_types[] = {
    {
        .name           = TYPE_FSL_IMX95,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(FslImx95State),
        .instance_init  = fsl_imx95_init,
        .class_init     = fsl_imx95_class_init,
    },
};

DEFINE_TYPES(fsl_imx95_types)
