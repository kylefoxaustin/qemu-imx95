/*
 * NXP i.MX 95 DDR controller (geometry reporting)
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The DDRC at 0x4e080000. This is not a memory-controller model - QEMU's DRAM
 * is plain host memory and needs no training, so there is no timing, no PHY
 * and no calibration here. What this block exists for is the one question the
 * System Manager asks it: how much DRAM is there, and where.
 *
 * The SM reads that out of four fields (imx-sm, DDR_GetDRAMInfo() in
 * drivers/ddr/fsl_ddr.c) and answers SCMI MISC_DDR_INFO_GET from them, which
 * is how U-Boot's board_phys_sdram_size() sizes DRAM on an i.MX 95:
 *
 *   CS_BNDS[0]    start and end address, in units of 16 MiB
 *   CS_CONFIG[n]  which ranks are enabled
 *   DDR_SDRAM_CFG SDRAM type, data bus width, dual-channel, rank interleave
 *   ERR_EN        inline ECC
 *
 * So the registers are seeded at reset to describe the machine's own RAM:
 * one rank starting at the DRAM base, ending at the last 16 MiB boundary
 * inside it, LPDDR5, 32-bit bus, no ECC. The end address rounds down, so a
 * -m that is not a multiple of 16 MiB is under-reported rather than over-.
 *
 * On silicon these registers are programmed by the OEI - the DDR-init image a
 * released imx-boot carries as an AHAB image of type 5, which the ROM runs on
 * the M33 before it loads the SM. The emulated ROM does not execute it (there
 * is no DRAM to train), so the machine presents the state it would have left.
 *
 * Measured effect, with NXP's released SM firmware: DEV_SM_PerfDramTypeGet()
 * reads DDR_SDRAM_CFG and now gets LPDDR5 where it previously left the type
 * unknown, and the window is no longer unmapped - a read of it while the
 * DDRMIX domain is powered used to be an external abort on the M33.
 *
 * What this does NOT fix, so as not to imply otherwise: U-Boot still prints a
 * DRAM size that is the EVK's compile-time 15.8 GiB rather than -m. The SM
 * answers SCMI MISC_DDR_INFO_GET with SM_ERR_POWER (-131) because it powers
 * the DDR domain off early in its own init and never powers it back on, so it
 * has no DDR regions to report whatever these registers say. Two candidate
 * causes have been ruled out by measurement: it is not the
 * "uninitialized by OEI" test (the DRAM PLL seed in hw/misc/imx95_anatop.c
 * satisfies that, and that power-down no longer happens), and it is not the
 * DRAM PLL rate (seeding a non-zero one changes nothing). What remains is a
 * DEV_SM_PowerStateSet(DDR, OFF) whose caller is inside the released SM
 * binary - a different build from the imx-sm source read here.
 */

#include "qemu/osdep.h"
#include "hw/misc/imx95_ddrc.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"

/* Register offsets (imx-sm, devices/MIMX95/MIMX95_DDRC.h). */
#define DDRC_CS_BNDS(n)         (0x000 + (n) * 8)
#define DDRC_CS_CONFIG(n)       (0x080 + (n) * 4)
#define DDRC_DDR_SDRAM_CFG      0x110
#define DDRC_ERR_EN             0x1000

/* CS_BNDS: start address in [31:16], end address in [15:0], 16 MiB units. */
#define DDRC_CS_BNDS_SA_SHIFT   16
#define DDRC_CS_BNDS_EA_SHIFT   0
#define DDRC_CS_BNDS_UNIT       (16 * MiB)

/* CS_CONFIG: rank enable. */
#define DDRC_CS_CONFIG_CS_EN    0x80000000u

