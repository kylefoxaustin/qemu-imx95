/*
 * NXP i.MX 95 SoC Implementation - v0.0.1 scaffold
 *
 * Modeled on hw/arm/fsl-imx8mp.c by Bernhard Beschow
 *
 * Copyright (c) 2026, Kyle Fox
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
#include "qemu/main-loop.h"
#include "system/address-spaces.h"
#include "system/cpus.h"
#include "system/system.h"
#include "hw/arm/bsa.h"
#include "hw/arm/boot.h"
#include "hw/arm/armv7m.h"
#include "hw/arm/fsl-imx95.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/i2c/i2c.h"
#include "hw/audio/wm8962.h"
#include "hw/core/irq.h"
#include "hw/intc/arm_gicv3.h"
#include "hw/intc/arm_gicv3_its_common.h"
#include "hw/pci-host/gpex.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pci_bus.h"
#include "hw/misc/unimp.h"
#include "hw/net/flexcan.h"
#include "hw/sd/sdhci.h"
#include "hw/ssi/ssi.h"
#include "system/blockdev.h"
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

    /*
     * On-chip RAM (OCRAM-A). The Linux DTS only declares a 96 KiB
     * sram1 slice at 0x204C0000, but U-Boot SPL is linked for
     * CONFIG_SPL_TEXT_BASE=0x20480000 with BSS+stack at 0x204D6000
     * (see references/uboot-imx/configs/imx95_19x19_evk_defconfig).
     * Cover the full SPL run + stack area as one writable region
     * spanning 0x20480000-0x204D8000. Linux's sram1 slice sits
     * inside this range and continues to work unchanged.
     * TODO: confirm precise OCRAM-A size against the i.MX 95 RM
     * "Memory Map" chapter; 384 KiB is sufficient for SPL but may
     * be smaller than real silicon.
     */
    [FSL_IMX95_OCRAM]                = { 0x20480000, 384 * KiB,  "ocram" },

    /*
     * System counter. dtsi: timer@44290000 reg = <... 0x30000>.
     * Logging stub for v0.1; the ARM generic timer in QEMU's CPU
     * model already provides time, and U-Boot SPL's timer_init()
     * reads CNTFRQ_EL0 rather than this MMIO block. Promote to a
     * real model if any guest write to this region surfaces.
     */
    [FSL_IMX95_SYSCNT]               = { 0x44290000, 192 * KiB,  "sysctr" },
    [FSL_IMX95_BBNSM]                = { 0x44440000, 4 * KiB,    "bbnsm" },
    [FSL_IMX95_XCACHE_PC]            = { 0x44400000, 0x800,      "xcache_pc" },
    [FSL_IMX95_XCACHE_PS]            = { 0x44400800, 0x800,      "xcache_ps" },
    [FSL_IMX95_BLK_CTRL_HSIOMIX]     = { 0x4c010000, 64 * KiB,   "blk_ctrl_hsiomix" },
    [FSL_IMX95_GPC]                  = { 0x44470000, 64 * KiB,   "gpc" },

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

    /* EdgeLock Secure Enclave mailboxes. dtsi: mailbox@47520000.. */
    [FSL_IMX95_ELE_MU]               = { 0x47520000, 64 * KiB,   "elemu0" },
    [FSL_IMX95_ELE_MU1]              = { 0x47530000, 64 * KiB,   "elemu1" },
    /* Fuse Shadow Block (MIMX95_FSB.h FSB_BASE). */
    [FSL_IMX95_FSB]                  = { 0x47510000, 64 * KiB,   "fsb" },
    /* VFCCU / FCCU fault-control unit (AON_VFCCU). */
    [FSL_IMX95_VFCCU]                = { 0x446b0000, 64 * KiB,   "vfccu" },
    /* A1 MU SRAM page; SM keeps the A55 CPU wait-semaphore here (+0x3f8). */
    [FSL_IMX95_CPU_SEMA]             = { 0x44231000, 4 * KiB,    "cpu-sema" },
    /* CortexA TMPSNS (CORTEXA__TMPSNS_BASE). */
    [FSL_IMX95_TMPSNS_CA]            = { 0x4a440000, 64 * KiB,   "tmpsns-ca" },
    /* AON_VFCCU (FCCU_BASE), AON_ERMA, NOC_SRAMCTL - SM eMcem/fabric init. */
    [FSL_IMX95_VFCCU_AON]            = { 0x44570000, 64 * KiB,   "vfccu-aon" },
    [FSL_IMX95_ERMA]                 = { 0x44560000, 64 * KiB,   "erma" },
    [FSL_IMX95_NOC_SRAMCTL]          = { 0x490a0000, 64 * KiB,   "noc-sramctl" },

    /*
     * Other MU instances U-Boot proper probes from DT but we don't
     * model yet (V2X, NETC, etc.). Logging stubs so mu_hal_init's
     * register accesses don't external-abort.
     */
    [FSL_IMX95_MU_47320000] = { 0x47320000, 64 * KiB, "mu@47320000" },
    [FSL_IMX95_MU_47350000] = { 0x47350000, 64 * KiB, "mu@47350000" },
    [FSL_IMX95_MU_47540000] = { 0x47540000, 64 * KiB, "mu@47540000" },
    [FSL_IMX95_MU_47550000] = { 0x47550000, 64 * KiB, "mu@47550000" },
    [FSL_IMX95_MU_47560000] = { 0x47560000, 64 * KiB, "mu@47560000" },
    [FSL_IMX95_MU_47570000] = { 0x47570000, 64 * KiB, "mu@47570000" },

    /*
     * Watchdogs. WDG3 is in the Linux DTS (the kernel sees it); WDG4
     * and WDG5 are SPL-only - U-Boot's arch_cpu_init() disables all
     * three. Bases from references/uboot-imx/arch/arm/include/asm/
     * arch-imx9/imx-regs.h.
     */
    [FSL_IMX95_WDOG2]                = { 0x442e0000, 64 * KiB,   "wdog2" },
    [FSL_IMX95_WDOG3]                = { 0x42490000, 64 * KiB,   "wdog3" },
    [FSL_IMX95_WDOG4]                = { 0x424a0000, 64 * KiB,   "wdog4" },
    [FSL_IMX95_WDOG5]                = { 0x424b0000, 64 * KiB,   "wdog5" },

    /*
     * GPIO bases from dtsi (gpio1@47400000) and U-Boot imx-regs.h
     * (GPIO2-5 at 0x43810000, 0x43820000, 0x43840000, 0x43850000).
     */
    [FSL_IMX95_GPIO1]                = { 0x47400000, 64 * KiB,   "gpio1" },
    [FSL_IMX95_GPIO2]                = { 0x43810000, 64 * KiB,   "gpio2" },
    [FSL_IMX95_GPIO3]                = { 0x43820000, 64 * KiB,   "gpio3" },
    [FSL_IMX95_GPIO4]                = { 0x43840000, 64 * KiB,   "gpio4" },
    [FSL_IMX95_GPIO5]                = { 0x43850000, 64 * KiB,   "gpio5" },

    /*
     * ARM SMMU-v3 at 0x490d0000 (U-Boot imx-regs.h SMMU_BASE_ADDR).
     * 128 KiB window covers CR0 / CR0_ACK / GBPA + the SMMU programming
     * page. Stub returns 0 on read so SPL's disable_smmuv3() sees
     * CR0.SMMUEN clear and early-exits.
     */
    [FSL_IMX95_SMMU]                 = { 0x490d0000, 128 * KiB,  "smmu" },

    /*
     * uSDHC controllers (compat "fsl,imx95-usdhc"). Bases + IRQs
     * from imx95.dtsi:1303-1340. Logging stubs in v0.2 prep; the
     * real fsl-esdhc model lands when we have a backing storage
     * story (FIT or AHAB container on a disk image).
     */
    [FSL_IMX95_USDHC1]               = { 0x42850000, 64 * KiB,   "usdhc1" },
    [FSL_IMX95_USDHC2]               = { 0x42860000, 64 * KiB,   "usdhc2" },
    [FSL_IMX95_USDHC3]               = { 0x428b0000, 64 * KiB,   "usdhc3" },

    /* LPI2C1..8 from imx95.dtsi - logging stubs in v0.2. */
    /*
     * NB: this LPI2C<N> enum uses SDK block numbering, which does NOT match the
     * Linux i2c bus number (i2c-N in dmesg / "N-00xx" device names). The MMIO
     * address is the only invariant; the Linux bus number comes from the dtb
     * `aliases` node (decompile + grep `i2c[0-9] = `), not from this enum.
     * For the 19x19 EVK dtb the aliases map addr -> Linux i2c-N:
     *   0x44340000 i2c-0   0x44350000 i2c-1
     *   0x42530000 i2c-2   0x42540000 i2c-3
     *   0x426b0000 i2c-4   0x426c0000 i2c-5
     *   0x426d0000 i2c-6   0x426e0000 i2c-7
     * (so e.g. LPI2C2 = 0x42540000 = Linux i2c-3, not "i2c-2"). A `pca953x
     * N-00xx: -110` on a bus with no real master here is just an unmodelled
     * stub, not a bug.
     */
    [FSL_IMX95_LPI2C1] = { 0x42530000, 64 * KiB, "lpi2c1" },
    [FSL_IMX95_LPI2C2] = { 0x42540000, 64 * KiB, "lpi2c2" },
    [FSL_IMX95_LPI2C3] = { 0x426b0000, 64 * KiB, "lpi2c3" },
    [FSL_IMX95_LPI2C4] = { 0x426c0000, 64 * KiB, "lpi2c4" },
    [FSL_IMX95_LPI2C5] = { 0x426d0000, 64 * KiB, "lpi2c5" },
    [FSL_IMX95_LPI2C6] = { 0x426e0000, 64 * KiB, "lpi2c6" },
    [FSL_IMX95_LPI2C7] = { 0x44340000, 64 * KiB, "lpi2c7" },
    [FSL_IMX95_LPI2C8] = { 0x44350000, 64 * KiB, "lpi2c8" },

    /* usb3 PHY (stub) + usb2 ChipIdea controller (a real model, below). */
    [FSL_IMX95_USB_PHY]  = { 0x4c1f0000, 64 * KiB, "usb_phy" },
    [FSL_IMX95_USB2]     = { 0x4c200000, 4 * KiB,  "usb2" },
};

/*
 * For every peripheral region we don't yet model, install a stub that
 * traces accesses but doesn't fault. This lets U-Boot / Linux probe
 * registers safely while we iterate.
 */
/*
 * NOP tr-write handler for the stub MUs. Its mere presence triggers
 * the IMX_MU model's "set TSR.TEn after TR write" path - effectively
 * making the stub mailboxes drop-and-ack on every send.
 */
static void fsl_imx95_stub_mu_tr_write(void *opaque, unsigned int idx,
                                       uint32_t value)
{
}

