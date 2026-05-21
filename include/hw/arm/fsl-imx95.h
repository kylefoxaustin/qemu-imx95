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
#include "hw/arm/armv7m.h"
#include "hw/char/imx_lpuart.h"
#include "hw/core/clock.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/misc/imx95_ele_server.h"
#include "hw/misc/imx95_scmi_server.h"
#include "hw/misc/imx_mu.h"
#include "hw/sd/sdhci.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/notify.h"
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
 * Cortex-M33 System Manager (SM) core. The SM firmware is linked to run
 * from the M33's tightly-coupled memories: code in ITCM, data/bss/stack
 * in DTCM. Sizes are rounded up to 256 KiB (the real TCM banks are
 * smaller; the SM image fits well within this). The reset vector table
 * sits at the ITCM base, so the M33's reset VTOR (init-svtor) points
 * there. The M33's 32-bit view is otherwise a superset of the A55 view
 * (it also reaches CCM/ANATOP/SRC/etc. that the A55 cannot) - for now we
 * give it the A55 system memory plus its private TCM; unmodelled
 * SM-only peripherals will fault and get stubbed in later milestones.
 */
#define FSL_IMX95_M33_ITCM_BASE     0x1ffc0000ULL
#define FSL_IMX95_M33_DTCM_BASE     0x20000000ULL
#define FSL_IMX95_M33_TCM_SIZE      (256 * KiB)
#define FSL_IMX95_M33_SVTOR         0x1ffc0000U
#define FSL_IMX95_M33_NUM_IRQ       256
#define FSL_IMX95_M33_CLK_HZ        333333333U  /* SM runs the M33 ~333 MHz */

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

    /*
     * Cortex-M33 System Manager core. v0.6 wires the CPU + its private
     * TCM so the real NXP SM firmware (m33_image.bin) can be loaded and
     * started; it is not yet the SCMI provider (the C-side stub still is).
     * m33_view is the M33's 32-bit address space: ITCM + DTCM RAM layered
     * over a low-priority alias of the A55 system memory.
     */
    ARMv7MState             m33;
    MemoryRegion            m33_view;
    MemoryRegion            m33_sysmem_alias;
    MemoryRegion            m33_itcm;
    MemoryRegion            m33_dtcm;
    Clock                  *m33_cpuclk;
    /*
     * The M33 starts powered-off and is released only if SM firmware was
     * actually loaded into its ITCM (checked at reset via this hook,
     * registered late so it runs after the -device loader populates the
     * ITCM). Without firmware the M33 stays halted, so a plain A55 Linux
     * boot is unaffected.
     */
    Notifier                m33_machine_done;

    GICv3State              gic;
    MemoryRegion            ocram;
    MemoryRegion            sm_shmem;
    /* Alias of sm_shmem into the MU2 MUB window (v0.9 SCMI swap). */
    MemoryRegion            sm_shmem_b;
    /* Fuse Shadow Block (FSB) backing RAM - SM reads fuse words at boot. */
    MemoryRegion            fsb;
    /* VFCCU backing RAM - SM eMcem init writes fault config/flags. */
    MemoryRegion            vfccu;
    /* A55 CPU wait-semaphore SRAM (A1 MU SRAM page). */
    MemoryRegion            cpu_sema;
    /* CortexA TMPSNS backing RAM (SM sensor tick; CTRL0=0 -> filter idle). */
    MemoryRegion            tmpsns_ca;
    /* More eMcem/fabric init targets backed by RAM (write-acceptors). */
    MemoryRegion            vfccu_aon;
    MemoryRegion            erma;
    MemoryRegion            noc_sramctl;
    /*
     * System counter region backed by RAM so writes stick. Linux's
     * imx-sysctr driver (timer-imx-sysctr.c) writes CMPCR/CMPCV
     * registers then polls readback until the values match. A pure
     * unimplemented-device stub drops the write and reads return 0,
     * so the readback loop never converges - kernel log fills with
     * "sysctr_timer_read_write write failed, retry: N". RAM backing
     * gives the loop the write-then-read-back semantics it needs.
     * Note: counter-value reads (CNTCV at 0x8/0xc, 0x20008/0x2000c)
     * return whatever was last written (typically 0) rather than a
     * live counter. Linux uses arch_timer as the primary clocksource;
     * the sysctr is a secondary event timer.
     */
    MemoryRegion            sysctr;
    /*
     * BBNSM (Battery-Backed Non-Secure Module: RTC, tamper, 8 general-
     * purpose registers) backed by RAM so writes stick. The NXP SM
     * firmware touches it during early init (brd_sm reset-record GPRs via
     * BBNSM_GprSetValue, and RTC setup that polls CTRL.RTC_EN back). The
     * driver only does write-then-read-back on these registers and never
     * reads VID/FEATURES, so a plain RAM region is sufficient. The A55
     * Linux side does not currently touch it.
     */
    MemoryRegion            bbnsm;
    /*
     * HSIO BLK_CTRL (HSIOMIX) backed by RAM. The SM's SystemInit does a
     * read-modify-write of the LFAST IO register at 0x4c0100c0; RAM gives
     * the read-back the write expects.
     */
    MemoryRegion            blk_ctrl_hsiomix;
    IMXLPUARTState          lpuart[FSL_IMX95_NUM_LPUARTS];
    IMXMUState              sm_mu;
    /* ELE MU the M33 SM uses for its EdgeLock channel (elemu0, 0x47520000). */
    IMXMUState              ele_mu0;
    IMXMUState              ele_mu1;
    /*
     * M33-side (MUB) endpoint of MU2, peer-linked to sm_mu (the A55-side
     * MUA @0x445b0000). Only instantiated in the v0.9 SCMI-swap path
     * (scmi-stub=off), mapped at 0x445c0000 with its IRQ routed to the M33
     * NVIC, so the real SM firmware services the A55's SCMI doorbells.
     */
    IMXMUState              sm_mu_b;
    /*
     * Stub MUs for the V2X / NETC / spare instances the DT references
     * but nothing in the model talks to. Wired with a NOP tr-write
     * handler so writes drop and TSR.TEn re-asserts, letting U-Boot
     * iterate them cleanly without hanging on TX-empty polls.
     */
    IMXMUState              stub_mu[6];
    IMX95SCMIServerState    scmi_server;
    IMX95ELEServerState     ele_server;
    /* ELE responder for the SM's elemu0 channel (0x47520000). */
    IMX95ELEServerState     ele_server0;
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
    DeviceState            *wdog2;
    DeviceState            *wdog3;
    /* M33 XCACHE controllers (PC @0x44400000, PS @0x44400800). */
    DeviceState            *xcache_pc;
    DeviceState            *xcache_ps;
    /*
     * LPI2C the SM uses for the PMIC / IO-expander (SDK "LPI2C1" at
     * 0x44340000, our memmap FSL_IMX95_LPI2C7). Real master model with a
     * PF09 PMIC + PCAL6408A on its bus; the other LPI2C instances stay
     * logging stubs.
     */
    DeviceState            *lpi2c_pmic;
    /* GPC (General Power Controller) - SM power-domain / CPU-mode control. */
    DeviceState            *gpc;
    /* SRC (System Reset Controller) - SM mix-slice power-down/up control. */
    DeviceState            *src;
    /* ANATOP/PLL - SM DVFS PLL lock + DFS status (A55 perf level). */
    DeviceState            *anatop;

    /*
     * When true (default), instantiate the C-stub SCMI server that answers
     * the A55's SCMI traffic on the A55-side MU (MUA @0x445b0000). Set false
     * to leave that channel for the real SM firmware on the M33 to service
     * (the v0.9 SCMI swap). Toggle via -global fsl-imx95.scmi-stub=false.
     */
    bool                    scmi_server_enabled;
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

    /* BBNSM (RTC / tamper / GPRs) - touched by the M33 SM firmware. */
    FSL_IMX95_BBNSM,

    /* M33 XCACHE controllers (enabled/invalidated by the SM at boot). */
    FSL_IMX95_XCACHE_PC,
    FSL_IMX95_XCACHE_PS,

    /* HSIO BLK_CTRL (HSIOMIX) - SM SystemInit RMWs the LFAST IO reg. */
    FSL_IMX95_BLK_CTRL_HSIOMIX,

    /* GPC (per-domain CPU_CTRL + GLOBAL) - SM power/CPU-mode control. */
    FSL_IMX95_GPC,

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

    /* Fuse Shadow Block - read by the SM's DEV_SM_FuseInit at boot. */
    FSL_IMX95_FSB,

    /* VFCCU (Fault Collection & Control Unit) - SM eMcem init touches it. */
    FSL_IMX95_VFCCU,
    /* A1 MU SRAM page holding the A55 CPU wait-semaphore (DEV_SM_CpuInit). */
    FSL_IMX95_CPU_SEMA,
    /* CortexA TMPSNS - SM sensor tick reads it (post-init periodic task). */
    FSL_IMX95_TMPSNS_CA,
    /* More SM eMcem/fabric init targets: AON_VFCCU, AON_ERMA, NOC_SRAMCTL. */
    FSL_IMX95_VFCCU_AON,
    FSL_IMX95_ERMA,
    FSL_IMX95_NOC_SRAMCTL,

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

    /*
     * Watchdogs. SPL disables WDG3/4/5 in arch_cpu_init(). WDOG2 is the
     * M33 SM's own watchdog (unlocked + configured in its reset handler).
     */
    FSL_IMX95_WDOG2,
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

    /*
     * LPI2C controllers. U-Boot proper probes them during the post-
     * banner dram_init / board_init phase (PMIC over I2C2 is the
     * critical one - PCA9450 on the EVK). Logging stubs are enough
     * to let the I2C transfer time out gracefully and DRAM init
     * fall back to defaults.
     */
    FSL_IMX95_LPI2C1,
    FSL_IMX95_LPI2C2,
    FSL_IMX95_LPI2C3,
    FSL_IMX95_LPI2C4,
    FSL_IMX95_LPI2C5,
    FSL_IMX95_LPI2C6,
    FSL_IMX95_LPI2C7,
    FSL_IMX95_LPI2C8,

    /*
     * USB PHY + DWC3 controller. U-Boot autoboot's bootcmd touches
     * these via dwc3_imx8mp_glue_configure (reads at 0x4c1f0000).
     * Logging stubs prevent the data abort that otherwise resets
     * the board mid-autoboot.
     */
    FSL_IMX95_USB_PHY,
    FSL_IMX95_USB_DWC3,

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

/*
 * M33 NVIC external interrupt number for the MU2 B-side (MU2_B_IRQn in the
 * SM's MIMX95 device header) - the IRQ the SM enables to receive A55 SCMI
 * doorbells on its side of MU2.
 */
#define FSL_IMX95_SM_MU_B_M33_IRQ   227

#endif /* FSL_IMX95_H */
