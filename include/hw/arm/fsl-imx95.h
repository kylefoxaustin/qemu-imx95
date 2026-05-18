/*
 * NXP i.MX 95 SoC definitions
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * Modeled on hw/arm/fsl-imx8mp.h
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Address and IRQ ground truth comes from two sources:
 *   - Linux DTS for everything Linux probes directly:
 *     arch/arm64/boot/dts/freescale/imx95.dtsi
 *   - NXP U-Boot's RM-derived header for the SCMI-routed peripherals
 *     U-Boot SPL pokes before SCMI is up:
 *     arch/arm/include/asm/arch-imx9/imx-regs.h
 * The latter set (CCM, ANATOP, IOMUXC, SRC, TRDC, the BLK_CTRL
 * aggregates) is currently logging-stub only - they are not modelled
 * because the NXP imx95-evk SPL routes all clock/pinmux/power-domain
 * access through SCMI to the M33 SM (and the SCMI server stub lands
 * elsewhere in v0.1).
 */

#ifndef FSL_IMX95_H
#define FSL_IMX95_H

#include "target/arm/cpu.h"
#include "hw/char/imx_lpuart.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/misc/imx95_ele_server.h"
#include "hw/misc/imx95_scmi_server.h"
#include "hw/misc/imx_mu.h"
#include "hw/sd/sdhci.h"
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
    FSL_IMX95_NUM_USDHCS    = 3,    /* uSDHC1..uSDHC3 */
    FSL_IMX95_NUM_IRQS      = 320,  /* GIC SPI budget (max is 1020) */
};

struct FslImx95State {
    SysBusDevice            parent_obj;

    ARMCPU                  cpu[FSL_IMX95_NUM_A55_CPUS];
    GICv3State              gic;
    MemoryRegion            ocram;
    MemoryRegion            sm_shmem;
    IMXLPUARTState          lpuart[FSL_IMX95_NUM_LPUARTS];
    IMXMUState              sm_mu;
    IMXMUState              ele_mu1;
    /*
     * Stub MUs for the V2X / NETC / spare instances the DT references
     * but nothing in the model talks to. Wired with a NOP tr-write
     * handler so writes drop and TSR.TEn re-asserts, letting U-Boot
     * iterate them cleanly without hanging on TX-empty polls.
     */
    IMXMUState              stub_mu[6];
    IMX95SCMIServerState    scmi_server;
    IMX95ELEServerState     ele_server;
    /*
     * Second ELE server attached to the stub MU at 0x47550000. The
     * imx9_probe_mu() SCMI variant in U-Boot proper hardcodes
     * "mailbox@47550000" as the ELE channel (the SPL variant uses
     * "mailbox@47530000", which is ele_mu1). Without an ELE responder
     * at 0x47550000, ele_get_info() times out (-110), board_init_f's
     * initcall fails, and the boot hangs before serial_init prints.
     * Mirror the responder so both SPL and U-Boot proper reach it.
     */
    IMX95ELEServerState     ele_server2;
    SDHCIState              usdhc[FSL_IMX95_NUM_USDHCS];
    DeviceState            *wdog3;
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

    /* System counter (drives the ARM generic timer). */
    FSL_IMX95_SYSCNT,

    /* LPUART block (8 instances; all currently stubbed) */
    FSL_IMX95_LPUART1,
    FSL_IMX95_LPUART2,
    FSL_IMX95_LPUART3,
    FSL_IMX95_LPUART4,
    FSL_IMX95_LPUART5,
    FSL_IMX95_LPUART6,
    FSL_IMX95_LPUART7,
    FSL_IMX95_LPUART8,

    /*
     * Clock / reset / pinmux infrastructure. SCMI-routed when M33 SM
     * is up; SPL touches them only before SCMI, kernel never. Logging
     * stubs in v0.1.
     */
    FSL_IMX95_CCM,
    FSL_IMX95_ANATOP,
    FSL_IMX95_IOMUXC,
    FSL_IMX95_SRC,
    FSL_IMX95_TRDC_AON,

    /* BLK_CTRL aggregates per power domain. Logging stubs in v0.1. */
    FSL_IMX95_BLK_CTRL_S_AONMIX,
    FSL_IMX95_BLK_CTRL_NS_ANOMIX,
    FSL_IMX95_BLK_CTRL_WAKEUPMIX,
    FSL_IMX95_BLK_CTRL_NETCMIX,

    /* System Manager SCMI: mu2 mailbox + sram0 shared-memory buffer */
    FSL_IMX95_SM_MU,
    FSL_IMX95_SM_SHMEM,

    /* EdgeLock Secure Enclave mailboxes (elemu0/elemu1 needed for SPL) */
    FSL_IMX95_ELE_MU,
    FSL_IMX95_ELE_MU1,

    /*
     * Other MU instances U-Boot proper probes from DT (mailbox@*).
     * Not yet modelled; logging stubs are enough for mu_hal_init to
     * complete cleanly (it reads PAR, writes RCR/TCR=0, reads SR — all
     * zeros are accepted and the drain loop is skipped).
     */
    FSL_IMX95_MU_47320000,
    FSL_IMX95_MU_47350000,
    FSL_IMX95_MU_47540000,
    FSL_IMX95_MU_47550000,
    FSL_IMX95_MU_47560000,
    FSL_IMX95_MU_47570000,

    /* Watchdogs. SPL disables WDG3/4/5 in arch_cpu_init(). */
    FSL_IMX95_WDOG3,
    FSL_IMX95_WDOG4,
    FSL_IMX95_WDOG5,

    /* GPIO controllers (1..5). SPL gpio_reset()s 2..5; #1 added for symmetry. */
    FSL_IMX95_GPIO1,
    FSL_IMX95_GPIO2,
    FSL_IMX95_GPIO3,
    FSL_IMX95_GPIO4,
    FSL_IMX95_GPIO5,

    /* ARM SMMU-v3 - SPL's disable_smmuv3() reads CR0; stub returns 0. */
    FSL_IMX95_SMMU,

    /*
     * uSDHC (SD / eMMC) controllers. SPL probes them to find a boot
     * device; v0.2 stubs them so we can observe access patterns
     * before promoting to a real fsl-esdhc model.
     */
    FSL_IMX95_USDHC1,
    FSL_IMX95_USDHC2,
    FSL_IMX95_USDHC3,

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
    /* uSDHC1/2/3 SPIs (dtsi: usdhc1=86, usdhc2=87, usdhc3=191). */
    FSL_IMX95_USDHC1_IRQ    = 86,
    FSL_IMX95_USDHC2_IRQ    = 87,
    FSL_IMX95_USDHC3_IRQ    = 191,
    /* mu2 is the SCMI mailbox to the M33 SM (dtsi:1655). */
    FSL_IMX95_SM_MU_IRQ     = 226,
};

#endif /* FSL_IMX95_H */
