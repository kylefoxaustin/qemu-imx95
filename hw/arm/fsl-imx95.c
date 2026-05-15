/*
 * NXP i.MX 95 SoC Implementation - v0.0.1 scaffold
 *
 * Modeled on hw/arm/fsl-imx8mp.c by Bernhard Beschow
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * v0.0.2 scope:
 *   - 6x Cortex-A55 cluster instantiated
 *   - GIC-600 (GICv3) wired to all cores including timer PPIs
 *   - DDR mapped at 0x8000_0000
 *   - All non-CPU/GIC peripherals are create_unimplemented_device() stubs
 *     so accesses log instead of faulting
 *
 * Memory-map addresses and IRQ numbers below come from two sources:
 *   - Linux DTS for direct-MMIO peripherals (LPUART, GIC, SM mailbox,
 *     ELE mailbox, OCRAM, watchdogs):
 *       references/linux-imx/arch/arm64/boot/dts/freescale/imx95.dtsi
 *   - NXP U-Boot's imx-regs.h for the SCMI-routed peripherals the SPL
 *     pokes before SCMI is up (CCM, ANATOP, IOMUXC, SRC, TRDC, the
 *     BLK_CTRL aggregates):
 *       references/uboot-imx/arch/arm/include/asm/arch-imx9/imx-regs.h
 *
 * The SCMI-routed set is logging-stub only at this milestone; the
 * SCMI server stub that takes their place at runtime lands in
 * later v0.1 commits.
 */

#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/arm/bsa.h"
#include "hw/arm/fsl-imx95.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
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
 * and a debug name. Values are taken from the NXP BSP device tree at
 * references/linux-imx/arch/arm64/boot/dts/freescale/imx95.dtsi unless
 * otherwise noted.
 */
static const struct {
    hwaddr      addr;
    size_t      size;
    const char *name;
} fsl_imx95_memmap[FSL_IMX95_NUM_REGIONS] = {
    [FSL_IMX95_RAM]                  = {
        FSL_IMX95_RAM_START, FSL_IMX95_RAM_SIZE_MAX, "ram"
    },

    /* GIC-600. dtsi: interrupt-controller@48000000 (dist + redist + its). */
    [FSL_IMX95_GIC_DIST]             = { 0x48000000, 64 * KiB,   "gic_dist" },
    [FSL_IMX95_GIC_REDIST]           = { 0x48060000, 768 * KiB,  "gic_redist" },
    [FSL_IMX95_GIC_ITS]              = { 0x48040000, 128 * KiB,  "gic_its" },

    /* On-chip SRAM "sram1". dtsi: sram@204c0000. */
    [FSL_IMX95_OCRAM]                = { 0x204c0000, 96 * KiB,   "ocram" },

    /*
     * LPUART block. compatible: "fsl,imx95-lpuart". Each instance is
     * a 4 KiB MMIO window. LPUART1/2 sit in the AON aips1 bus,
     * LPUART3-6 in the Wakeup aips2 bus, LPUART7/8 in aips3.
     * LPUART1 is the 19x19 EVK console (stdout-path = &lpuart1).
     */
    [FSL_IMX95_LPUART1]              = { 0x44380000, 4 * KiB,    "lpuart1" },
    [FSL_IMX95_LPUART2]              = { 0x44390000, 4 * KiB,    "lpuart2" },
    [FSL_IMX95_LPUART3]              = { 0x42570000, 4 * KiB,    "lpuart3" },
    [FSL_IMX95_LPUART4]              = { 0x42580000, 4 * KiB,    "lpuart4" },
    [FSL_IMX95_LPUART5]              = { 0x42590000, 4 * KiB,    "lpuart5" },
    [FSL_IMX95_LPUART6]              = { 0x425a0000, 4 * KiB,    "lpuart6" },
    [FSL_IMX95_LPUART7]              = { 0x42690000, 4 * KiB,    "lpuart7" },
    [FSL_IMX95_LPUART8]              = { 0x426a0000, 4 * KiB,    "lpuart8" },

    /*
     * SCMI-routed peripherals. Bases from U-Boot imx-regs.h. Logging
     * stubs only; SPL doesn't reach them through direct MMIO once the
     * SCMI server stub is up, but stubs catch any unexpected probing.
     */
    [FSL_IMX95_CCM]                  = { 0x44450000, 64 * KiB,   "ccm" },
    [FSL_IMX95_ANATOP]               = { 0x44480000, 64 * KiB,   "anatop" },
    [FSL_IMX95_IOMUXC]               = { 0x443c0000, 64 * KiB,   "iomuxc" },
    [FSL_IMX95_SRC]                  = { 0x44460000, 64 * KiB,   "src" },
    [FSL_IMX95_TRDC_AON]             = { 0x44270000, 64 * KiB,   "trdc_aon" },

    /*
     * BLK_CTRL aggregates per power domain. NETCMIX is Linux-visible
     * (kernel touches it directly per imx95.dtsi:2874); the other three
     * are SCMI-routed from U-Boot imx-regs.h.
     */
    [FSL_IMX95_BLK_CTRL_S_AONMIX]    = { 0x444f0000, 64 * KiB,   "blk_ctrl_s_aonmix" },
    [FSL_IMX95_BLK_CTRL_NS_ANOMIX]   = { 0x44210000, 64 * KiB,   "blk_ctrl_ns_anomix" },
    [FSL_IMX95_BLK_CTRL_WAKEUPMIX]   = { 0x42420000, 64 * KiB,   "blk_ctrl_wakeupmix" },
    [FSL_IMX95_BLK_CTRL_NETCMIX]     = { 0x4c810000, 64 * KiB,   "blk_ctrl_netcmix" },

    /*
     * System Manager SCMI channel. dtsi: mu2 mailbox@445b0000 (4 KiB) with
     * a 1 KiB sram0 child at 0x445b1000 holding the two SCMI A2P/P2A
     * shared-memory buffers.
     */
    [FSL_IMX95_SM_MU]                = { 0x445b0000, 4 * KiB,    "sm_mu" },
    [FSL_IMX95_SM_SHMEM]             = { 0x445b1000, 1 * KiB,    "sm_shmem" },

    /* EdgeLock Secure Enclave mailbox (elemu0). dtsi: mailbox@47520000. */
    [FSL_IMX95_ELE_MU]               = { 0x47520000, 64 * KiB,   "ele_mu" },

    /* Wakeup-domain watchdog. dtsi: watchdog@42490000. */
    [FSL_IMX95_WDOG3]                = { 0x42490000, 64 * KiB,   "wdog3" },
};

