/*
 * NXP i.MX 95 SoC definitions
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * Modeled on hw/arm/fsl-imx8mp.h
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Address and IRQ ground truth comes from the NXP BSP device tree at
 *   arch/arm64/boot/dts/freescale/imx95.dtsi
 * (kernel commit pinned in the NXP linux-imx fork). Peripherals that the
 * kernel reaches through SCMI -> M33 SM firmware (CCM, ANATOP, IOMUXC,
 * SRC, GPC, AONMIX/WAKEUPMIX block-control, TRDC, WDOG1/2) are not present
 * in the Linux DTS and are intentionally absent here; they will be added
 * back in v0.1 with RM-sourced bases when U-Boot SPL bring-up begins.
 */

#ifndef FSL_IMX95_H
#define FSL_IMX95_H

#include "target/arm/cpu.h"
#include "hw/char/imx_lpuart.h"
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
    IMXLPUARTState  lpuart[FSL_IMX95_NUM_LPUARTS];
};

/*
 * Memory map region identifiers. The actual addresses live in the memmap
 * table in fsl-imx95.c.
 */
enum FslImx95MemoryRegions {
    FSL_IMX95_RAM,

    /* GIC-600 (GICv3-compatible) */
    FSL_IMX95_GIC_DIST,
    FSL_IMX95_GIC_REDIST,
    FSL_IMX95_GIC_ITS,

    /* On-chip SRAM "sram1" */
    FSL_IMX95_OCRAM,

    /* LPUART block (8 instances; all currently stubbed) */
    FSL_IMX95_LPUART1,
    FSL_IMX95_LPUART2,
    FSL_IMX95_LPUART3,
    FSL_IMX95_LPUART4,
    FSL_IMX95_LPUART5,
    FSL_IMX95_LPUART6,
    FSL_IMX95_LPUART7,
    FSL_IMX95_LPUART8,

    /* NETCMIX block control (the only BLK_CTRL the kernel touches directly) */
    FSL_IMX95_BLK_CTRL_NETCMIX,

    /* System Manager SCMI: mu2 mailbox + sram0 shared-memory buffer */
    FSL_IMX95_SM_MU,
    FSL_IMX95_SM_SHMEM,

    /* EdgeLock Secure Enclave mailbox (elemu0) */
    FSL_IMX95_ELE_MU,

    /* Wakeup-domain watchdog (WDOG1/WDOG2 are M33-domain, not modeled) */
    FSL_IMX95_WDOG3,

    FSL_IMX95_NUM_REGIONS,
};

/*
 * IRQ assignments. All values are GIC SPI numbers extracted from
 * references/linux-imx/arch/arm64/boot/dts/freescale/imx95.dtsi.
 */
enum FslImx95Irqs {
    FSL_IMX95_LPUART1_IRQ   = 19,
    FSL_IMX95_LPUART2_IRQ   = 20,
    FSL_IMX95_LPUART3_IRQ   = 64,
    FSL_IMX95_LPUART4_IRQ   = 65,
    FSL_IMX95_LPUART5_IRQ   = 66,
    FSL_IMX95_LPUART6_IRQ   = 67,
    FSL_IMX95_LPUART7_IRQ   = 68,
    FSL_IMX95_LPUART8_IRQ   = 69,
    FSL_IMX95_WDOG3_IRQ     = 77,
};

#endif /* FSL_IMX95_H */
