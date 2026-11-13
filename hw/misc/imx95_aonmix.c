/*
 * NXP i.MX 95 BLK_CTRL_S_AONMIX - minimal model for the M7 CPU-WAIT gate
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Covers BLK_CTRL_S_AONMIX at 0x444f0000. The block is mostly plain
 * configuration storage, modelled here as a RAM-backed register file (an
 * improvement over the previous logging stub, which read back as zero).
 *
 * The one load-bearing register is M7_CFG (offset 0x124). Its WAIT bit
 * (bit 4) is the Cortex-M7 hold/run gate the System Manager uses to manage
 * the M7's lifecycle: CPU_RunModeGet reads it (WAIT set => HOLD, clear =>
 * START) and CPU_WaitSet sets/clears it. At reset WAIT is set, so the SM
 * sees the M7 held and runs its full DEV_SM_CpuStart sequence - releasing
 * CPUWAIT (which we surface as the m7-run gpio) and, crucially, enabling
 * the CM7_SYSRESETREQ fault IRQ so the SM can later cold-reset the M7 LM
 * (v1.x Step 5). A WAIT 1->0 transition releases the M7; 0->1 holds it.
 *
 * CA55_CPUWAIT (0x160) is the same gate for the six A55 cores, one bit each.
 * All six read as held at reset, exactly as the SoC comes out of reset. The
 * SM clears bit 0 from LMM_CpuStart once it has booted the AP logical
 * machine, which is what orders the AP behind the SM's own SCMI bring-up.
 * Bits 1-5 are cleared later, per core, when something asks the SM to start
 * a secondary - on a secure-firmware boot that asker is TF-A BL31, serving
 * the OS's PSCI CPU_ON over its own SCMI channel.
 *
 * CA55_RVBARADDR<n>_{L,H} (0x164 + n * 8) are the per-core reset vectors the
 * SM programs just before it opens the gate. On silicon each pair's output
 * is wired to that core's RVBAR input, so the core fetches its first
 * instruction from there; imx95_aonmix_a55_rvbar() surfaces the value for
 * the SoC to apply. The SM writes them shifted right by two (vectorShift in
 * its own fsl_cpu.c), so the register pair holds a word address.
 *
 * INITVTOR (0x108) - the M7 boot vector the SM programs - is stored but
 * unused: our M7 boots from its ITCM reset vector (init-svtor = 0).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/misc/imx95_aonmix.h"
#include "migration/vmstate.h"
#include "trace.h"

#define IMX95_AONMIX_REG_SIZE   0x10000
#define IMX95_AONMIX_NUM_WORDS  (IMX95_AONMIX_REG_SIZE / 4)

#define AONMIX_M7_CFG           0x124       /* M7 configure register */
#define AONMIX_M7_CFG_WAIT      0x10        /* bit 4: M7 CPU-WAIT (hold) */

#define AONMIX_CA55_CPUWAIT     0x160       /* CPUWAIT settings for the CA55 */
#define AONMIX_CA55_WAIT_ALL    0x3f        /* all six cores held at reset */

/* CA55_RVBARADDR<n>_L at 0x164 + n * 8, _H in the word above it. */
#define AONMIX_CA55_RVBAR_L(n)  (0x164 + (n) * 8)
#define AONMIX_CA55_RVBAR_H(n)  (0x168 + (n) * 8)
#define AONMIX_CA55_RVBAR_SHIFT 2           /* the SM's vectorShift */

struct IMX95AonmixState {
    SysBusDevice    parent_obj;
    MemoryRegion    iomem;
    uint32_t        regs[IMX95_AONMIX_NUM_WORDS];

    /*
     * M7 run gate, driven by M7_CFG.WAIT (level: 1 = released/run,
     * 0 = held). The machine wires this to a handler that resets+resumes
     * the M7 on release and halts it on hold.
     */
    qemu_irq        m7_run;

    /*
     * A55 run gates, one per core, driven by that core's CA55_CPUWAIT bit
     * (level: 1 = released/run, 0 = held).
     */
    qemu_irq        a55_run[IMX95_AONMIX_A55_CORES];
};

uint64_t imx95_aonmix_a55_rvbar(IMX95AonmixState *s, unsigned core)
{
    uint64_t words;

    if (core >= IMX95_AONMIX_A55_CORES) {
        return 0;
    }

    words = s->regs[AONMIX_CA55_RVBAR_L(core) / 4] |
            ((uint64_t)s->regs[AONMIX_CA55_RVBAR_H(core) / 4] << 32);

    return words << AONMIX_CA55_RVBAR_SHIFT;
}

