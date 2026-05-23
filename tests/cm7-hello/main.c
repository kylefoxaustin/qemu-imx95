/*
 * Cortex-M7 hello firmware for the i.MX 95 v1.x M7 bring-up.
 *
 * Writes a machine-readable 32-bit magic word and a human-readable ASCII
 * fingerprint to the start of the M7's own DTCM (M7-view 0x20000000),
 * then returns; startup.S halts the M7 in WFI from there. The fingerprint
 * is observable from the A55 side via the system-view alias of the M7
 * DTCM at 0x20400000 (per the upstream Linux imx_rproc_att_imx95_m7
 * attribute table, which says "A55 sees M7 DTCM here").
 *
 * Writing into the M7's private TCM rather than shared DDR is
 * deliberate: a Step-2 fixture should not depend on the placement of
 * Linux's initramfs, kernel, or other DRAM allocations - the M7 has
 * its own memory and the architectural cross-view alias is the right
 * channel for an A55-side observer.
 *
 * The QEMU-side functional/integration tests (added in this step) read
 * 0x20400000 from the A55 side to confirm the M7 executed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>

/* M7-view of the start of DTCM; A55 sees the same RAM at 0x20400000. */
#define CM7_FINGERPRINT_ADDR    0x20000000U
#define CM7_FINGERPRINT_MAGIC   0xC0FFEE07U  /* "coffee for the M7" */

int main(void)
{
    /*
     * volatile is correct here: cross-CPU observed memory. The A55 reads
     * this fingerprint via the system-view alias of M7 DTCM
     * (0x20400000) to verify the M7 actually executed, so the compiler
     * must not elide the stores or reorder them across the return.
     */
    volatile uint8_t *fp = (volatile uint8_t *)CM7_FINGERPRINT_ADDR;
    const char *msg = "CM7 RUNNING\n";
    uint32_t magic = CM7_FINGERPRINT_MAGIC;
    unsigned i;

    /* 32-bit magic word, little-endian (M7 + A55 are both LE on imx95). */
    fp[0] = (uint8_t)(magic);
    fp[1] = (uint8_t)(magic >> 8);
    fp[2] = (uint8_t)(magic >> 16);
    fp[3] = (uint8_t)(magic >> 24);

    /* ASCII fingerprint immediately after the magic word. */
    for (i = 0; msg[i]; i++) {
        fp[4 + i] = (uint8_t)msg[i];
    }
    fp[4 + i] = 0;

    return 0;  /* startup.S spins in WFI from here */
}