static bool fsl_imx95_install_unimplemented(FslImx95State *s, Error **errp)
{
    static const int unimplemented_regions[] = {
        FSL_IMX95_CCM, FSL_IMX95_IOMUXC,
        FSL_IMX95_TRDC_AON,
        /* BLK_CTRL_S_AONMIX has a real model (M7 CPU-WAIT gate) below. */
        FSL_IMX95_BLK_CTRL_NS_ANOMIX,
        FSL_IMX95_BLK_CTRL_WAKEUPMIX, FSL_IMX95_BLK_CTRL_NETCMIX,
        FSL_IMX95_WDOG4, FSL_IMX95_WDOG5,
        /* GPIO1..5 are real RGPIO models (imx95.gpio) below. */
        FSL_IMX95_SMMU,
        /*
         * LPI2C1/2/3/4 (0x42530000/42540000/426b0000/426c0000 = Linux
         * i2c-2/3/4/5) and LPI2C5 (0x426d0000 = Linux i2c-6) are real masters
         * below.
         */
        FSL_IMX95_LPI2C6,
        /*
         * LPI2C7 (0x44340000 = Linux i2c-0): the SM's PMIC bus, model below.
         * LPI2C8 (0x44350000 = Linux i2c-1): real master + adp5585, below.
         */
        /* usb2 (0x4c200000) is a real ChipIdea model below. */
        FSL_IMX95_USB_PHY,
    };

    for (size_t i = 0; i < ARRAY_SIZE(unimplemented_regions); i++) {
        int r = unimplemented_regions[i];
        create_unimplemented_device(fsl_imx95_memmap[r].name,
                                    fsl_imx95_memmap[r].addr,
                                    fsl_imx95_memmap[r].size);
    }

    /*
     * Bulk MU stubs for every mailbox the Linux imx95.dtsi declares
     * that we haven't already wired with a real IMX_MU model. Linux's
     * imx_mu driver probes each in turn (via fsl,imx95-mu /
     * fsl,imx95-mu-v2x compatibles); without backing devices the
     * probe takes a synchronous external abort reading PAR at offset
     * 0x4. mu_hal_init() is safe against zero reads (rr_count=0
     * skips the drain loop, SR bit 6 stays clear), so an
     * unimplemented-device stub is enough to clear the abort.
     *
     * The addresses + names come from
     * references/linux-imx/arch/arm64/boot/dts/freescale/imx95.dtsi
     * mailbox nodes. We exclude addresses already covered by real
     * models (sm_mu, ele_mu/elemu0, ele_mu1/elemu1, and the six
     * stub_mu[] entries at 0x47320000/0x47350000/0x47540000..0x47570000).
     */
    {
        static const struct {
            uint64_t addr;
            const char *name;
        } linux_mus[] = {
            /* mu7 (0x42430000) is a real cross-connect to the M7, below. */
            { 0x42730000, "mu8" },
            { 0x44220000, "mu1" },
            { 0x445d0000, "mu3" },
            { 0x445f0000, "mu4" },
            { 0x44630000, "mu6" },
            { 0x47300000, "v2x_mu4" },
            { 0x47330000, "v2x_mu7" },
            { 0x47340000, "v2x_mu8" },
            { 0x4ac60000, "cameramix_mu1" },
            { 0x4ac70000, "cameramix_mu2" },
            { 0x4ac80000, "cameramix_mu3" },
            { 0x4ac90000, "cameramix_mu4" },
            { 0x4aca0000, "cameramix_mu5" },
            { 0x4acb0000, "cameramix_mu6" },
            { 0x4acc0000, "cameramix_mu7" },
            { 0x4acd0000, "cameramix_mu8" },
            { 0x4ace0000, "cameramix_mu9" },
        };
        for (size_t i = 0; i < ARRAY_SIZE(linux_mus); i++) {
            create_unimplemented_device(linux_mus[i].name,
                                        linux_mus[i].addr, 64 * KiB);
        }
    }

    /*
     * Bulk stubs for the chip-wide register blocks the SM's config-op blob
     * (DEV_SM_ExecOp) pokes during DEV_SM_Init: TRDC/RDC and the per-MIX
     * block-control / clock-reset regions across the SoC. These are almost
     * all blind writes (a few read-modify-writes whose read value is not
     * acted on), so logging stubs that accept the accesses are enough to
     * let DEV_SM_Init complete; none of these subsystems is modelled.
     * create_unimplemented_device is a low-priority background, so the
     * cpu-sema RAM at 0x44231000 still overrides inside 0x44230000.
     * Addresses from the M33_ENUM enumeration of the config-op phase; the
     * real SCMI MUs (0x445c0000 = sm_mu_b, 0x44620000 = m7_sm_mu_b) are
     * intentionally absent - the SM's eager LMM_Init touches 0x44620000 as
     * MU9/MU5_B, so the real MU + its SMT alias at 0x44621000 must own that
     * window, not a dead logging stub.
     */
    {
        static const uint64_t sm_cfgop_regions[] = {
            0x42460000, 0x42470000, 0x42810000, 0x44230000, 0x44280000,
            0x49000000, 0x49010000, 0x49020000, 0x49060000,
            0x4ac40000, 0x4ac50000, 0x4aff0000, 0x4b040000, 0x4b050000,
            0x4c040000, 0x4c050000, 0x4c440000, 0x4c450000, 0x4c840000,
            0x4c850000, 0x4d810000, 0x4d840000, 0x4d850000,
        };
        for (size_t i = 0; i < ARRAY_SIZE(sm_cfgop_regions); i++) {
            g_autofree char *name =
                g_strdup_printf("sm-cfgop@0x%08" PRIx64, sm_cfgop_regions[i]);
            create_unimplemented_device(name, sm_cfgop_regions[i], 64 * KiB);
        }
    }

    /*
     * Peripheral MMIO the Linux built-in drivers touch once the REAL SM
     * powers/clocks their domains. With the C-stub SCMI these all deferred
     * ("Failed getting clock"); with the real SM they probe and take a
     * synchronous external abort on unmapped MMIO, killing PID 1. The list
     * is exactly the C-stub boot's "deferred probe pending" set (plus the
     * Neutron NPU). Logging stubs let the probes proceed without faulting;
     * v1.0 models these subsystems for real. Low priority, so the modelled
     * uSDHC / LPUART and the FSB RAM still override where they overlap.
     */
    {
        static const struct {
            uint64_t addr;
            uint64_t size;
        } linux_periph_regions[] = {
            /*
             * eDMA controllers: the driver maps the full DT reg window and
             * touches the per-channel pages (0x40 channels x 0x8000 stride =
             * 0x200000), so a 1 MiB stub faults on the high channels. Use the
             * DT-declared sizes: edma5 = 0x210000, edma3 = 0x200000.
             */
            /* edma2 (0x42000000) + edma1 (0x44000000) are real models below;
             * edma3 (0x42210000) stays a stub (no modelled consumer). */
            { 0x42210000, 0x210000 },
            { 0x42430000, 64 * KiB }, /* cm7 mailbox */
            { 0x42490000, 64 * KiB }, /* watchdog */
            /* pwm@424e0000/42510000 are real TPM-PWM models below. */
            /* 0x42540000 (Linux i2c-3): real master (wm8962 codec) below. */
            { 0x42530000, 64 * KiB }, /* i2c (Linux i2c-2) */
            { 0x42550000, 64 * KiB }, /* spi (lpspi3; enabled on 15x15 FRDM) */
            { 0x42590000, 64 * KiB }, /* serial */
            /* 0x425e0000 (flexspi1) is a real FlexSPI + NOR model below. */
            { 0x426b0000, 64 * KiB }, { 0x426c0000, 64 * KiB }, /* i2c */
            /* 0x426d0000 (Linux i2c-6) is a real master + pcal6524 below. */
            { 0x42710000, 64 * KiB }, /* spi */
            { 0x42850000, 64 * KiB }, { 0x42860000, 64 * KiB }, /* mmc */
            { 0x43810000, 64 * KiB }, { 0x43820000, 64 * KiB },
            { 0x43840000, 64 * KiB }, { 0x43850000, 64 * KiB }, /* gpio */
            { 0x44350000, 64 * KiB }, /* i2c */
            /* adc@44530000 is a real imx93-adc model below. */
            { 0x4ac10000, 64 * KiB }, /* camera csr (clock provider) */
            { 0x4ad00000, 64 * KiB }, /* display stream csr (clock provider) */
            { 0x4ad10000, 64 * KiB }, /* display master csr (mmio-mux) */
            { 0x4b010000, 64 * KiB }, /* syscon (dispmix csr) */
            { 0x4b0c0000, 64 * KiB }, /* lvds csr / ldb */
            { 0x4b0d0000, 0x20000 },  /* pixel interleaver bridge */
            /* 0x4b400000 display-controller (dpu) -> imx95.dpu status stub */
            { 0x4c010000, 64 * KiB }, /* hsio blk-ctl (clock provider) */
            { 0x4c100000, 64 * KiB }, /* usb3 dwc3 core */
            { 0x4c1f0000, 64 * KiB }, /* usb3 phy */
            /* usb2 (0x4c200000) is a real ChipIdea model; usbmisc stub below. */
            { 0x4c300000, 64 * KiB }, { 0x4c380000, 64 * KiB }, /* pcie dbi */
            /*
             * PCIe controller "app" + "atu" windows (dtsi pcie0/pcie1
             * reg-names). The imx_pcie driver maps "app" as its iomuxc_gpr and
             * reads IMX95_PCIE_RST_CTRL (app+0x3010) during core-reset; if the
             * window is unbacked the async probe worker takes a synchronous
             * external abort and dies, so wait_for_device_probe() hangs and
             * userspace never starts. Backing them (read-0) lets the probe fail
             * fast (PHY PLL never locks -> timeout -> returns) instead. These
             * probe only once the M.2/slot power regulators come up, which
             * happens now that the pcal6524 expander is modelled.
             */
            { 0x4c340000, 0x4000 }, { 0x4c360000, 64 * KiB }, /* pcie0 app/atu */
            { 0x4c3c0000, 0x4000 }, { 0x4c3e0000, 64 * KiB }, /* pcie1 app/atu */
            { 0x4c410000, 64 * KiB }, /* syscon */
            { 0x4c480000, 0x40000 },  /* vpu */
            { 0x4c4c0000, 64 * KiB }, /* vpu-ctrl */
            /* jpegdec/jpegenc (0x4c500000/4c550000) are real models below. */
            { 0x4c810000, 64 * KiB }, /* syscon */
            /* netc pcie ecam0 (0x4ca00000) is now a real gpex host (v2.x). */
            { 0x4cb00000, 0x100000 }, /* netc pcie ecam 1 (EMDIO domain) */
            { 0x4cde0000, 64 * KiB }, /* netc-blk-ctrl ierb */
            { 0x4cdf0000, 64 * KiB }, /* netc-blk-ctrl prb */
            { 0x4d900000, 0x100000 }, /* gpu */
            /* 0x4ab00000 neutron npu -> imx95.neutron (rproc + mailbox) below */
            /*
             * Audio (SAI/XCVR/MICFIL): not yet modelled. Stub them so a
             * board/use case enabling these nodes degrades gracefully
             * (reads-as-0) instead of taking an unmapped-access fault on the
             * first MMIO. (FlexCAN is a real model, wired below.)
             */
            /* sai1 (0x443b0000) + sai3 (0x42650000) + micfil (0x44520000) are
             * real audio front-ends below; sai4/5 + xcvr stay stubbed. */
            { 0x42660000, 64 * KiB }, /* sai4 */
            { 0x42670000, 64 * KiB }, /* sai5 */
            { 0x42680000, 64 * KiB }, /* xcvr (spdif) */
            { 0x4c880000, 64 * KiB }, /* sai2 */
            /*
             * Display (MIPI DSI) and camera (MIPI CSI / ISI / ISP) interface
             * controllers: not modelled. Stub them so enabling these DT nodes
             * degrades gracefully (reads-as-0) instead of faulting on the
             * first MMIO. The display-mix CSRs / LVDS-LDB CSR / DPU are stubbed
             * elsewhere; real display scanout + camera capture are roadmap
             * items (see the README).
             */
            { 0x4acf0000, 64 * KiB }, /* mipi dsi host + intf syscon */
            { 0x4ad20000, 64 * KiB }, /* mipi tx phy csr */
            { 0x4ad30000, 64 * KiB }, /* mipi csi0 */
            { 0x4ad40000, 64 * KiB }, /* mipi csi1 */
            { 0x4ad50000, 0x80000 },  /* isi (image sensing interface) */
            { 0x4ae00000, 64 * KiB }, /* neo isp registers */
            { 0x4afe0000, 64 * KiB }, /* neo isp stats */
        };
        for (size_t i = 0; i < ARRAY_SIZE(linux_periph_regions); i++) {
            g_autofree char *name =
                g_strdup_printf("linux-periph@0x%08" PRIx64,
                                linux_periph_regions[i].addr);
            create_unimplemented_device(name, linux_periph_regions[i].addr,
                                        linux_periph_regions[i].size);
        }
    }

    /*
     * displaymix IRQSTEER (interrupt-controller@0x4b0b0000). Funnels the DPU's
     * 512 interrupt inputs onto 8 GIC outputs (64 inputs each). The dpu95
     * driver reaches its frame-complete / shadow-load interrupts only through
     * this irqsteer, so without it those interrupts can't be requested and the
     * commit path falls back to timeouts. The reserved 6th/7th outputs share
     * GIC SPI 303 per the dtsi. The DPU's interrupts ride inputs 64..127
     * (output group 1 -> GIC SPI 215).
     */
    DeviceState *irqsteer = qdev_new("imx.irqsteer");
    {
        static const int irqsteer_spi[8] = { 214, 215, 216, 217,
                                             218, 303, 303, 219 };
        DeviceState *gicdev = DEVICE(&s->gic);
        int i;

        if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(irqsteer), errp)) {
            return false;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(irqsteer), 0, 0x4b0b0000);
        for (i = 0; i < 8; i++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(irqsteer), i,
                               qdev_get_gpio_in(gicdev, irqsteer_spi[i]));
        }
    }

    /*
     * DPU (display-controller @0x4b400000). The imx95.dpu model: reports the
     * blit-engine command-sequencer idle with FIFO space (the dpu95 probe
     * busy-waits that status with no timeout, so a zero-returning stub would
     * wedge wait_for_device_probe() forever); scans the primary plane's
     * FetchLayer framebuffer out of guest DRAM to a QEMU console (enough for
     * the dpu95 driver to bind and put the boot logo / fbcon on screen); and
     * drives a ~60 Hz frame-complete + shadow-load interrupt through the
     * irqsteer so atomic commits complete on interrupt instead of falling
     * through dpu95's ~10 s flip_done/SHDLD timeouts. The four modelled
     * sources map to irqsteer input lines (dtsi dpu interrupts): EXTDST0_SHDLOAD
     * ->64, DOMAINBLEND0_SHDLOAD ->70, DISENGCFG_SHDLOAD0 ->73, vblank ->74.
     * The 2nd pixel pipeline (stream 1 / CRTC 1) has its own sources on the
     * disp_irq2 block -> irqsteer lines EXTDST1_SHDLOAD ->192,
     * DOMAINBLEND1_SHDLOAD ->198, DISENGCFG_SHDLOAD1 ->201, vblank ->202 (all
     * in irqsteer group 3 -> GIC SPI 217). The downstream pixel-link / LDB /
     * DSI bridge blocks carry no pixels.
     */
    {
        static const int dpu_irqsteer_line[4] = { 64, 70, 73, 74 };
        static const int dpu_irqsteer_line2[4] = { 192, 198, 201, 202 };
        /* 2D blit completion: ComCtrl SW0..3 -> irqsteer lines 1..4 (dtsi). */
        static const int blit_irqsteer_line[4] = { 1, 2, 3, 4 };
        DeviceState *dpu = qdev_new("imx95.dpu");
        int i;

        /* id so QMP screendump can target the 2nd pipeline (device=dpu) */
        dpu->id = g_strdup("dpu");
        if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(dpu), errp)) {
            return false;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(dpu), 0, 0x4b400000);
        for (i = 0; i < 4; i++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(dpu), i,
                               qdev_get_gpio_in(irqsteer, dpu_irqsteer_line[i]));
        }
        for (i = 0; i < 4; i++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(dpu), 4 + i,
                qdev_get_gpio_in(irqsteer, blit_irqsteer_line[i]));
        }
        for (i = 0; i < 4; i++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(dpu), 8 + i,
                qdev_get_gpio_in(irqsteer, dpu_irqsteer_line2[i]));
        }
    }

    /*
     * Neutron NPU (bring-up). Two DT reg windows: the remoteproc's RESETCTRL
     * @0x4ab00000 (clock gate) and the device/mailbox @0x4ab00004. Linux loads
     * NeutronFirmware.elf onto the NPU core and drives inference over the
     * mailbox (completion on GIC SPI 318). The proprietary NPU compute is not
     * modelled; the model brings the whole stack up so the driver binds and the
     * TFLite/LiteRT Neutron delegate runs inferences to completion. Gated behind
     * the imx95-19x19-evk-neutron.dtso overlay (not in the base EVK dtb).
     */
    {
        DeviceState *gicdev = DEVICE(&s->gic);
        DeviceState *npu = qdev_new("imx95.neutron");

        if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(npu), errp)) {
            return false;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(npu), 0, 0x4ab00000);  /* RESETCTRL */
        sysbus_mmio_map(SYS_BUS_DEVICE(npu), 1, 0x4ab00004);  /* dev/mailbox */
        sysbus_connect_irq(SYS_BUS_DEVICE(npu), 0,
                           qdev_get_gpio_in(gicdev, 318));
    }

    return true;
}

