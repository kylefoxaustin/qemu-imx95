/*
 * NXP i.MX 95 SoC definitions
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * Modeled on hw/arm/fsl-imx8mp.h
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * NOTE: This is v0.0.1 scaffolding. Addresses marked TODO are placeholders
 * that need to be verified against the i.MX 95 Reference Manual,
 * sections "System Address Map" and "Interrupt Mapping".
 */

#ifndef FSL_IMX95_H
#define FSL_IMX95_H

#include "target/arm/cpu.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_FSL_IMX95 "fsl-imx95"
OBJECT_DECLARE_SIMPLE_TYPE(FslImx95State, FSL_IMX95)

/*
 * Main DDR window. i.MX 95 maps DRAM starting at 0x8000_0000 on both
 * the 19x19 (LPDDR5) and 15x15 (LPDDR4X) EVK variants.
 */
#define FSL_IMX95_RAM_START         0x80000000ULL
#define FSL_IMX95_RAM_SIZE_MAX      (16ULL * GiB)

/*
 * i.MX 95 application processor complex:
 *   - 6x Cortex-A55  (the main APUs we emulate here)
 *   - 1x Cortex-M33  (System Manager, runs SM firmware - stubbed via SCMI)
 *   - 1x Cortex-M7   (real-time domain - not modeled in v0.0.1)
 * In v0.0.1 we only instantiate the A55 cluster. The M33/M7 cores can be
 * added later as heterogeneous CPU contexts within the same QEMU instance.
 */
enum FslImx95Configuration {
    FSL_IMX95_NUM_A55_CPUS  = 6,
    FSL_IMX95_NUM_LPUARTS   = 8,    /* LPUART1..LPUART8 */
    FSL_IMX95_NUM_IRQS      = 320,  /* GIC-600 supports up to 1020 SPIs; this
                                     * is a conservative budget for v0.0.1 */
};

struct FslImx95State {
    SysBusDevice    parent_obj;

    ARMCPU          cpu[FSL_IMX95_NUM_A55_CPUS];
    GICv3State      gic;
    MemoryRegion    ocram;
};

/*
 * Memory map region identifiers. The actual addresses live in the memmap
 * table in fsl-imx95.c. Anything tagged TODO is a placeholder; verify
 * against the RM before promoting beyond v0.0.1.
 */
enum FslImx95MemoryRegions {
    FSL_IMX95_RAM,

    /* GIC-600 (GICv3-compatible) */
    FSL_IMX95_GIC_DIST,
    FSL_IMX95_GIC_REDIST,
    FSL_IMX95_GIC_ITS,

    /* On-chip RAM */
    FSL_IMX95_OCRAM,

    /* AON / Wakeup domain peripherals - LPUART block */
    FSL_IMX95_LPUART1,
    FSL_IMX95_LPUART2,
    FSL_IMX95_LPUART3,

    /* Clock / reset / pinmux infrastructure (stubbed as unimplemented) */
    FSL_IMX95_CCM,
    FSL_IMX95_IOMUXC,
    FSL_IMX95_SRC,
    FSL_IMX95_GPC,
    FSL_IMX95_ANATOP,

    /* BLK_CTRL aggregates (one per power domain - stubbed) */
    FSL_IMX95_BLK_CTRL_AONMIX,
    FSL_IMX95_BLK_CTRL_WAKEUPMIX,
    FSL_IMX95_BLK_CTRL_NETCMIX,

    /* System Manager interface: M33 mailbox + SCMI shared memory */
    FSL_IMX95_SM_MU,
    FSL_IMX95_SM_SHMEM,

    /* EdgeLock Secure Enclave - stubbed; full model deferred */
    FSL_IMX95_ELE_MU,

    /* Trusted Resource Domain Controller - stubbed */
    FSL_IMX95_TRDC,

    /* Watchdogs */
    FSL_IMX95_WDOG1,
    FSL_IMX95_WDOG2,

    /* Reserved for future expansion */
    FSL_IMX95_NUM_REGIONS,
};

/*
 * IRQ assignments. All values are TODO until cross-referenced with the
 * RM's "GIC interrupt assignments" table.
 */
enum FslImx95Irqs {
    FSL_IMX95_LPUART1_IRQ   = 19,   /* TODO: verify */
    FSL_IMX95_LPUART2_IRQ   = 20,   /* TODO: verify */
    FSL_IMX95_LPUART3_IRQ   = 21,   /* TODO: verify */
};

#endif /* FSL_IMX95_H */
