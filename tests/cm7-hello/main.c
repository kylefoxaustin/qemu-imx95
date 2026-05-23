/*
 * Cortex-M7 hello firmware for the i.MX 95 v1.x M7 bring-up.
 *
 * Writes a machine-readable 32-bit magic word and a human-readable ASCII
 * fingerprint into a known DDR location (0x88200000), then returns; the
 * startup code halts the M7 in WFI from there.
 *
 * 0x88200000 sits in the wider shared M-core DDR carveout the upstream
 * BSP DTS lays out (vdev*vring@8800{0,1,2}000, rsc_table@88220000); the
 * fingerprint address is just below rsc_table, so writing here doesn't
 * collide with the resource-table contract.
 *
 * The QEMU-side functional test (added in a later v1.x patch) reads this
 * address from the A55 to confirm the M7 actually executed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>

#define CM7_FINGERPRINT_ADDR    0x88200000U
#define CM7_FINGERPRINT_MAGIC   0xC0FFEE07U  /* "coffee for the M7" */

int main(void)
{
    /*
     * volatile is correct here: cross-CPU shared DDR. The A55 reads this
     * fingerprint to verify the M7 actually executed, so the compiler
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
