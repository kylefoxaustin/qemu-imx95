/*
 * Cortex-M7 fault-recovery fixture for the i.MX 95 v1.x Step 5.
 *
 * Demonstrates the SM-orchestrated M7 power cycle: on its FIRST boot the M7
 * deliberately faults itself by asserting SYSRESETREQ. On silicon (and in
 * the QEMU model) the SRC routes that to the System Manager as
 * CM7_SYSRESETREQ_IRQn; the SM, for an LM with reaction=lm_reset (the M7 LM
 * on mx95evk), cold-resets the M7 LM - stopping then starting the M7 via the
 * SRC M7 mix-slice. The M7 then re-runs from its reset vector.
 *
 * To make the cycle observable from the A55 side, the firmware keeps a boot
 * counter in DTCM (which survives the core reset - the SM resets the CPU, not
 * the TCM RAM, and startup.S does not zero .bss). The A55-side test reads the
 * counter via the system-view DTCM alias at 0x20400000+ and asserts it
 * reached 2: proof the M7 faulted and the real SM cold-restarted it. With no
 * SM running, nothing restarts the M7 and the counter stays at 1.
 *
 * Memory map (M7-view DTCM; A55 sees the same RAM at 0x20400000):
 *   0x20000000  magic word 0xC0FFEE07   (A55: 0x20400000)
 *   0x20000040  boot counter            (A55: 0x20400040)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>

#define CM7_FINGERPRINT_ADDR    0x20000000U
#define CM7_FINGERPRINT_MAGIC   0xC0FFEE07U
#define CM7_BOOTCOUNT_ADDR      0x20000040U

/* ARMv7-M System Control Block: AIRCR at 0xE000ED0C. */
#define SCB_AIRCR               0xE000ED0CU
#define SCB_AIRCR_VECTKEY       (0x05FAU << 16)
#define SCB_AIRCR_SYSRESETREQ   (1U << 2)

static void trigger_sysresetreq(void)
{
    volatile uint32_t *aircr = (volatile uint32_t *)SCB_AIRCR;

    __asm__ volatile ("dsb" ::: "memory");
    *aircr = SCB_AIRCR_VECTKEY | SCB_AIRCR_SYSRESETREQ;
    __asm__ volatile ("dsb" ::: "memory");
}

int main(void)
{
    volatile uint32_t *magic = (volatile uint32_t *)CM7_FINGERPRINT_ADDR;
    volatile uint32_t *count = (volatile uint32_t *)CM7_BOOTCOUNT_ADDR;
    uint32_t boot;

    *magic = CM7_FINGERPRINT_MAGIC;

    /* Persists across the SM-driven core reset (TCM RAM is not wiped). */
    boot = *count + 1U;
    *count = boot;

    if (boot == 1U) {
        /*
         * Spin briefly so the SM has finished CpuStart (which enables
         * CM7_SYSRESETREQ_IRQn on the M33) before we fault, then assert
         * SYSRESETREQ. The SM cold-resets us; we re-enter main with boot == 2.
         */
        for (volatile uint32_t i = 0; i < 2000000U; i++) {
        }
        trigger_sysresetreq();
    }

    return 0;  /* startup.S spins in WFI from here */
}
