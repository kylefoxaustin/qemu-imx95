/*
 * NXP i.MX 95 19x19 Evaluation Kit (LPDDR5) - QEMU machine
 *
 * Modeled on hw/arm/imx8mp-evk.c by Bernhard Beschow
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * v0.0.1 scope: instantiate the SoC, attach DDR, hand control to
 * arm_load_kernel() so -kernel works. No DTB modification, no SD card,
 * no console yet (LPUART model arrives in v0.0.2).
 */

#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "system/device_tree.h"
#include "hw/arm/boot.h"
#include "hw/arm/fsl-imx95.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "system/kvm.h"
#include "system/qtest.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "net/can_emu.h"

#define TYPE_IMX95_EVK_MACHINE MACHINE_TYPE_NAME("imx95-19x19-evk")
OBJECT_DECLARE_SIMPLE_TYPE(Imx95EvkMachineState, IMX95_EVK_MACHINE)

struct Imx95EvkMachineState {
    MachineState parent_obj;
    /* Optional CAN buses, attached via -machine canbus0=...,canbus1=... */
    CanBusState *canbus[FSL_IMX95_NUM_FLEXCAN];
};

/*
 * Inject device-tree nodes for the virtio-mmio transports the SoC instantiates
 * (see fsl-imx95.c) so the guest enumerates them on a normal boot of the
 * supplied dtb (the kernel's CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES is off). The
 * node has no interrupt-parent, so it inherits the root's GIC (#interrupt-cells
 * = 3): <GIC_SPI irq IRQ_TYPE_LEVEL_HIGH>.
 */
static void imx95_evk_modify_dtb(const struct arm_boot_info *info, void *fdt)
{
    for (int i = FSL_IMX95_NUM_VIRTIO_MMIO - 1; i >= 0; i--) {
        hwaddr base = FSL_IMX95_VIRTIO_MMIO_BASE +
                      (hwaddr)i * FSL_IMX95_VIRTIO_MMIO_SIZE;
        int irq = FSL_IMX95_VIRTIO_MMIO_IRQ + i;
        g_autofree char *node = g_strdup_printf("/virtio_mmio@%" PRIx64, base);

        qemu_fdt_add_subnode(fdt, node);
        qemu_fdt_setprop_string(fdt, node, "compatible", "virtio,mmio");
        qemu_fdt_setprop_cells(fdt, node, "reg",
                               0, base, 0, FSL_IMX95_VIRTIO_MMIO_SIZE);
        qemu_fdt_setprop_cells(fdt, node, "interrupts", 0, irq, 4);
        qemu_fdt_setprop(fdt, node, "dma-coherent", NULL, 0);
    }
}

static void imx95_evk_init(MachineState *machine)
{
    Imx95EvkMachineState *m = IMX95_EVK_MACHINE(machine);
    static struct arm_boot_info boot_info;
    FslImx95State *s;
    int i;

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
        /*
         * The default arm_load_kernel() heuristic lands the initrd (and the
         * DTB right after it) at loader_start + 128 MiB == 0x88000000. That
         * is exactly where the NXP BSP device tree places the Cortex-M7
         * remoteproc carveout (vdev vrings + rsc-table @0x88220000). The
         * collision makes Linux reserve those pages for the initrd/FDT, so
         * the later reserved-memory no-map pass fails (-EBUSY) and imx-rproc
         * cannot ioremap the rsc-table (probe fails with -ENOMEM). Force the
         * initrd/DTB up to 0x90000000, in the free gap above the M7 carveout
         * and below the GPU/VPU carveouts at 0xa0000000.
         */
        .initrd_start = FSL_IMX95_RAM_START + 256 * MiB,
        /* Inject the virtio-mmio transport nodes into the supplied dtb. */
        .modify_dtb   = imx95_evk_modify_dtb,
    };

    s = FSL_IMX95(object_new(TYPE_FSL_IMX95));
    object_property_add_child(OBJECT(machine), "soc", OBJECT(s));

    /* Forward any user-attached CAN buses to the SoC's FlexCAN controllers. */
    for (i = 0; i < FSL_IMX95_NUM_FLEXCAN; i++) {
        if (m->canbus[i]) {
            g_autofree char *name = g_strdup_printf("canbus%d", i);
            object_property_set_link(OBJECT(s), name, OBJECT(m->canbus[i]),
                                     &error_abort);
        }
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(s), &error_fatal);

    memory_region_add_subregion(get_system_memory(), FSL_IMX95_RAM_START,
                                machine->ram);

    if (!qtest_enabled()) {
        arm_load_kernel(&s->cpu[0], machine, &boot_info);

        /*
         * arm_load_kernel() registers its boot reset hook on every CPU in
         * the system and treats all non-boot cores as A-profile PSCI
         * secondaries. That is correct for the five A55 secondaries, but
         * the Cortex-M33 SM core is not part of the A55 boot flow - it
         * boots from its own ITCM vector table. Detach it from the
         * A-profile boot machinery so its reset just runs the normal
         * M-profile vector-table reset. Whether it then actually runs is
         * decided by the SoC's M33 reset hook (only if SM firmware was
         * loaded into ITCM).
         */
        if (s->m33.cpu) {
            s->m33.cpu->env.boot_info = NULL;
        }
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
    /*
     * Total vCPUs = 6 A55 + 1 Cortex-M33 System Manager core + 1 Cortex-M7
     * real-time core that the SoC always instantiates. TCG sizes its
     * per-CPU context table from the resolved smp.max_cpus, which defaults
     * to smp.cpus when -smp is not given - so both the default and the
     * max must include the M33 and the M7, otherwise the 8th CPU's
     * tcg_register_thread() asserts. The A55 cluster size is fixed in
     * the SoC regardless of -smp; this count is really "A55 cluster +
     * SM core + RT core".
     */
    mc->default_cpus          = FSL_IMX95_NUM_A55_CPUS + 2;
    mc->max_cpus              = FSL_IMX95_NUM_A55_CPUS + 2;
    mc->default_ram_id        = "imx95-19x19-evk.ram";
    mc->default_ram_size      = 8 * GiB;   /* 19x19 EVK has 8 GiB LPDDR5 */
    mc->get_default_cpu_type  = imx95_evk_get_default_cpu_type;
}

static void imx95_evk_machine_instance_init(Object *obj)
{
    int i;

    /*
     * Per-FlexCAN CAN-bus links, settable from the command line, e.g.
     *   -object can-bus,id=canbus0 -machine canbus0=canbus0
     * The machine forwards each to the matching SoC FlexCAN controller.
     */
    for (i = 0; i < FSL_IMX95_NUM_FLEXCAN; i++) {
        g_autofree char *name = g_strdup_printf("canbus%d", i);
        object_property_add_link(obj, name, TYPE_CAN_BUS,
                                 (Object **)&IMX95_EVK_MACHINE(obj)->canbus[i],
                                 object_property_allow_set_link, 0);
    }
}

static void imx95_19x19_evk_class_init(ObjectClass *oc, const void *data)
{
    imx95_19x19_evk_machine_init(MACHINE_CLASS(oc));
}

static const TypeInfo imx95_19x19_evk_machine_types[] = {
    {
        .name          = TYPE_IMX95_EVK_MACHINE,
        .parent        = TYPE_MACHINE,
        .instance_size = sizeof(Imx95EvkMachineState),
        .instance_init = imx95_evk_machine_instance_init,
        .class_init    = imx95_19x19_evk_class_init,
        .interfaces    = aarch64_machine_interfaces,
    },
};

DEFINE_TYPES(imx95_19x19_evk_machine_types)