/*
 * Release the M33 only when SM firmware has actually been loaded into its
 * ITCM. The SM image's vector table begins with the initial stack
 * pointer, so a non-zero first ITCM word means firmware is present; a
 * zeroed ITCM (no -device loader) leaves the M33 powered off, so a plain
 * A55-only Linux boot does not run the M33 into a HardFault lockup.
 *
 * Run from a bottom half scheduled at machine-init-done: by the time the
 * BH executes, the initial system reset has already committed the loaded
 * image into ITCM, side-stepping the reset-handler ordering problem (the
 * generic -device loader commits its ROM blob during the reset, after any
 * reset hook a device could register).
 */
/*
 * Boot-ROM handover (v1.x Step 5). On silicon the boot ROM loads each LM's
 * image and records it in a handover table the SM reads at startup
 * (DEV_SM_RomBootCpuGet via LMM_CpuInit) to learn which cores it owns and
 * their boot vectors - which is what lets the SM boot, manage and
 * fault-recover them. Our flow loads the M7 image directly (-device loader),
 * bypassing the ROM, so we fabricate a minimal handover with a single M7
 * EXEC entry. That makes LMM_CpuInit set s_bootFlags[M7], so the SM boots
 * the M7 via LMM_CpuStart - releasing CPUWAIT through our AONMIX model and
 * enabling the CM7_SYSRESETREQ fault IRQ. M7-only by design: the A55 boots
 * outside the SM's LMM path, so leaving its handover absent doesn't perturb
 * it (its s_bootFlags stays 0, exactly as before).
 *
 * The blob lives at 0x2003dc00 in the M33's DTCM, which is the SM's
 * __StackTop - reserved DTCM above the stack the SM never writes, exactly
 * where the ROM places it on hardware. Written here (M33 release time) so it
 * is in place before the SM runs LMM_CpuInit, and only when M7 firmware was
 * actually staged.
 */
#define M7_HANDOVER_DTCM_OFFSET (0x2003dc00ULL - FSL_IMX95_M33_DTCM_BASE)
#define M7_HANDOVER_BARKER      0xc0ffee16u
#define M7_HANDOVER_VER         0x0002u
#define M7_HANDOVER_SIZE        0x0100u
/*
 * img->flags: bits[7:0] = cpu (M7 = CPU_IDX_M7P = 1), [15:8] = type (EXEC = 0).
 */
#define M7_HANDOVER_IMG_FLAGS   0x00000001u

static void fsl_imx95_write_m7_handover(FslImx95State *s)
{
    uint8_t *dtcm = memory_region_get_ram_ptr(&s->m33_dtcm);
    uint8_t *h = dtcm + M7_HANDOVER_DTCM_OFFSET;
    const void *m7_itcm = memory_region_get_ram_ptr(&s->m7_itcm);

    /* Only if M7 firmware was actually staged into its ITCM. */
    if (ldl_le_p(m7_itcm) == 0) {
        return;
    }

    memset(h, 0, M7_HANDOVER_SIZE);
    stl_le_p(h + 0x00, M7_HANDOVER_BARKER);     /* barker */
    stw_le_p(h + 0x04, M7_HANDOVER_VER);        /* ver   */
    stw_le_p(h + 0x06, M7_HANDOVER_SIZE);       /* size  */
    h[0x08] = 1;                                /* num images */
    /* img[0] at 0x10: addr(u64), cpu(u16), resv(u16), flags(u32). */
    stq_le_p(h + 0x10, FSL_IMX95_M7_ITCM_SYSVIEW);
    stw_le_p(h + 0x18, 1);                      /* cpu = CPU_IDX_M7P */
    stl_le_p(h + 0x1c, M7_HANDOVER_IMG_FLAGS);
}

static void fsl_imx95_m33_start_bh(void *opaque)
{
    FslImx95State *s = opaque;
    const void *itcm = memory_region_get_ram_ptr(&s->m33_itcm);
    uint32_t initial_sp = ldl_le_p(itcm);

    if (initial_sp != 0 && s->m33.cpu) {
        CPUState *cs = CPU(s->m33.cpu);
        /* Place the M7 boot-ROM handover before the SM starts. */
        fsl_imx95_write_m7_handover(s);
        cs->halted = 0;
        cpu_resume(cs);
    }
}

/*
 * Same shape as fsl_imx95_m33_start_bh, applied to the M7. Vector[0] in
 * the M7's ITCM is the initial SP; firmware sets it to a non-zero value
 * (typically the top of DTCM), so a non-zero first word means a
 * "-device loader,...,cpu-num=7" actually populated the M7 image. With
 * no firmware loaded the M7 stays halted and a plain A55+M33 boot is
 * unaffected.
 */
static void fsl_imx95_m7_start_bh(void *opaque)
{
    FslImx95State *s = opaque;
    const void *m7_itcm = memory_region_get_ram_ptr(&s->m7_itcm);
    const void *m33_itcm = memory_region_get_ram_ptr(&s->m33_itcm);
    uint32_t initial_sp = ldl_le_p(m7_itcm);

    /*
     * When the SM (M33 firmware) is loaded it owns the M7 lifecycle: it boots
     * the M7 via DEV_SM_CpuStart (driven by our boot-ROM handover above),
     * which our AONMIX M7_CFG.WAIT model turns into the release and which also
     * enables the M7 fault IRQ. Force-starting the M7 here would race that
     * managed release, so skip it when the SM is present. This force start is
     * only for SM-less M7 boots (tests/m7-boot), where nothing else releases
     * the core.
     */
    if (ldl_le_p(m33_itcm) != 0) {
        return;
    }

    if (initial_sp != 0 && s->m7.cpu) {
        CPUState *cs = CPU(s->m7.cpu);
        cs->halted = 0;
        cpu_resume(cs);
    }
}

static void fsl_imx95_machine_done(Notifier *notifier, void *data)
{
    FslImx95State *s = container_of(notifier, FslImx95State, m33_machine_done);

    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            fsl_imx95_m33_start_bh, s);
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            fsl_imx95_m7_start_bh, s);
}

/*
 * SRC-driven M7 release path (v1.x Step 3). When the SM's
 * DEV_SM_CpuStart(M7) writes SRC_GEN.SCR.BOOT_RESET_RELEASE_M7MIX
 * (bit 12) from 0 to 1, hw/misc/imx95_src raises this irq. We defer
 * the actual cpu_resume to a BH so it runs in the AIO context rather
 * than inside the M33's TCG block that issued the SCR write. The
 * underlying fsl_imx95_m7_start_bh's vector[0] guard keeps the release
 * safe when no M7 firmware is loaded - in that case it's a no-op
 * (preserves the v1.0 Linux-only boot path's behaviour). Edge-triggered
 * so a re-write of the bit (locked, won't change) doesn't repeatedly
 * fire; the SRC model only raises on the 0 -> 1 transition.
 */
static void fsl_imx95_src_m7_release_handler(void *opaque, int n, int level)
{
    FslImx95State *s = opaque;

    if (!level) {
        return;
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            fsl_imx95_m7_start_bh, s);
}

/*
 * M7 run gate (v1.x Step 5). The SM manages the M7 lifecycle via its CPUWAIT
 * (AONMIX M7_CFG.WAIT): it releases the M7 on DEV_SM_CpuStart and holds it on
 * DEV_SM_CpuStop. The fault-recovery path (reaction=lm_reset) does stop then
 * start, cold-cycling the M7. imx95_aonmix drives the m7-run line with the
 * released state; we halt the M7 when held and reset+resume it when released.
 *
 * Both run as work on the M7 vCPU so the state change is applied with the
 * core stopped (the WAIT write happens in the M33's TCG block, a different
 * CPU). On release we cpu_reset the M7 first: for ARMv7-M that reloads SP/PC
 * from the reset vector in ITCM (which still holds the firmware image - we
 * don't wipe it), so the M7 re-runs from the top, exactly as after the SM's
 * real DEV_SM_CpuStart. cpu_reset re-applies start-powered-off (halted = 1),
 * so we clear halted afterwards to release it.
 */
static void fsl_imx95_m7_power_off_work(CPUState *cs, run_on_cpu_data data)
{
    cs->halted = 1;
}

static void fsl_imx95_m7_power_on_work(CPUState *cs, run_on_cpu_data data)
{
    cpu_reset(cs);
    cs->halted = 0;
}

static void fsl_imx95_m7_power_off_bh(void *opaque)
{
    FslImx95State *s = opaque;

    if (s->m7.cpu) {
        async_run_on_cpu(CPU(s->m7.cpu), fsl_imx95_m7_power_off_work,
                         RUN_ON_CPU_NULL);
    }
}

static void fsl_imx95_m7_power_on_bh(void *opaque)
{
    FslImx95State *s = opaque;

    if (s->m7.cpu) {
        async_run_on_cpu(CPU(s->m7.cpu), fsl_imx95_m7_power_on_work,
                         RUN_ON_CPU_NULL);
    }
}

/*
 * The M7's NVIC SYSRESETREQ gpio-out (pulsed when firmware writes
 * AIRCR.SYSRESETREQ) - forward it to the SM as CM7_SYSRESETREQ_IRQn on the
 * M33's NVIC, the way the SRC routes it on silicon.
 */
static void fsl_imx95_m7_sysresetreq_handler(void *opaque, int n, int level)
{
    FslImx95State *s = opaque;

    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->m33),
                                  FSL_IMX95_M7_SYSRESETREQ_M33_IRQ), level);
}

static void fsl_imx95_m7_run_handler(void *opaque, int n, int level)
{
    FslImx95State *s = opaque;

    /*
     * Defer to a BH so the cross-CPU async work is queued from the AIO
     * context rather than inside the M33's TCG block that wrote M7_CFG.
     * level: 1 = released (start), 0 = held (stop).
     */
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            level ? fsl_imx95_m7_power_on_bh
                                  : fsl_imx95_m7_power_off_bh, s);
}

