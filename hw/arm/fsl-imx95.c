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
#include "hw/intc/arm_gicv3.h"
#include "hw/misc/unimp.h"
#include "hw/sd/sdhci.h"
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
    [FSL_IMX95_LPI2C1] = { 0x42530000, 64 * KiB, "lpi2c1" },
    [FSL_IMX95_LPI2C2] = { 0x42540000, 64 * KiB, "lpi2c2" },
    [FSL_IMX95_LPI2C3] = { 0x426b0000, 64 * KiB, "lpi2c3" },
    [FSL_IMX95_LPI2C4] = { 0x426c0000, 64 * KiB, "lpi2c4" },
    [FSL_IMX95_LPI2C5] = { 0x426d0000, 64 * KiB, "lpi2c5" },
    [FSL_IMX95_LPI2C6] = { 0x426e0000, 64 * KiB, "lpi2c6" },
    [FSL_IMX95_LPI2C7] = { 0x44340000, 64 * KiB, "lpi2c7" },
    [FSL_IMX95_LPI2C8] = { 0x44350000, 64 * KiB, "lpi2c8" },

    /* USB PHY + DWC3 controller (autoboot bootcmd touches these). */
    [FSL_IMX95_USB_PHY]  = { 0x4c1f0000, 64 * KiB, "usb_phy" },
    [FSL_IMX95_USB_DWC3] = { 0x4c200000, 64 * KiB, "usb_dwc3" },
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

static void fsl_imx95_install_unimplemented(FslImx95State *s)
{
    static const int unimplemented_regions[] = {
        FSL_IMX95_CCM, FSL_IMX95_ANATOP, FSL_IMX95_IOMUXC,
        FSL_IMX95_SRC, FSL_IMX95_TRDC_AON,
        FSL_IMX95_BLK_CTRL_S_AONMIX, FSL_IMX95_BLK_CTRL_NS_ANOMIX,
        FSL_IMX95_BLK_CTRL_WAKEUPMIX, FSL_IMX95_BLK_CTRL_NETCMIX,
        FSL_IMX95_ELE_MU,
        FSL_IMX95_WDOG4, FSL_IMX95_WDOG5,
        FSL_IMX95_GPIO1, FSL_IMX95_GPIO2, FSL_IMX95_GPIO3,
        FSL_IMX95_GPIO4, FSL_IMX95_GPIO5,
        FSL_IMX95_SMMU,
        FSL_IMX95_LPI2C1, FSL_IMX95_LPI2C2, FSL_IMX95_LPI2C3,
        FSL_IMX95_LPI2C4, FSL_IMX95_LPI2C5, FSL_IMX95_LPI2C6,
        FSL_IMX95_LPI2C7, FSL_IMX95_LPI2C8,
        FSL_IMX95_USB_PHY, FSL_IMX95_USB_DWC3,
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
            { 0x42430000, "mu7" },
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
static void fsl_imx95_m33_start_bh(void *opaque)
{
    FslImx95State *s = opaque;
    const void *itcm = memory_region_get_ram_ptr(&s->m33_itcm);
    uint32_t initial_sp = ldl_le_p(itcm);

    if (initial_sp != 0 && s->m33.cpu) {
        CPUState *cs = CPU(s->m33.cpu);
        cs->halted = 0;
        cpu_resume(cs);
    }
}

static void fsl_imx95_machine_done(Notifier *notifier, void *data)
{
    FslImx95State *s = container_of(notifier, FslImx95State, m33_machine_done);

    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            fsl_imx95_m33_start_bh, s);
}

static void fsl_imx95_realize(DeviceState *dev, Error **errp)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    FslImx95State *s = FSL_IMX95(dev);
    DeviceState *gicdev = DEVICE(&s->gic);
    const char *cpu_type = ms->cpu_type ?: ARM_CPU_TYPE_NAME("cortex-a55");
    /*
     * The 19x19 EVK has a fixed 6-core A55 cluster. The Cortex-M33 SM
     * core is an additional, always-present vCPU not part of the A55
     * cluster (it has its own NVIC, not the GIC), so the A55 wiring below
     * is sized by the fixed cluster count rather than ms->smp.cpus, which
     * also counts the M33.
     */
    const unsigned n_a55 = FSL_IMX95_NUM_A55_CPUS;
    int i;

    if (ms->smp.cpus != n_a55 + 1) {
        error_setg(errp,
                   "%s: fixed topology is %u A55 + 1 M33 SM core; "
                   "run with -smp %u (the default) - %d requested",
                   TYPE_FSL_IMX95, n_a55, n_a55 + 1, (int)ms->smp.cpus);
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
     * System counter region as RAM (see comment on s->sysctr in
     * fsl-imx95.h). Removed from the unimplemented-region list so
     * the RAM mapping isn't shadowed by the zero-returning stub.
     */
    memory_region_init_ram(&s->sysctr, OBJECT(dev), "imx95-sysctr",
                           fsl_imx95_memmap[FSL_IMX95_SYSCNT].size,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                                fsl_imx95_memmap[FSL_IMX95_SYSCNT].addr,
                                &s->sysctr);

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
    memory_region_add_subregion(get_system_memory(),
                                fsl_imx95_memmap[FSL_IMX95_BLK_CTRL_HSIOMIX].addr,
                                &s->blk_ctrl_hsiomix);

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

    object_property_set_link(OBJECT(&s->scmi_server), "mu",
                             OBJECT(&s->sm_mu), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->scmi_server), errp)) {
        return;
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
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->wdog3), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->wdog3), 0,
                    fsl_imx95_memmap[FSL_IMX95_WDOG3].addr);

    /*
     * WDOG2 is the M33 SM's own watchdog: its reset handler unlocks it
     * (key 0xD928C520 -> CNT) and configures it before continuing. Same
     * ULP WDOG IP as wdog3; reuse the model (no timer behaviour, so it
     * never fires - the SM is free to configure and refresh it).
     */
    s->wdog2 = qdev_new("imx95.wdog");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->wdog2), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->wdog2), 0,
                    fsl_imx95_memmap[FSL_IMX95_WDOG2].addr);

    /*
     * M33 XCACHE controllers. The SM enables + invalidates its caches at
     * boot (CCR ENCACHE + INVWn + GO, then polls); the model self-clears
     * the command bits so those polls converge.
     */
    s->xcache_pc = qdev_new("imx95.xcache");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->xcache_pc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->xcache_pc), 0,
                    fsl_imx95_memmap[FSL_IMX95_XCACHE_PC].addr);
    s->xcache_ps = qdev_new("imx95.xcache");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->xcache_ps), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->xcache_ps), 0,
                    fsl_imx95_memmap[FSL_IMX95_XCACHE_PS].addr);

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
     * Register the M33 auto-start check from a machine-done notifier so
     * its reset hook is installed AFTER the generic -device loader's, and
     * therefore runs after the SM image has been written into ITCM.
     */
    s->m33_machine_done.notify = fsl_imx95_machine_done;
    qemu_add_machine_init_done_notifier(&s->m33_machine_done);

    /* All peripherals not yet modeled get logging stubs. */
    fsl_imx95_install_unimplemented(s);
}