/*
 * For every peripheral region we don't yet model, install a stub that
 * traces accesses but doesn't fault. This lets U-Boot / Linux probe
 * registers safely while we iterate.
 */
static void fsl_imx95_install_unimplemented(FslImx95State *s)
{
    static const int unimplemented_regions[] = {
        FSL_IMX95_CCM, FSL_IMX95_ANATOP, FSL_IMX95_IOMUXC,
        FSL_IMX95_SRC, FSL_IMX95_TRDC_AON,
        FSL_IMX95_BLK_CTRL_S_AONMIX, FSL_IMX95_BLK_CTRL_NS_ANOMIX,
        FSL_IMX95_BLK_CTRL_WAKEUPMIX, FSL_IMX95_BLK_CTRL_NETCMIX,
        FSL_IMX95_SM_MU, FSL_IMX95_SM_SHMEM,
        FSL_IMX95_ELE_MU,
        FSL_IMX95_WDOG3,
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

        /* Wire the per-CPU timer PPIs and IRQ/FIQ lines. */
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

    /* LPUARTs. LPUART1 is the 19x19 EVK console. */
    {
        static const struct {
            int region;
            int irq;
        } lpuart_table[FSL_IMX95_NUM_LPUARTS] = {
            { FSL_IMX95_LPUART1, FSL_IMX95_LPUART1_IRQ },
            { FSL_IMX95_LPUART2, FSL_IMX95_LPUART2_IRQ },
            { FSL_IMX95_LPUART3, FSL_IMX95_LPUART3_IRQ },
            { FSL_IMX95_LPUART4, FSL_IMX95_LPUART4_IRQ },
            { FSL_IMX95_LPUART5, FSL_IMX95_LPUART5_IRQ },
            { FSL_IMX95_LPUART6, FSL_IMX95_LPUART6_IRQ },
            { FSL_IMX95_LPUART7, FSL_IMX95_LPUART7_IRQ },
            { FSL_IMX95_LPUART8, FSL_IMX95_LPUART8_IRQ },
        };

        for (i = 0; i < FSL_IMX95_NUM_LPUARTS; i++) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(&s->lpuart[i]);

            qdev_prop_set_chr(DEVICE(&s->lpuart[i]), "chardev",
                              serial_hd(i));
            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0,
                            fsl_imx95_memmap[lpuart_table[i].region].addr);
            sysbus_connect_irq(sbd, 0,
                qdev_get_gpio_in(gicdev, lpuart_table[i].irq));
        }
    }

    /* All peripherals not yet modeled get logging stubs. */
    fsl_imx95_install_unimplemented(s);
}

static void fsl_imx95_init(Object *obj)
{
    FslImx95State *s = FSL_IMX95(obj);
    int i;

    object_initialize_child(obj, "gic", &s->gic, TYPE_ARM_GICV3);

    for (i = 0; i < FSL_IMX95_NUM_LPUARTS; i++) {
        g_autofree char *name = g_strdup_printf("lpuart%d", i + 1);
        object_initialize_child(obj, name, &s->lpuart[i], TYPE_IMX_LPUART);
    }
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