static void fsl_imx95_realize(DeviceState *dev, Error **errp)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    FslImx95State *s = FSL_IMX95(dev);
    DeviceState *gicdev = DEVICE(&s->gic);
    const char *cpu_type = ms->cpu_type ?: ARM_CPU_TYPE_NAME("cortex-a55");
    /*
     * The 19x19 EVK has a fixed 6-core A55 cluster. The Cortex-M33 SM
     * core and the Cortex-M7 real-time core are additional, always-present
     * vCPUs not part of the A55 cluster (each has its own NVIC, not the
     * GIC), so the A55 wiring below is sized by the fixed cluster count
     * rather than ms->smp.cpus, which also counts the M33 and M7.
     */
    const unsigned n_a55 = FSL_IMX95_NUM_A55_CPUS;
    int i;

    if (ms->smp.cpus != n_a55 + 2) {
        error_setg(errp,
                   "%s: fixed topology is %u A55 + 1 M33 + 1 M7; "
                   "run with -smp %u (the default) - %d requested",
                   TYPE_FSL_IMX95, n_a55, n_a55 + 2, (int)ms->smp.cpus);
        return;
    }

    /* Instantiate the A55 cluster. */
    for (i = 0; i < n_a55; i++) {
        g_autofree char *name = g_strdup_printf("cpu%d", i);
        object_initialize_child(OBJECT(dev), name, &s->cpu[i], cpu_type);
    }

    for (i = 0; i < n_a55; i++) {
        if (n_a55 > 1 &&
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

        /*
         * MPIDR per-CPU. The Linux DTS (imx95.dtsi cpu@0..cpu@500)
         * puts each A55 in its own cluster: Aff1 = CPU index, Aff0 = 0.
         * So cpu[i] gets mp-affinity = i << 8. PSCI CPU_ON from Linux
         * uses these MPIDR values to identify the target CPU; without
         * the override QEMU would default to Aff0=i which doesn't match
         * the DT and PSCI CPU_ON fails for every secondary.
         * Source: references/linux-imx/arch/arm64/boot/dts/freescale/imx95.dtsi
         */
        object_property_set_uint(OBJECT(&s->cpu[i]), "mp-affinity",
                                 (uint64_t)i << 8, &error_abort);

        if (object_property_find(OBJECT(&s->cpu[i]), "has_el2")) {
            object_property_set_bool(OBJECT(&s->cpu[i]), "has_el2",
                                     !kvm_enabled(), &error_abort);
        }
        if (object_property_find(OBJECT(&s->cpu[i]), "has_el3")) {
            object_property_set_bool(OBJECT(&s->cpu[i]), "has_el3",
                                     !kvm_enabled(), &error_abort);
        }

        /*
         * U-Boot proper runs at EL3 (no TF-A below it in our model)
         * and issues OPTEE/PSCI SMCs assuming firmware will service
         * them. Without psci-conduit, those SMCs trap to U-Boot's own
         * EL3 sync vector and crash. Wire QEMU's built-in PSCI handler
         * to the SMC conduit so PSCI calls return cleanly and the
         * vendor-OS calls return UNDEFINED, which U-Boot handles as
         * "no OP-TEE present" and moves on.
         */
        object_property_set_int(OBJECT(&s->cpu[i]), "psci-conduit",
                                QEMU_PSCI_CONDUIT_SMC, &error_abort);

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

        qdev_prop_set_uint32(gicdev, "num-cpu", n_a55);
        qdev_prop_set_uint32(gicdev, "num-irq",
                             FSL_IMX95_NUM_IRQS + GIC_INTERNAL);

        redist_region_count = qlist_new();
        qlist_append_int(redist_region_count, n_a55);
        qdev_prop_set_array(gicdev, "redist-region-count", redist_region_count);

        object_property_set_link(OBJECT(&s->gic), "sysmem",
                                 OBJECT(get_system_memory()), &error_fatal);
        /* Enable LPIs so the GICv3 ITS (below) can deliver MSIs (v2.x/NETC). */
        qdev_prop_set_bit(gicdev, "has-lpi", true);
        if (!sysbus_realize(gicsbd, errp)) {
            return;
        }
        sysbus_mmio_map(gicsbd, 0, fsl_imx95_memmap[FSL_IMX95_GIC_DIST].addr);
        sysbus_mmio_map(gicsbd, 1, fsl_imx95_memmap[FSL_IMX95_GIC_REDIST].addr);

        /* Wire the per-CPU timer PPIs and IRQ/FIQ lines. */
        for (i = 0; i < n_a55; i++) {
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
            sysbus_connect_irq(gicsbd, i + n_a55,
                qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
            sysbus_connect_irq(gicsbd, i + 2 * n_a55,
                qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
            sysbus_connect_irq(gicsbd, i + 3 * n_a55,
                qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
        }
    }

    /*
     * GICv3 ITS (dtsi: msi-controller@48040000). Provides MSI translation
     * for the NETC PCIe endpoints (v2.x). Mirrors hw/arm/virt.c create_its:
     * link to the GICv3 (which has has-lpi set above) and map its frame.
     */
    DeviceState *its = qdev_new(its_class_name());

    object_property_set_link(OBJECT(its), "parent-gicv3",
                             OBJECT(&s->gic), &error_abort);
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(its), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(its), 0,
                    fsl_imx95_memmap[FSL_IMX95_GIC_ITS].addr);

    /*
     * NETC integrated-endpoint ECAM PCIe host (v2.x). dtsi:
     * netc-blk-ctrl@4cde0000's child pcie@4ca00000 ("pci-host-ecam-generic").
     * The ENETC MACs attach as endpoint functions on this bus (stage 4); their
     * MSIs route through the GICv3 ITS (above). bus/domain 0 only for now (the
     * minimal path uses the port's internal MDIO, so the bus-1 EMDIO ECAM is
     * left stubbed). The blk-ctrl model (later) gates Linux's probe of it.
     */
    {
        DeviceState *pcie = qdev_new(TYPE_GPEX_HOST);
        MemoryRegion *ecam_alias = g_new0(MemoryRegion, 1);
        MemoryRegion *mmio_alias = g_new0(MemoryRegion, 1);
        MemoryRegion *ecam_reg, *mmio_reg;
        int pin;

        /*
         * Relocate gpex's own root device off PCI devfn 00.0. The real NETC
         * ECAM is an integrated-endpoint "pci-host-ecam-generic" with no host
         * bridge: ENETC0 itself sits at 00.0 (ethernet@0,0). gpex always plants
         * a PCI_CLASS_BRIDGE_HOST device at devfn 0 (hw/pci-host/gpex.c), which
         * would collide with ENETC0. Move it to the unused slot 31.0 (no dtsi
         * node maps there) so ENETC0 can occupy 00.0 like the boards expect.
         */
        {
            Object *root = object_resolve_path_component(OBJECT(pcie),
                                                         "gpex_root");

            object_property_set_int(root, "addr", PCI_DEVFN(31, 0),
                                    &error_abort);
        }

        if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(pcie), errp)) {
            return;
        }

        /* ECAM config window (sysbus region 0) @0x4ca00000, 1 MiB. */
        ecam_reg = sysbus_mmio_get_region(SYS_BUS_DEVICE(pcie), 0);
        memory_region_init_alias(ecam_alias, OBJECT(pcie), "netc-pcie-ecam",
                                 ecam_reg, 0, 0x100000);
        memory_region_add_subregion(get_system_memory(), 0x4ca00000,
                                    ecam_alias);

        /*
         * 32-bit MMIO/BAR window (sysbus region 1) @0x4cc00000. Sized to the
         * dtsi ECAM ranges (ENETC/EMDIO/timer BARs, 0x4cc00000..0x4cd20000)
         * and kept clear of the netc-blk-ctrl ierb/prb stubs at 0x4cde0000/
         * 0x4cdf0000 (a wider window would shadow them and wedge the driver's
         * IERB lock poll).
         */
        mmio_reg = sysbus_mmio_get_region(SYS_BUS_DEVICE(pcie), 1);
        memory_region_init_alias(mmio_alias, OBJECT(pcie), "netc-pcie-mmio",
                                 mmio_reg, 0x4cc00000, 0x120000);
        memory_region_add_subregion(get_system_memory(), 0x4cc00000,
                                    mmio_alias);

        /* INTx -> GIC SPIs 304..307 (MSI-X via the ITS is the real path). */
        for (pin = 0; pin < PCI_NUM_PINS; pin++) {
            int spi = 304 + pin;

            sysbus_connect_irq(SYS_BUS_DEVICE(pcie), pin,
                               qdev_get_gpio_in(gicdev, spi));
            gpex_set_irq_num(GPEX_HOST(pcie), pin, spi);
        }

        s->netc_pcie_bus = PCI_HOST_BRIDGE(pcie)->bus;

        /*
         * ENETC PFs (PCI 1131:e101). Three station interfaces at the devfns the
         * BSP DT maps to ENETC0 (ethernet@0,0 = 00.0), ENETC1 (ethernet@8,0 =
         * devfn 0x40) and ENETC2 (ethernet@10,0 = devfn 0x80, the 10G port).
         * The MAC + BD-ring datapath is identical across all three - the wire
         * speed is a property of the (fixed-)link, not the model - so the 10G
         * port is just a third PF; the test DTB (tests/netc/patch-dtb.py)
         * enables it as a fixed-link 10gbase-r node, so enetc4_pf brings up a
         * 10G eth without the real Aquantia PHY / EMDIO. Each PF binds the next
         * -nic backend in order (qemu_configure_nic_device consumes nd_table
         * sequentially), so the ports map to the -nic devices on the cmdline.
         */
        {
            static const int enetc_devfn[FSL_IMX95_NUM_ENETC] = {
                PCI_DEVFN(0, 0), PCI_DEVFN(8, 0), PCI_DEVFN(0x10, 0),
            };
            int p;

            for (p = 0; p < FSL_IMX95_NUM_ENETC; p++) {
                s->enetc[p] = PCI_DEVICE(pci_new(enetc_devfn[p],
                                                 TYPE_FSL_ENETC));
                qemu_configure_nic_device(DEVICE(s->enetc[p]), true, NULL);
                pci_realize_and_unref(s->enetc[p], s->netc_pcie_bus,
                                      &error_fatal);
            }
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

    /*
     * System Manager mailbox (MU2) and SCMI server stub.
     *
     * Real silicon has the Cortex-M33 SM firmware on the far side of
     * MU2, talking SCMI back to whoever's running on the A55s
     * (U-Boot SPL, U-Boot proper, Linux). v0.1 replaces the M33 with
     * a C-side SCMI server stub that handles enough of the base /
     * clock / pinctrl protocols to get past U-Boot SPL's
     * imx9_probe_mu(). Replaced by a real M33 + SM firmware in v0.4+.
     *
     * Convert SM_SHMEM (sram0) from an unimplemented logging stub to
     * a real RAM region so the SMT shared-memory transport can be
     * read and written by both sides.
     */
    memory_region_init_ram(&s->sm_shmem, OBJECT(dev), "imx95-sm-shmem",
                           fsl_imx95_memmap[FSL_IMX95_SM_SHMEM].size,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                                fsl_imx95_memmap[FSL_IMX95_SM_SHMEM].addr,
                                &s->sm_shmem);

    /*
     * System counter (see comment on s->sysctr in fsl-imx95.h): a real
     * clockevent timer, since it is Linux's tick broadcast device for the
     * local-timer-stop idle state. Not in the unimplemented-region list.
     */
    s->sysctr = qdev_new("imx95.sysctr");
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(s->sysctr), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->sysctr), 0,
                    fsl_imx95_memmap[FSL_IMX95_SYSCNT].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->sysctr), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX95_SYSCNT_IRQ));

    /*
     * BBNSM region as RAM (see comment on s->bbnsm in fsl-imx95.h). The
     * M33 SM firmware reaches it via the m33_view alias of system memory.
     */
    memory_region_init_ram(&s->bbnsm, OBJECT(dev), "imx95-bbnsm",
                           fsl_imx95_memmap[FSL_IMX95_BBNSM].size,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                                fsl_imx95_memmap[FSL_IMX95_BBNSM].addr,
                                &s->bbnsm);

    /* HSIO BLK_CTRL (HSIOMIX) as RAM (see comment on s->blk_ctrl_hsiomix). */
    memory_region_init_ram(&s->blk_ctrl_hsiomix, OBJECT(dev),
                           "imx95-blk-ctrl-hsiomix",
                           fsl_imx95_memmap[FSL_IMX95_BLK_CTRL_HSIOMIX].size,
                           &error_fatal);
    memory_region_add_subregion(
        get_system_memory(),
        fsl_imx95_memmap[FSL_IMX95_BLK_CTRL_HSIOMIX].addr,
        &s->blk_ctrl_hsiomix);

    /*
     * Fuse Shadow Block as RAM. On real silicon the ROM/ELE populates it
     * from OTP before the SM runs; here the SM's DEV_SM_FuseInit just reads
     * fuse words (FSB->FUSE[]), so a readable region is enough to get past
     * the read. Fuse values default to zero; populate specific words later
     * if a DEV_SM_Init consumer needs a non-zero value.
     */
    memory_region_init_ram(&s->fsb, OBJECT(dev), "imx95-fsb",
                           fsl_imx95_memmap[FSL_IMX95_FSB].size, &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                                fsl_imx95_memmap[FSL_IMX95_FSB].addr, &s->fsb);

    /*
     * VFCCU (fault collection/control) as RAM. The SM's eMcem_Vfccu_InitCVfccu
     * writes fault config/flags and only re-reads what it wrote (e.g. clears
     * FHCFG0.FHIDEN and loops until it reads back 0), so a register-file is
     * enough; no fault-injection or IRQ behaviour is modelled (v1.0 item).
     */
    memory_region_init_ram(&s->vfccu, OBJECT(dev), "imx95-vfccu",
                           fsl_imx95_memmap[FSL_IMX95_VFCCU].size,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                                fsl_imx95_memmap[FSL_IMX95_VFCCU].addr,
                                &s->vfccu);

    /*
     * A55 CPU wait-semaphore SRAM. DEV_SM_CpuInit resets a Peterson-style
     * semaphore (flag0/flag1/turn) the SM keeps at 0x442313f8 (in the A1 MU
     * SRAM page); back it with RAM so the writes land.
     */
    memory_region_init_ram(&s->cpu_sema, OBJECT(dev), "imx95-cpu-sema",
                           fsl_imx95_memmap[FSL_IMX95_CPU_SEMA].size,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                                fsl_imx95_memmap[FSL_IMX95_CPU_SEMA].addr,
                                &s->cpu_sema);

    /*
     * CortexA TMPSNS as RAM. The SM's periodic sensor tick reads CTRL0
     * (TMPSNS_GetFilterBusy) and the data/threshold registers; with the
     * region zeroed the filter reads "idle" and the tick is a no-op. No
     * real thermal model (canned/quiet); enough to keep the post-init
     * sensor task from faulting on an unmapped read.
     */
    memory_region_init_ram(&s->tmpsns_ca, OBJECT(dev), "imx95-tmpsns-ca",
                           fsl_imx95_memmap[FSL_IMX95_TMPSNS_CA].size,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                                fsl_imx95_memmap[FSL_IMX95_TMPSNS_CA].addr,
                                &s->tmpsns_ca);

    /*
     * More register-file write targets the SM's eMcem/fabric init touches:
     * AON_VFCCU (FCCU central, 0x44570000), AON_ERMA (error reporting,
     * 0x44560000) and NOC_SRAMCTL (0x490a0000). The SM only writes config
     * and reads back what it wrote, so plain RAM suffices; no fault or
     * fabric behaviour is modelled (v1.0 item).
     */
    memory_region_init_ram(&s->vfccu_aon, OBJECT(dev), "imx95-vfccu-aon",
                           fsl_imx95_memmap[FSL_IMX95_VFCCU_AON].size,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                                fsl_imx95_memmap[FSL_IMX95_VFCCU_AON].addr,
                                &s->vfccu_aon);
    memory_region_init_ram(&s->erma, OBJECT(dev), "imx95-erma",
                           fsl_imx95_memmap[FSL_IMX95_ERMA].size, &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                                fsl_imx95_memmap[FSL_IMX95_ERMA].addr,
                                &s->erma);
    memory_region_init_ram(&s->noc_sramctl, OBJECT(dev), "imx95-noc-sramctl",
                           fsl_imx95_memmap[FSL_IMX95_NOC_SRAMCTL].size,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                                fsl_imx95_memmap[FSL_IMX95_NOC_SRAMCTL].addr,
                                &s->noc_sramctl);

    {
        SysBusDevice *mu_sbd = SYS_BUS_DEVICE(&s->sm_mu);

        if (!sysbus_realize(mu_sbd, errp)) {
            return;
        }
        sysbus_mmio_map(mu_sbd, 0,
                        fsl_imx95_memmap[FSL_IMX95_SM_MU].addr);
        sysbus_connect_irq(mu_sbd, 0,
            qdev_get_gpio_in(gicdev, FSL_IMX95_SM_MU_IRQ));
    }

    /*
     * ELE mailbox 1 and its responder stub. The MU is a plain
     * register file; the ele-server hangs off the MU's TR-write hook
     * and answers ELE-protocol commands from SPL with stub responses.
     * v0.1 only needs ele_get_info() to succeed for U-Boot SPL's
     * imx9_probe_mu() to complete.
     */
    {
        SysBusDevice *mu_sbd = SYS_BUS_DEVICE(&s->ele_mu1);

        if (!sysbus_realize(mu_sbd, errp)) {
            return;
        }
        sysbus_mmio_map(mu_sbd, 0,
                        fsl_imx95_memmap[FSL_IMX95_ELE_MU1].addr);
    }

    /*
     * Ordering is load-bearing here: the MU must be realised (so its
     * MMIO + state are valid) BEFORE the responder is realised,
     * because the responder's realize() calls imx_mu_set_tr_write_handler()
     * on the MU. The dependency is via a runtime callback registration
     * inside the responder's realize hook, not via a QOM property, so
     * a grep for "ele_mu1" won't surface it. If you reorder these
     * lines, the responder will register its handler on a still-zeroed
     * MU state and SPL's first ELE TR write will silently no-op.
     */
    object_property_set_link(OBJECT(&s->ele_server), "mu",
                             OBJECT(&s->ele_mu1), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ele_server), errp)) {
        return;
    }

    /*
     * ELE MU 0 (elemu0, 0x47520000) + its responder. This is the EdgeLock
     * channel the M33 SM uses during DEV_SM_Init (ELE_MuTx). Without a real
     * MU here the SM's MU_SendMsg spins on TSR.TEn, and without a responder
     * it then waits forever for an ELE reply. Same MU-then-responder
     * ordering rule as ele_mu1 above.
     */
    {
        SysBusDevice *mu_sbd = SYS_BUS_DEVICE(&s->ele_mu0);

        if (!sysbus_realize(mu_sbd, errp)) {
            return;
        }
        sysbus_mmio_map(mu_sbd, 0,
                        fsl_imx95_memmap[FSL_IMX95_ELE_MU].addr);
    }
    object_property_set_link(OBJECT(&s->ele_server0), "mu",
                             OBJECT(&s->ele_mu0), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ele_server0), errp)) {
        return;
    }

    /*
     * Realise the 6 stub MUs. Wiring imx_mu_set_tr_write_handler() to
     * a NOP makes IMX_MU re-assert TSR.TEn after every TR write, so
     * U-Boot's TX-empty poll on these unmodeled mailboxes completes
     * instead of looping. RR stays empty - no responder, no replies -
     * which matches "the mailbox is here but nothing is listening on
     * the other end."
     */
    {
        static const int stub_mu_regions[] = {
            FSL_IMX95_MU_47320000, FSL_IMX95_MU_47350000,
            FSL_IMX95_MU_47540000, FSL_IMX95_MU_47550000,
            FSL_IMX95_MU_47560000, FSL_IMX95_MU_47570000,
        };
        QEMU_BUILD_BUG_ON(ARRAY_SIZE(stub_mu_regions) !=
                          ARRAY_SIZE(s->stub_mu));
        for (i = 0; i < ARRAY_SIZE(s->stub_mu); i++) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(&s->stub_mu[i]);
            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0,
                            fsl_imx95_memmap[stub_mu_regions[i]].addr);
            imx_mu_set_tr_write_handler(&s->stub_mu[i],
                                        fsl_imx95_stub_mu_tr_write, NULL);
        }
    }

    /*
     * MUI_A5 @0x44610000 - the M7-side MUA of the M7<->SM SCMI channel.
     * Realised + mapped here so the window exists; its peer (MUB), the
     * SMT shared-memory page, and both cores' IRQs are wired in one block
     * after the M33 and M7 are realised (see the M7 cross-connect below).
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->m7_sm_mu), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->m7_sm_mu), 0, 0x44610000);

    /*
     * Wire the second ELE responder to the stub MU at 0x47550000.
     * imx9_probe_mu() in U-Boot proper (SCMI variant, arch/arm/
     * mach-imx/imx9/scmi/soc.c:1953) hard-codes "mailbox@47550000" as
     * the ELE channel; SPL's matching #if XPL_BUILD branch uses
     * "mailbox@47530000" (our ele_mu1). Mirror the responder here so
     * U-Boot proper's ele_get_info() completes instead of timing out.
     * The realize call below will overwrite the NOP tr-write handler
     * the stub_mu loop above just installed.
     *
     * stub_mu[3] = FSL_IMX95_MU_47550000 (must match stub_mu_regions
     * ordering above; the QEMU_BUILD_BUG_ON in the loop body anchors
     * the count but not the order).
     */
    object_property_set_link(OBJECT(&s->ele_server2), "mu",
                             OBJECT(&s->stub_mu[3]), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ele_server2), errp)) {
        return;
    }

    /*
     * Watchdog 3 (the Wakeup-domain wdog the kernel sees). U-Boot SPL
     * also disables it in arch_cpu_init() before the console comes up.
     * Stub model is enough to satisfy disable_wdog().
     */
    s->wdog3 = qdev_new("imx95.wdog");
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(s->wdog3), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->wdog3), 0,
                    fsl_imx95_memmap[FSL_IMX95_WDOG3].addr);

    /*
     * WDOG2 is the M33 SM's own watchdog: its reset handler unlocks it
     * (key 0xD928C520 -> CNT) and configures it before continuing. Same
     * ULP WDOG IP as wdog3; reuse the model (no timer behaviour, so it
     * never fires - the SM is free to configure and refresh it).
     */
    s->wdog2 = qdev_new("imx95.wdog");
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(s->wdog2), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->wdog2), 0,
                    fsl_imx95_memmap[FSL_IMX95_WDOG2].addr);

    /*
     * M33 XCACHE controllers. The SM enables + invalidates its caches at
     * boot (CCR ENCACHE + INVWn + GO, then polls); the model self-clears
     * the command bits so those polls converge.
     */
    s->xcache_pc = qdev_new("imx95.xcache");
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(s->xcache_pc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->xcache_pc), 0,
                    fsl_imx95_memmap[FSL_IMX95_XCACHE_PC].addr);
    s->xcache_ps = qdev_new("imx95.xcache");
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(s->xcache_ps), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->xcache_ps), 0,
                    fsl_imx95_memmap[FSL_IMX95_XCACHE_PS].addr);

    /*
     * LPI2C master the SM uses for the PMIC + IO-expander (SDK LPI2C1 at
     * 0x44340000 = our FSL_IMX95_LPI2C7). Real model with the serial
     * devices BRD_SM_SerialDevicesInit probes on its bus, so SM init
     * completes: PCAL6408A IO-expander (0x20), PF09 main PMIC (0x08), and
     * the PF5301/PF5302 (PF53) buck regulators (0x2a/0x29). The PCA2131 RTC
     * (0x53) needs no model - its PCA2131_Init() is a no-op.
     */
    s->lpi2c_pmic = qdev_new("imx.lpi2c");
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(s->lpi2c_pmic), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->lpi2c_pmic), 0,
                    fsl_imx95_memmap[FSL_IMX95_LPI2C7].addr);
    {
        I2CBus *i2c = I2C_BUS(qdev_get_child_bus(s->lpi2c_pmic, "i2c"));
        i2c_slave_create_simple(i2c, "pf09-pmic", 0x08);
        i2c_slave_create_simple(i2c, "pcal6408a", 0x20);
        i2c_slave_create_simple(i2c, "pf53-pmic", 0x2a);
        i2c_slave_create_simple(i2c, "pf53-pmic", 0x29);
    }

    /*
     * Linux i2c-6 (i2c@0x426d0000; the enum we label LPI2C5 - SDK numbering,
     * see the memmap note; the dtb-alias bus number and the address are what
     * matter). The EVK hangs a PCAL6524 IO-expander at 0x22 on it whose port-0
     * line 3 enables the USB host's 5V VBUS fixed-regulator. Model master + the
     * expander so the gpio-pca953x driver probes, the regulator turns on, and
     * the usb2 ChipIdea host leaves deferred-probe and binds (-> usb-kbd input
     * for the DPU display window). Other slaves on this bus (ptn5110 USB-PD at
     * 0x50) stay unmodelled and simply NAK.
     */
    s->lpi2c7 = qdev_new("imx.lpi2c");
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(s->lpi2c7), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->lpi2c7), 0,
                    fsl_imx95_memmap[FSL_IMX95_LPI2C5].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->lpi2c7), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX95_LPI2C7_IRQ));
    {
        I2CBus *i2c = I2C_BUS(qdev_get_child_bus(s->lpi2c7, "i2c"));
        i2c_slave_create_simple(i2c, "pcal6524", 0x22);
    }

    /*
     * GPC: the SM polls per-domain CMC_MODE_STAT and uses the sleep/wake
     * handshakes for CPU power-mode control. The model mirrors each status
     * register to its paired control so the SM's "request mode, wait for
     * mode" loops converge.
     */
    s->gpc = qdev_new("imx95.gpc");
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(s->gpc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->gpc), 0,
                    fsl_imx95_memmap[FSL_IMX95_GPC].addr);

    /*
     * SRC: the SM powers mix domains down/up via SLICE_SW_CTRL.PDN_SOFT and
     * polls each slice's FUNC_STAT until the transition completes. The model
     * derives FUNC_STAT from PDN_SOFT (instantaneous), so DEV_SM_Init's
     * power-down/up poll loops converge instead of spinning forever.
     */
    s->src = qdev_new("imx95.src");
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(s->src), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->src), 0,
                    fsl_imx95_memmap[FSL_IMX95_SRC].addr);
    /*
     * Wire SRC_GEN.SCR.BOOT_RESET_RELEASE_M7MIX rising edge to the M7
     * release path. This is the silicon-faithful M7 lifecycle handle:
     * the SM's CpuStart(M7_cpu_id) writes the bit, our SRC model raises
     * the named gpio-out, the handler schedules fsl_imx95_m7_start_bh.
     * Complements the existing machine-init-done BH (which serves the
     * M7-only test boots that have no SM running); fsl_imx95_m7_start_bh
     * is idempotent so both paths firing for the same M7 boot is safe.
     */
    qdev_connect_gpio_out_named(s->src, "m7mix-release", 0,
        qemu_allocate_irq(fsl_imx95_src_m7_release_handler, s, 0));

    /*
     * BLK_CTRL_S_AONMIX: real model for the M7 CPU-WAIT gate. Its M7_CFG.WAIT
     * bit reads as held at reset, so the SM's CPU_RunModeGet sees the M7 in
     * HOLD and runs the full DEV_SM_CpuStart (releasing CPUWAIT and enabling
     * the M7 fault IRQ). The m7-run gpio carries the released state (1 = run,
     * 0 = held); we wire it to the M7 run handler so the SM's stop/start of
     * the M7 LM (incl. fault-recovery lm_reset) actually cycles the core.
     */
    s->aonmix = qdev_new("imx95.aonmix");
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(s->aonmix), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->aonmix), 0,
                    fsl_imx95_memmap[FSL_IMX95_BLK_CTRL_S_AONMIX].addr);
    qdev_connect_gpio_out_named(s->aonmix, "m7-run", 0,
        qemu_allocate_irq(fsl_imx95_m7_run_handler, s, 0));

    /*
     * ANATOP/PLL: the SM's DVFS path powers a PLL up and polls
     * PLL_STATUS.LOCK, then enables each DFS and polls DFS_STATUS.DFS_OK
     * (an unbounded wait). The model makes LOCK mirror CTRL.POWERUP and
     * DFS_OK mirror each DFS's enable, so the A55 perf-level set completes
     * instead of timing out (-9) or hanging.
     */
    s->anatop = qdev_new("imx95.anatop");
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(s->anatop), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->anatop), 0,
                    fsl_imx95_memmap[FSL_IMX95_ANATOP].addr);

    /*
     * uSDHC1/2/3. Reuses QEMU's TYPE_IMX_USDHC (an SDHCI subclass with
     * the i.MX MMIO quirks). With no backing -drive the card line
     * stays unpopulated and SPL fails its MMC1 probe cleanly.
     * Attach storage with e.g.:
     *   -drive if=none,format=raw,file=sd.img,id=mmc0
     *   -device sd-card,drive=mmc0
     *
     * Realization order is intentional: we want usdhc1's sd-bus to be
     * the first one a bare `-device sd-card,drive=...` (no explicit
     * bus=) picks up, since SPL is hard-wired to boot from MMC1.
     * QEMU's anonymous-bus matching prefers the most-recently-added
     * bus of the type, so we realize the array in reverse so that
     * usdhc1 ends up at the top of the stack.
     */
    {
        static const struct {
            int region;
            int irq;
        } usdhc_table[FSL_IMX95_NUM_USDHCS] = {
            { FSL_IMX95_USDHC1, FSL_IMX95_USDHC1_IRQ },
            { FSL_IMX95_USDHC2, FSL_IMX95_USDHC2_IRQ },
            { FSL_IMX95_USDHC3, FSL_IMX95_USDHC3_IRQ },
        };

        for (i = FSL_IMX95_NUM_USDHCS - 1; i >= 0; i--) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(&s->usdhc[i]);

            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0,
                            fsl_imx95_memmap[usdhc_table[i].region].addr);
            sysbus_connect_irq(sbd, 0,
                qdev_get_gpio_in(gicdev, usdhc_table[i].irq));
        }
    }

    /*
     * FlexCAN1..5. Real controllers (hw/net/can/flexcan.c) on QEMU's CAN
     * bus subsystem; each connects to a host/emulated bus via the SoC's
     * canbus0..4 link (NULL = register-only, frames dropped). The stock NXP
     * EVK DT leaves these disabled, so a default boot does not probe them.
     */
    {
        static const struct {
            hwaddr addr;
            int irq;
        } flexcan_table[FSL_IMX95_NUM_FLEXCAN] = {
            { 0x443a0000, FSL_IMX95_FLEXCAN1_IRQ },
            { 0x425b0000, FSL_IMX95_FLEXCAN2_IRQ },
            { 0x42600000, FSL_IMX95_FLEXCAN3_IRQ },
            { 0x427c0000, FSL_IMX95_FLEXCAN4_IRQ },
            { 0x427d0000, FSL_IMX95_FLEXCAN5_IRQ },
        };

        for (i = 0; i < FSL_IMX95_NUM_FLEXCAN; i++) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(&s->flexcan[i]);

            if (s->canbus[i]) {
                object_property_set_link(OBJECT(&s->flexcan[i]), "canbus",
                                         OBJECT(s->canbus[i]), &error_abort);
            }
            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0, flexcan_table[i].addr);
            sysbus_connect_irq(sbd, 0,
                qdev_get_gpio_in(gicdev, flexcan_table[i].irq));
        }
    }

    /*
     * RGPIO controllers (fsl,imx95-gpio / fsl,imx8ulp-gpio). Five instances;
     * Linux gpio-vf610 binds each and requests only the first of the two GIC
     * SPIs (its chained handler walks the whole ISFR), so output 0 carries
     * every pin. Promoting these from the UNIMP stub makes output pins (resets
     * / regulator-enables) drive their consumers and the gpio interrupt-
     * controller latch real ISFR state instead of reading back zero.
     */
    {
        static const struct {
            hwaddr addr;
            int irq0, irq1;     /* GIC SPIs, per the dtsi interrupts cells */
        } gpio_table[] = {
            { 0x47400000, 10, 11 },   /* GPIO1 (AONMIX) */
            { 0x43810000, 49, 50 },   /* GPIO2 */
            { 0x43820000, 51, 52 },   /* GPIO3 */
            { 0x43840000, 53, 54 },   /* GPIO4 */
            { 0x43850000, 55, 56 },   /* GPIO5 */
        };
        int g;

        for (g = 0; g < ARRAY_SIZE(gpio_table); g++) {
            DeviceState *gpio = qdev_new("imx95.gpio");
            SysBusDevice *sbd = SYS_BUS_DEVICE(gpio);

            if (!sysbus_realize_and_unref(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0, gpio_table[g].addr);
            sysbus_connect_irq(sbd, 0,
                qdev_get_gpio_in(gicdev, gpio_table[g].irq0));
            sysbus_connect_irq(sbd, 1,
                qdev_get_gpio_in(gicdev, gpio_table[g].irq1));
        }
    }

    /*
     * HW JPEG codecs (dtsi jpegdec@4c500000 / jpegenc@4c550000, driver
     * mxc-jpeg). One descriptor-driven model serves both: on the decode kick
     * (CAST_CTRL) it decodes the source JPEG with libjpeg into the descriptor's
     * frame format; on the encode "GO" (CAST_MODE) it compresses the raw frame
     * back to a JPEG. Each has four per-slot GIC SPIs (jpegdec 295.., jpegenc
     * 291..); the m2m driver uses slot 0.
     */
    {
        static const struct {
            hwaddr addr;
            int irq;            /* first of four consecutive GIC SPIs */
        } jpeg_table[] = {
            { 0x4c500000, 295 },   /* jpegdec */
            { 0x4c550000, 291 },   /* jpegenc */
        };
        int t, j;

        for (t = 0; t < ARRAY_SIZE(jpeg_table); t++) {
            DeviceState *jpeg = qdev_new("imx95.jpeg");
            SysBusDevice *sbd = SYS_BUS_DEVICE(jpeg);

            if (!sysbus_realize_and_unref(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0, jpeg_table[t].addr);
            for (j = 0; j < 4; j++) {
                sysbus_connect_irq(sbd, j,
                    qdev_get_gpio_in(gicdev, jpeg_table[t].irq + j));
            }
        }
    }

    /*
     * USB2 ChipIdea controller (dtsi usb@4c200000, "fsl,imx7d-usb"). The EVK
     * runs it in host mode, so ci_hdrc binds the EHCI host and a -device
     * usb-kbd / usb-mouse on its bus becomes an input source for the DPU
     * display window. The companion usbmisc@4c200200 (non-core wrapper:
     * over-current / charger-detect glue) is a logging stub - the ChipIdea
     * driver only needs it to not fault. The usb3 DWC3 (0x4c100000) stays a
     * stub; the ChipIdea is all we need for HID.
     */
    {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->usb2);

        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, fsl_imx95_memmap[FSL_IMX95_USB2].addr);
        sysbus_connect_irq(sbd, 0,
            qdev_get_gpio_in(gicdev, FSL_IMX95_USB2_IRQ));
        create_unimplemented_device("usbmisc", 0x4c200200, 0x200);
    }

    /*
     * Audio. The EVK runs three ASoC cards: wm8962 (sai3 <-> wm8962 codec on
     * lpi2c4), micfil (PDM mic), and bt-sco (sai1 <-> a dummy codec). Their
     * SAI/MICFIL FIFOs are serviced by eDMA - sai1 + micfil on edma1
     * (0x44000000), sai3 on edma2 (0x42000000) - so a working DMA engine that
     * the fsl-edma driver can probe and allocate channels from is required for
     * the cards to register. The SAI/MICFIL models carry the register file the
     * fsl-sai/fsl-micfil drivers probe; sample movement rides the eDMA
     * datapath (no audio backend is attached).
     */
    {
        SysBusDevice *sbd;

        /* edma1 (fsl,imx93-edma3): 31 channels, default 0x10000 page stride;
         * channel N raises GIC SPI 96 + N. */
        object_property_set_uint(OBJECT(&s->edma1), "num-channels",
                                 FSL_IMX95_EDMA1_CHANNELS, &error_abort);
        sbd = SYS_BUS_DEVICE(&s->edma1);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, 0x44000000);
        for (i = 0; i < FSL_IMX95_EDMA1_CHANNELS; i++) {
            sysbus_connect_irq(sbd, i,
                qdev_get_gpio_in(gicdev, FSL_IMX95_EDMA1_IRQ_BASE + i));
        }

        /* edma2 (fsl,imx95-edma5): 64 channels at a 0x8000 page stride;
         * channels are paired so channel N raises GIC SPI 128 + N/2. */
        object_property_set_uint(OBJECT(&s->edma2), "num-channels",
                                 FSL_IMX95_EDMA2_CHANNELS, &error_abort);
        object_property_set_uint(OBJECT(&s->edma2), "chan-stride",
                                 FSL_IMX95_EDMA2_CHAN_STRIDE, &error_abort);
        sbd = SYS_BUS_DEVICE(&s->edma2);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, 0x42000000);
        for (i = 0; i < FSL_IMX95_EDMA2_CHANNELS; i++) {
            sysbus_connect_irq(sbd, i,
                qdev_get_gpio_in(gicdev, FSL_IMX95_EDMA2_IRQ_BASE + i / 2));
        }

        /* SAI1 (0x443b0000, bt-sco) and SAI3 (0x42650000, wm8962). */
        sbd = SYS_BUS_DEVICE(&s->sai[0]);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, 0x443b0000);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(gicdev, FSL_IMX95_SAI1_IRQ));

        sbd = SYS_BUS_DEVICE(&s->sai[1]);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, 0x42650000);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(gicdev, FSL_IMX95_SAI3_IRQ));

        /* MICFIL PDM mic (0x44520000), four interrupt lines. */
        {
            static const int micfil_irqs[IMX95_MICFIL_IRQS] =
                FSL_IMX95_MICFIL_IRQS;
            sbd = SYS_BUS_DEVICE(&s->micfil);
            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0, 0x44520000);
            for (i = 0; i < IMX95_MICFIL_IRQS; i++) {
                sysbus_connect_irq(sbd, i,
                    qdev_get_gpio_in(gicdev, micfil_irqs[i]));
            }
        }

        /*
         * lpi2c@42530000 (Linux i2c-2): real master so the on-board PCAL6408A
         * IO-expander (0x20) and PCA9632 LED controller (0x62) probe instead
         * of timing out (-110) on the old stub. Both are register-file i2c
         * devices, so the generic expander model serves both (leds-pca963x
         * only needs its MODE/LEDOUT/PWM register reads + writes to land).
         */
        {
            DeviceState *m = qdev_new("imx.lpi2c");
            I2CBus *i2c;

            if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(m), errp)) {
                return;
            }
            sysbus_mmio_map(SYS_BUS_DEVICE(m), 0,
                            fsl_imx95_memmap[FSL_IMX95_LPI2C1].addr);
            sysbus_connect_irq(SYS_BUS_DEVICE(m), 0,
                               qdev_get_gpio_in(gicdev, 58)); /* dtsi SPI 58 */
            i2c = I2C_BUS(qdev_get_child_bus(m, "i2c"));
            i2c_slave_create_simple(i2c, "pcal6408a", 0x20);
            i2c_slave_create_simple(i2c, "pcal6408a", 0x62);
        }

        /*
         * lpi2c i2c-4 (0x426b0000) and i2c-5 (0x426c0000): real masters so
         * their PCAL6408/PCAL6416 IO-expanders at 0x21 probe instead of -110.
         * i2c-4's expander gates the AQR/serdes regulators (the Path B power
         * chain); both are register-file devices served by the generic model.
         */
        {
            static const struct {
                hwaddr addr;
                int irq;            /* dtsi GIC SPI */
            } exp_i2c[] = {
                { 0x426b0000, 181 },   /* Linux i2c-4 */
                { 0x426c0000, 182 },   /* Linux i2c-5 */
            };
            int k;

            for (k = 0; k < ARRAY_SIZE(exp_i2c); k++) {
                DeviceState *m = qdev_new("imx.lpi2c");
                I2CBus *i2c;

                if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(m), errp)) {
                    return;
                }
                sysbus_mmio_map(SYS_BUS_DEVICE(m), 0, exp_i2c[k].addr);
                sysbus_connect_irq(SYS_BUS_DEVICE(m), 0,
                                   qdev_get_gpio_in(gicdev, exp_i2c[k].irq));
                i2c = I2C_BUS(qdev_get_child_bus(m, "i2c"));
                i2c_slave_create_simple(i2c, "pcal6408a", 0x21);
            }
        }

        /*
         * lpi2c@44350000 (Linux i2c-1): real master so the ADP5585 GPIO/PWM
         * IO-expander at 0x34 probes instead of -110. The adp5585 model seeds
         * its manufacturer-id register so the MFD driver accepts it and adds
         * its gpio + pwm sub-devices.
         */
        {
            DeviceState *m = qdev_new("imx.lpi2c");
            I2CBus *i2c;

            if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(m), errp)) {
                return;
            }
            sysbus_mmio_map(SYS_BUS_DEVICE(m),
                            0, fsl_imx95_memmap[FSL_IMX95_LPI2C8].addr);
            sysbus_connect_irq(SYS_BUS_DEVICE(m), 0,
                               qdev_get_gpio_in(gicdev, 14)); /* dtsi SPI 14 */
            i2c = I2C_BUS(qdev_get_child_bus(m, "i2c"));
            i2c_slave_create_simple(i2c, "adp5585", 0x34);
        }

        /*
         * TPM PWM controllers (dtsi pwm@424e0000/42510000, "fsl,imx7ulp-pwm").
         * pwm-imx-tpm reads PARAM for the channel count, so the UNIMP stub
         * (PARAM == 0) failed with "failed to add PWM chip". The real model
         * reports its channels and stores period/duty/enable, so the pwmchips
         * register and consumers (e.g. a PWM backlight) work.
         */
        {
            static const hwaddr tpm_pwm[] = { 0x424e0000, 0x42510000 };
            int k;

            for (k = 0; k < ARRAY_SIZE(tpm_pwm); k++) {
                DeviceState *p = qdev_new("imx.tpm-pwm");

                if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(p), errp)) {
                    return;
                }
                sysbus_mmio_map(SYS_BUS_DEVICE(p), 0, tpm_pwm[k]);
            }
        }

        /*
         * ADC (dtsi adc@44530000, "nxp,imx93-adc"). The imx93_adc driver power-
         * cycles + calibrates + converts via a small MSR/MCR state machine, so
         * the UNIMP stub (MSR == 0) looped on "ADC do not in power down mode".
         * The real model reflects power state, passes calibration and answers
         * conversions (synthetic samples) with the end-of-conversion IRQ.
         */
        {
            DeviceState *adc = qdev_new("imx93.adc");
            SysBusDevice *adc_sbd = SYS_BUS_DEVICE(adc);
            int k;

            if (!sysbus_realize_and_unref(adc_sbd, errp)) {
                return;
            }
            sysbus_mmio_map(adc_sbd, 0, 0x44530000);
            for (k = 0; k < 3; k++) {       /* dtsi GIC SPIs 199..201 */
                sysbus_connect_irq(adc_sbd, k,
                                   qdev_get_gpio_in(gicdev, 199 + k));
            }
        }

        /*
         * FlexSPI (dtsi flexspi1 spi@425e0000, "nxp,imx8mm-fspi") + a serial
         * NOR. The controller has two windows: the register block and a 128 MiB
         * AHB read window at 0x28000000. The UNIMP stub returned zeros, so
         * spi-nor's JEDEC/SFDP probe matched nothing and spi-nxp-fspi failed;
         * the real model replays the driver's LUT onto an SSI bus so the flash
         * enumerates and reads.
         *
         * The flash chip is a Micron mt25ql512ab (a fully-modelled SDR part)
         * rather than the EVK's actual mt35xu01gbba: spi-nor's mt35xu info
         * carries SPI_NOR_OCTAL_DTR_READ, so it forces an 8D-8D-8D transition
         * that QEMU's generic m25p80 does not model. The substitute exercises
         * the same controller datapath (the model being added here) over a NOR
         * the m25p80 fully supports. The DT node is a generic "jedec,spi-nor",
         * so it binds whatever part is present.
         */
        {
            DeviceState *fspi = qdev_new("imx.fspi");
            SysBusDevice *fspi_sbd = SYS_BUS_DEVICE(fspi);
            DeviceState *flash;
            DriveInfo *dinfo;

            if (!sysbus_realize_and_unref(fspi_sbd, errp)) {
                return;
            }
            sysbus_mmio_map(fspi_sbd, 0, 0x425e0000);   /* registers */
            sysbus_mmio_map(fspi_sbd, 1, 0x28000000);   /* AHB mmap window */
            sysbus_connect_irq(fspi_sbd, 0,
                               qdev_get_gpio_in(gicdev, 48)); /* dtsi SPI 48 */

            flash = qdev_new("mt25ql512ab");
            dinfo = drive_get(IF_MTD, 0, 0);
            if (dinfo) {
                qdev_prop_set_drive(flash, "drive",
                                    blk_by_legacy_dinfo(dinfo));
            }
            qdev_realize_and_unref(flash,
                                   qdev_get_child_bus(fspi, "spi"), errp);
            qdev_connect_gpio_out_named(fspi, "cs", 0,
                            qdev_get_gpio_in_named(flash, SSI_GPIO_CS, 0));
        }

        /*
         * 0x42540000 (Linux i2c-3): the wm8962 audio-codec bus. Real master so
         * the codec probes; the fsl-sai card then binds wm8962 as its codec.
         */
        s->lpi2c4 = qdev_new("imx.lpi2c");
        if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(s->lpi2c4), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(s->lpi2c4), 0, 0x42540000);
        sysbus_connect_irq(SYS_BUS_DEVICE(s->lpi2c4), 0,
                           qdev_get_gpio_in(gicdev, FSL_IMX95_LPI2C4_IRQ));
        {
            I2CBus *i2c = I2C_BUS(qdev_get_child_bus(s->lpi2c4, "i2c"));

            i2c_slave_create_simple(i2c, TYPE_WM8962, FSL_IMX95_WM8962_ADDR);
            /*
             * PCAL6408A IO-expander at 0x21 (dtsi gpio@21 on this i2c-3 bus -
             * distinct from the AQR/serdes expander at the same 0x21 on Linux
             * i2c-4 = 0x426b0000, which is a stub here). Its GPIOs gate
             * reg_audio_pwr - the fixed regulator that powers all 8
             * wm8962 supplies - so without it the codec stays in deferred probe
             * and the wm8962 ASoC card never registers. Reuse the register-file
             * expander model.
             */
            i2c_slave_create_simple(i2c, "pcal6408a", 0x21);
        }
    }

    /*
     * Cortex-M33 System Manager core (v0.6). Build the M33's 32-bit
     * address space: its private ITCM/DTCM RAM layered over a
     * low-priority alias of the A55 system memory (so the M33 can also
     * reach already-modelled peripherals and DRAM). The SM firmware is
     * loaded into ITCM separately via -device loader (see tests/sm-banner).
     *
     * The M33 boots first on real silicon; here the A55 cluster is the
     * primary boot path and the M33 is an additional core that runs only
     * when SM firmware is loaded. With no firmware, ITCM reads back zero,
     * so the M33 takes an early fault and locks up harmlessly without
     * disturbing the A55s.
     */
    memory_region_init(&s->m33_view, OBJECT(s), "imx95-m33-view",
                       4 * GiB);
    /* Low-priority window onto the A55 system memory (peripherals, DRAM). */
    memory_region_init_alias(&s->m33_sysmem_alias, OBJECT(s),
                             "imx95-m33-sysmem", get_system_memory(),
                             0, 4 * GiB);
    memory_region_add_subregion_overlap(&s->m33_view, 0,
                                        &s->m33_sysmem_alias, -1);
    /* Private TCM, layered on top. */
    memory_region_init_ram(&s->m33_itcm, OBJECT(s), "imx95-m33-itcm",
                           FSL_IMX95_M33_TCM_SIZE, &error_fatal);
    memory_region_add_subregion(&s->m33_view, FSL_IMX95_M33_ITCM_BASE,
                                &s->m33_itcm);
    memory_region_init_ram(&s->m33_dtcm, OBJECT(s), "imx95-m33-dtcm",
                           FSL_IMX95_M33_TCM_SIZE, &error_fatal);
    memory_region_add_subregion(&s->m33_view, FSL_IMX95_M33_DTCM_BASE,
                                &s->m33_dtcm);

    s->m33_cpuclk = clock_new(OBJECT(s), "m33-cpuclk");
    clock_set_hz(s->m33_cpuclk, FSL_IMX95_M33_CLK_HZ);

    qdev_prop_set_string(DEVICE(&s->m33), "cpu-type",
                         ARM_CPU_TYPE_NAME("cortex-m33"));
    qdev_prop_set_uint32(DEVICE(&s->m33), "num-irq", FSL_IMX95_M33_NUM_IRQ);
    qdev_prop_set_uint32(DEVICE(&s->m33), "init-svtor", FSL_IMX95_M33_SVTOR);
    /*
     * Hold the M33 in reset until we know SM firmware was loaded into its
     * ITCM (see fsl_imx95_m33_reset below). This keeps a plain A55 Linux
     * boot - which loads no SM image - from running the M33 on a zeroed
     * ITCM straight into a HardFault lockup.
     */
    qdev_prop_set_bit(DEVICE(&s->m33), "start-powered-off", true);
    qdev_connect_clock_in(DEVICE(&s->m33), "cpuclk", s->m33_cpuclk);
    object_property_set_link(OBJECT(&s->m33), "memory",
                             OBJECT(&s->m33_view), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->m33), errp)) {
        return;
    }

    /*
     * MU2 SCMI: instantiate the M33-side (MUB) endpoint of MU2 and peer-link
     * it to the A55-side MU, so the real SM firmware on the M33 services the
     * A55's SCMI. Linux's SCMI doorbell on MUA (0x445b0000) latches the
     * matching GIP on MUB and raises the M33's MU2_B IRQ; the SM's response
     * doorbell on MUB raises the A55's MU IRQ in turn. The MUB and its sram0
     * alias sit 0x10000 above their MUA counterparts (A2_MUB = A2_MUA +
     * 0x10000), and the SM's mb_mu computes its SMT buffer at MUB+0x1000 -
     * the same backing RAM Linux uses at MUA+0x1000, so both sides exchange
     * messages through one buffer. Done after the M33 is realized so its NVIC
     * GPIO inputs exist. (A plain A55 boot with no SM firmware loaded leaves
     * the M33 halted, so nothing answers SCMI - the SM image is required.)
     */
    {
        IMXMUState *mub = &s->sm_mu_b;
        hwaddr mub_base = fsl_imx95_memmap[FSL_IMX95_SM_MU].addr + 0x10000;
        hwaddr shmem_b = fsl_imx95_memmap[FSL_IMX95_SM_SHMEM].addr + 0x10000;

        object_initialize_child(OBJECT(s), "sm_mu_b", mub, TYPE_IMX_MU);
        if (!sysbus_realize(SYS_BUS_DEVICE(mub), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(mub), 0, mub_base);
        sysbus_connect_irq(SYS_BUS_DEVICE(mub), 0,
            qdev_get_gpio_in(DEVICE(&s->m33), FSL_IMX95_SM_MU_B_M33_IRQ));
        imx_mu_set_peer(&s->sm_mu, mub);

        memory_region_init_alias(&s->sm_shmem_b, OBJECT(s), "imx95-sm-shmem-b",
                                 &s->sm_shmem, 0,
                                 fsl_imx95_memmap[FSL_IMX95_SM_SHMEM].size);
        memory_region_add_subregion(get_system_memory(), shmem_b,
                                    &s->sm_shmem_b);
    }

    /*
     * Cortex-M7 real-time core (v1.x). Mirror of the M33 setup above with
     * M7-specific addresses: ITCM at M7-view 0x00000000 (the M7 boots
     * here, so its reset VTOR is 0 - the ARMv7-M default), DTCM at
     * M7-view 0x20000000. The same TCM RAM is also aliased into the
     * system view at 0x203c0000 / 0x20400000 per the upstream Linux
     * imx_rproc_att_imx95_m7 table, so an A55-side loader (Linux
     * remoteproc, U-Boot) can populate the M7's TCM from there. The
     * standalone -device loader,...,cpu-num=7 path writes through the
     * M7's own address space and does not need those aliases, but they
     * match real silicon and don't get in the way.
     */
    memory_region_init(&s->m7_view, OBJECT(s), "imx95-m7-view",
                       4ULL * GiB);
    memory_region_init_alias(&s->m7_sysmem_alias, OBJECT(s),
                             "imx95-m7-sysmem", get_system_memory(),
                             0, 4ULL * GiB);
    memory_region_add_subregion_overlap(&s->m7_view, 0,
                                        &s->m7_sysmem_alias, -1);

    memory_region_init_ram(&s->m7_itcm, OBJECT(s), "imx95-m7-itcm",
                           FSL_IMX95_M7_TCM_SIZE, &error_fatal);
    memory_region_add_subregion(&s->m7_view, FSL_IMX95_M7_ITCM_M7VIEW,
                                &s->m7_itcm);
    memory_region_init_ram(&s->m7_dtcm, OBJECT(s), "imx95-m7-dtcm",
                           FSL_IMX95_M7_TCM_SIZE, &error_fatal);
    memory_region_add_subregion(&s->m7_view, FSL_IMX95_M7_DTCM_M7VIEW,
                                &s->m7_dtcm);

    /* System-view aliases (A55 sees the same TCM RAM at these addrs). */
    memory_region_init_alias(&s->m7_itcm_sysalias, OBJECT(s),
                             "imx95-m7-itcm-sysalias", &s->m7_itcm, 0,
                             FSL_IMX95_M7_TCM_SIZE);
    memory_region_add_subregion(get_system_memory(),
                                FSL_IMX95_M7_ITCM_SYSVIEW,
                                &s->m7_itcm_sysalias);
    memory_region_init_alias(&s->m7_dtcm_sysalias, OBJECT(s),
                             "imx95-m7-dtcm-sysalias", &s->m7_dtcm, 0,
                             FSL_IMX95_M7_TCM_SIZE);
    memory_region_add_subregion(get_system_memory(),
                                FSL_IMX95_M7_DTCM_SYSVIEW,
                                &s->m7_dtcm_sysalias);

    s->m7_cpuclk = clock_new(OBJECT(s), "m7-cpuclk");
    clock_set_hz(s->m7_cpuclk, FSL_IMX95_M7_CLK_HZ);

    qdev_prop_set_string(DEVICE(&s->m7), "cpu-type",
                         ARM_CPU_TYPE_NAME("cortex-m7"));
    qdev_prop_set_uint32(DEVICE(&s->m7), "num-irq", FSL_IMX95_M7_NUM_IRQ);
    qdev_prop_set_uint32(DEVICE(&s->m7), "init-svtor", 0);
    qdev_prop_set_bit(DEVICE(&s->m7), "start-powered-off", true);
    qdev_connect_clock_in(DEVICE(&s->m7), "cpuclk", s->m7_cpuclk);
    object_property_set_link(OBJECT(&s->m7), "memory",
                             OBJECT(&s->m7_view), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->m7), errp)) {
        return;
    }

    /*
     * Route the M7's NVIC SYSRESETREQ to the SM (v1.x Step 5). On silicon the
     * SRC turns an M7 SYSRESETREQ into CM7_SYSRESETREQ_IRQn on the M33; the SM
     * takes the fault and, for the M7 LM (reaction=lm_reset), cold-resets it
     * via the SRC M7 mix-slice - which our m7mix-power path then turns into a
     * real halt+reset+resume of the M7. Without this wire, the NVIC's default
     * unconnected behaviour would reset the whole machine. Done after both
     * cores are realised so the M33 NVIC input exists.
     */
    qdev_connect_gpio_out_named(DEVICE(&s->m7), "SYSRESETREQ", 0,
        qemu_allocate_irq(fsl_imx95_m7_sysresetreq_handler, s, 0));

    /*
     * M7<->SM SCMI channel cross-connect (MUI_A5), the mirror of the MU2
     * block above. m7_sm_mu (MUA @0x44610000) is already mapped; here we
     * route its IRQ to the M7's NVIC, instantiate the SM-side MUB
     * (@0x44620000, = MUA + 0x10000) with its IRQ on the M33's NVIC, peer
     * the two, and back the SMT shared-memory page. The M7 rings its
     * doorbell on MUA -> GIP latches on MUB -> M33 MU5_B IRQ -> the SM's
     * MB_MU_Handler services MU9 and replies on MUB -> GIP on MUA -> M7
     * MU5_A IRQ. The SM already brought MU9 up in LMM_Init and is waiting.
     * Done after both cores are realised so their NVIC inputs exist.
     */
    {
        IMXMUState *mub = &s->m7_sm_mu_b;
        hwaddr mua_base = 0x44610000;
        hwaddr mub_base = mua_base + 0x10000;     /* 0x44620000 */
        hwaddr shmem    = mua_base + 0x1000;      /* 0x44611000 */
        hwaddr shmem_b  = mub_base + 0x1000;      /* 0x44621000 */

        sysbus_connect_irq(SYS_BUS_DEVICE(&s->m7_sm_mu), 0,
            qdev_get_gpio_in(DEVICE(&s->m7), FSL_IMX95_M7_SM_MU_M7_IRQ));

        object_initialize_child(OBJECT(s), "m7_sm_mu_b", mub, TYPE_IMX_MU);
        if (!sysbus_realize(SYS_BUS_DEVICE(mub), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(mub), 0, mub_base);
        sysbus_connect_irq(SYS_BUS_DEVICE(mub), 0,
            qdev_get_gpio_in(DEVICE(&s->m33), FSL_IMX95_M7_SM_MU_B_M33_IRQ));
        imx_mu_set_peer(&s->m7_sm_mu, mub);

        /*
         * 1 KiB SMT page at MUA+0x1000, aliased to MUB+0x1000 so both sides
         * share one buffer. The SM's three MU9 SCMI channels live at
         * +0x000/+0x080/+0x100 within it (SM_MB_MU_BUF_SIZE = 128), well
         * inside the page.
         */
        memory_region_init_ram(&s->m7_shmem, OBJECT(s), "imx95-m7-shmem",
                               1 * KiB, &error_fatal);
        memory_region_add_subregion(get_system_memory(), shmem, &s->m7_shmem);
        memory_region_init_alias(&s->m7_shmem_b, OBJECT(s), "imx95-m7-shmem-b",
                                 &s->m7_shmem, 0, 1 * KiB);
        memory_region_add_subregion(get_system_memory(), shmem_b,
                                    &s->m7_shmem_b);
    }

    /*
     * MU7: the A55<->M7 rpmsg notification mailbox. Linux's cm7 remoteproc
     * kicks the M7 by writing the virtqueue id into mu7's TR register
     * (mbox "tx" = <&mu7 0 1>); the M7's rpmsg_lite ISR reads it from RR on
     * its side and vice-versa. Unlike the SCMI MUs (doorbell + shared SMT
     * page) this channel carries data in TR/RR, which imx_mu forwards
     * across the peer link. No tr-write handler here - the peer link is the
     * consumer. Both endpoints are real IMX_MU: mu7_a (MUA @0x42430000,
     * Linux side, IRQ -> GIC SPI 234) and mu7_b (MUB @0x42440000, M7 side,
     * IRQ -> M7 NVIC 207). The vrings/buffers sit in the 0x88000000
     * carveout, ordinary DRAM both cores already see. Done after the M7 is
     * realised so its NVIC input exists.
     */
    {
        SysBusDevice *mua = SYS_BUS_DEVICE(&s->mu7_a);
        SysBusDevice *mub = SYS_BUS_DEVICE(&s->mu7_b);

        if (!sysbus_realize(mua, errp)) {
            return;
        }
        sysbus_mmio_map(mua, 0, 0x42430000);
        sysbus_connect_irq(mua, 0,
            qdev_get_gpio_in(gicdev, FSL_IMX95_MU7_A_IRQ));

        if (!sysbus_realize(mub, errp)) {
            return;
        }
        sysbus_mmio_map(mub, 0, 0x42440000);
        sysbus_connect_irq(mub, 0,
            qdev_get_gpio_in(DEVICE(&s->m7), FSL_IMX95_MU7_B_M7_IRQ));

        imx_mu_set_peer(&s->mu7_a, &s->mu7_b);
    }

    /*
     * Register the M33 and M7 auto-start checks from a machine-done
     * notifier so their reset hooks are installed AFTER the generic
     * -device loader's, and therefore run after the firmware images
     * have been written into the respective ITCMs.
     */
    s->m33_machine_done.notify = fsl_imx95_machine_done;
    qemu_add_machine_init_done_notifier(&s->m33_machine_done);

    /* All peripherals not yet modeled get logging stubs. */
    if (!fsl_imx95_install_unimplemented(s, errp)) {
        return;
    }
}