static void fsl_imx95_init(Object *obj)
{
    FslImx95State *s = FSL_IMX95(obj);
    int i;

    object_initialize_child(obj, "gic", &s->gic, TYPE_ARM_GICV3);

    object_initialize_child(obj, "m33", &s->m33, TYPE_ARMV7M);

    for (i = 0; i < FSL_IMX95_NUM_LPUARTS; i++) {
        g_autofree char *name = g_strdup_printf("lpuart%d", i + 1);
        object_initialize_child(obj, name, &s->lpuart[i], TYPE_IMX_LPUART);
    }

    object_initialize_child(obj, "sm_mu", &s->sm_mu, TYPE_IMX_MU);
    object_initialize_child(obj, "ele_mu1", &s->ele_mu1, TYPE_IMX_MU);
    for (i = 0; i < ARRAY_SIZE(s->stub_mu); i++) {
        g_autofree char *name = g_strdup_printf("stub_mu%d", i);
        object_initialize_child(obj, name, &s->stub_mu[i], TYPE_IMX_MU);
    }
    object_initialize_child(obj, "scmi-server", &s->scmi_server,
                            TYPE_IMX95_SCMI_SERVER);
    object_initialize_child(obj, "ele-server", &s->ele_server,
                            TYPE_IMX95_ELE_SERVER);
    object_initialize_child(obj, "ele-server2", &s->ele_server2,
                            TYPE_IMX95_ELE_SERVER);

    for (i = 0; i < FSL_IMX95_NUM_USDHCS; i++) {
        g_autofree char *name = g_strdup_printf("usdhc%d", i + 1);
        object_initialize_child(obj, name, &s->usdhc[i], TYPE_IMX_USDHC);
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