/*
 * DDR_SDRAM_CFG. SDRAM_TYPE 001b is LPDDR5 and 100b is LPDDR4X; the SM maps
 * anything that is not 100b to LPDDR5. DBW 01b is a 32-bit interface, which
 * with DC_EN clear is the 4-byte bus the SM reports as a width of 32. Leaving
 * BA_INTLV_CTL at 0 with only rank 0 enabled is what makes the SM take the end
 * address from CS_BNDS[0] rather than CS_BNDS[1].
 */
#define DDRC_CFG_SDRAM_TYPE_SHIFT   24
#define DDRC_CFG_SDRAM_TYPE_LPDDR5  0x1
#define DDRC_CFG_DBW_32BIT          0x00080000u

static uint64_t imx95_ddrc_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX95DdrcState *s = opaque;

    if ((offset >> 2) >= IMX95_DDRC_NUM_REGS) {
        return 0;
    }
    return s->regs[offset >> 2];
}

static void imx95_ddrc_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    IMX95DdrcState *s = opaque;

    if ((offset >> 2) >= IMX95_DDRC_NUM_REGS) {
        qemu_log_mask(LOG_UNIMP, "imx95.ddrc: write 0x%" PRIx64
                      " to offset 0x%" HWADDR_PRIx " ignored\n",
                      value, offset);
        return;
    }
    s->regs[offset >> 2] = value;
}

static const MemoryRegionOps imx95_ddrc_ops = {
    .read = imx95_ddrc_read,
    .write = imx95_ddrc_write,
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

static void imx95_ddrc_reset(DeviceState *dev)
{
    IMX95DdrcState *s = IMX95_DDRC(dev);
    uint32_t units = s->ram_size / DDRC_CS_BNDS_UNIT;

    memset(s->regs, 0, sizeof(s->regs));

    /*
     * One rank covering the machine's DRAM. The addresses the SM derives are
     * DRAM-base relative, so the start unit is 0 and the end unit is the last
     * whole 16 MiB in it. A machine with less than one unit of DRAM would have
     * no describable rank, so it gets none rather than a wrapped one.
     */
    if (units == 0) {
        return;
    }
    s->regs[DDRC_CS_BNDS(0) >> 2] = (0u << DDRC_CS_BNDS_SA_SHIFT) |
                                    ((units - 1) << DDRC_CS_BNDS_EA_SHIFT);
    s->regs[DDRC_CS_CONFIG(0) >> 2] = DDRC_CS_CONFIG_CS_EN;
    s->regs[DDRC_DDR_SDRAM_CFG >> 2] =
        (DDRC_CFG_SDRAM_TYPE_LPDDR5 << DDRC_CFG_SDRAM_TYPE_SHIFT) |
        DDRC_CFG_DBW_32BIT;
}

static void imx95_ddrc_realize(DeviceState *dev, Error **errp)
{
    IMX95DdrcState *s = IMX95_DDRC(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &imx95_ddrc_ops, s,
                          TYPE_IMX95_DDRC, IMX95_DDRC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const Property imx95_ddrc_properties[] = {
    /*
     * Set by the SoC from the machine's RAM size. The default describes no
     * DRAM at all, which is what an unwired instance should report rather
     * than a size it invented.
     */
    DEFINE_PROP_UINT64("ram-size", IMX95DdrcState, ram_size, 0),
};

static const VMStateDescription vmstate_imx95_ddrc = {
    .name = TYPE_IMX95_DDRC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX95DdrcState, IMX95_DDRC_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx95_ddrc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx95_ddrc_realize;
    dc->vmsd = &vmstate_imx95_ddrc;
    device_class_set_legacy_reset(dc, imx95_ddrc_reset);
    device_class_set_props(dc, imx95_ddrc_properties);
    dc->desc = "i.MX 95 DDR controller (geometry reporting)";
}

static const TypeInfo imx95_ddrc_types[] = {
    {
        .name          = TYPE_IMX95_DDRC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX95DdrcState),
        .class_init    = imx95_ddrc_class_init,
    },
};

DEFINE_TYPES(imx95_ddrc_types)