static void fsl_imx95_init(Object *obj)
{
    FslImx95State *s = FSL_IMX95(obj);
    int i;

    object_initialize_child(obj, "gic", &s->gic, TYPE_ARM_GICV3);

    object_initialize_child(obj, "m33", &s->m33, TYPE_ARMV7M);
    object_initialize_child(obj, "m7", &s->m7, TYPE_ARMV7M);

    for (i = 0; i < FSL_IMX95_NUM_LPUARTS; i++) {
        g_autofree char *name = g_strdup_printf("lpuart%d", i + 1);
        object_initialize_child(obj, name, &s->lpuart[i], TYPE_IMX_LPUART);
    }

    object_initialize_child(obj, "sm_mu", &s->sm_mu, TYPE_IMX_MU);
    object_initialize_child(obj, "ele_mu0", &s->ele_mu0, TYPE_IMX_MU);
    object_initialize_child(obj, "ele_mu1", &s->ele_mu1, TYPE_IMX_MU);
    for (i = 0; i < ARRAY_SIZE(s->stub_mu); i++) {
        g_autofree char *name = g_strdup_printf("stub_mu%d", i);
        object_initialize_child(obj, name, &s->stub_mu[i], TYPE_IMX_MU);
    }
    object_initialize_child(obj, "m7_sm_mu", &s->m7_sm_mu, TYPE_IMX_MU);
    object_initialize_child(obj, "mu7_a", &s->mu7_a, TYPE_IMX_MU);
    object_initialize_child(obj, "mu7_b", &s->mu7_b, TYPE_IMX_MU);
    object_initialize_child(obj, "ele-server", &s->ele_server,
                            TYPE_IMX95_ELE_SERVER);
    object_initialize_child(obj, "ele-server0", &s->ele_server0,
                            TYPE_IMX95_ELE_SERVER);
    object_initialize_child(obj, "ele-server2", &s->ele_server2,
                            TYPE_IMX95_ELE_SERVER);

    for (i = 0; i < FSL_IMX95_NUM_USDHCS; i++) {
        g_autofree char *name = g_strdup_printf("usdhc%d", i + 1);
        object_initialize_child(obj, name, &s->usdhc[i], TYPE_IMX_USDHC);
    }

    for (i = 0; i < FSL_IMX95_NUM_FLEXCAN; i++) {
        g_autofree char *name = g_strdup_printf("flexcan%d", i + 1);
        object_initialize_child(obj, name, &s->flexcan[i], TYPE_FLEXCAN);
    }

    object_initialize_child(obj, "usb2", &s->usb2, TYPE_CHIPIDEA);

    object_initialize_child(obj, "edma1", &s->edma1, TYPE_IMX95_EDMA);
    object_initialize_child(obj, "edma2", &s->edma2, TYPE_IMX95_EDMA);
    object_initialize_child(obj, "sai1", &s->sai[0], TYPE_IMX95_SAI);
    object_initialize_child(obj, "sai3", &s->sai[1], TYPE_IMX95_SAI);
    object_initialize_child(obj, "micfil", &s->micfil, TYPE_IMX95_MICFIL);
}

static const Property fsl_imx95_properties[] = {
    DEFINE_PROP_LINK("canbus0", FslImx95State, canbus[0], TYPE_CAN_BUS,
                     CanBusState *),
    DEFINE_PROP_LINK("canbus1", FslImx95State, canbus[1], TYPE_CAN_BUS,
                     CanBusState *),
    DEFINE_PROP_LINK("canbus2", FslImx95State, canbus[2], TYPE_CAN_BUS,
                     CanBusState *),
    DEFINE_PROP_LINK("canbus3", FslImx95State, canbus[3], TYPE_CAN_BUS,
                     CanBusState *),
    DEFINE_PROP_LINK("canbus4", FslImx95State, canbus[4], TYPE_CAN_BUS,
                     CanBusState *),
};

static void fsl_imx95_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = fsl_imx95_realize;
    device_class_set_props(dc, fsl_imx95_properties);
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