static uint64_t imx95_aonmix_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX95AonmixState *s = opaque;

    return s->regs[offset / 4];
}

static void imx95_aonmix_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    IMX95AonmixState *s = opaque;

    /*
     * M7_CFG.WAIT toggling is the SM holding (set) or releasing (clear) the
     * M7. Surface the released state on the m7-run line so the machine
     * cycles the core. Only fires on an actual WAIT transition.
     */
    if (offset == AONMIX_M7_CFG) {
        uint32_t old = s->regs[offset / 4];
        uint32_t new = (uint32_t)value;

        s->regs[offset / 4] = new;
        if ((old ^ new) & AONMIX_M7_CFG_WAIT) {
            qemu_set_irq(s->m7_run, (new & AONMIX_M7_CFG_WAIT) ? 0 : 1);
        }
        return;
    }

    /*
     * Same gate for the A55s, a bit per core: the SM clears a bit to release
     * that core. Report every bit that actually changed - the SM writes this
     * register more than once per start, and on a secure-firmware boot the
     * secondaries are released one at a time, long after core 0.
     */
    if (offset == AONMIX_CA55_CPUWAIT) {
        uint32_t old = s->regs[offset / 4];
        uint32_t new = (uint32_t)value;
        unsigned core;

        s->regs[offset / 4] = new;
        trace_imx95_aonmix_cpuwait(old & AONMIX_CA55_WAIT_ALL,
                                   new & AONMIX_CA55_WAIT_ALL);
        for (core = 0; core < IMX95_AONMIX_A55_CORES; core++) {
            uint32_t bit = 1u << core;

            if ((old ^ new) & bit) {
                qemu_set_irq(s->a55_run[core], (new & bit) ? 0 : 1);
            }
        }
        return;
    }

    if (offset >= AONMIX_CA55_RVBAR_L(0) &&
        offset < AONMIX_CA55_RVBAR_L(IMX95_AONMIX_A55_CORES)) {
        unsigned core = (offset - AONMIX_CA55_RVBAR_L(0)) / 8;

        s->regs[offset / 4] = value;
        trace_imx95_aonmix_rvbar(core, imx95_aonmix_a55_rvbar(s, core));
        return;
    }

    s->regs[offset / 4] = value;
}

static const MemoryRegionOps imx95_aonmix_ops = {
    .read = imx95_aonmix_read,
    .write = imx95_aonmix_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void imx95_aonmix_reset(DeviceState *dev)
{
    IMX95AonmixState *s = IMX95_AONMIX(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /* M7 held at reset: the SM sees HOLD and runs its full CpuStart. */
    s->regs[AONMIX_M7_CFG / 4] = AONMIX_M7_CFG_WAIT;
    /* All A55 cores held at reset, exactly as the SoC comes out of reset. */
    s->regs[AONMIX_CA55_CPUWAIT / 4] = AONMIX_CA55_WAIT_ALL;
}

static void imx95_aonmix_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    IMX95AonmixState *s = IMX95_AONMIX(obj);

    memory_region_init_io(&s->iomem, obj, &imx95_aonmix_ops, s,
                          TYPE_IMX95_AONMIX, IMX95_AONMIX_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    qdev_init_gpio_out_named(DEVICE(obj), &s->m7_run, "m7-run", 1);
    qdev_init_gpio_out_named(DEVICE(obj), s->a55_run, "a55-run",
                             IMX95_AONMIX_A55_CORES);
}

static const VMStateDescription vmstate_imx95_aonmix = {
    .name = TYPE_IMX95_AONMIX,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX95AonmixState, IMX95_AONMIX_NUM_WORDS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx95_aonmix_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_imx95_aonmix;
    device_class_set_legacy_reset(dc, imx95_aonmix_reset);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "NXP i.MX 95 BLK_CTRL_S_AONMIX (CPU-WAIT gates)";
}

static const TypeInfo imx95_aonmix_info = {
    .name           = TYPE_IMX95_AONMIX,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(IMX95AonmixState),
    .instance_init  = imx95_aonmix_init,
    .class_init     = imx95_aonmix_class_init,
};

static void imx95_aonmix_register_types(void)
{
    type_register_static(&imx95_aonmix_info);
}

type_init(imx95_aonmix_register_types)
