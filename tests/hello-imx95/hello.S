/*
 * Hello-world bare-metal binary for the qemu-imx95 emulator.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Prints "Hello from i.MX 95!\n" over LPUART1 (the 19x19 EVK console
 * per the BSP device tree's stdout-path = &lpuart1) and then halts
 * in WFI. Polling-only, no interrupts, MMU off, runs at whatever EL
 * the QEMU arm_load_kernel boot stub hands us.
 */

    /* LPUART1 base and register offsets (per imx95.dtsi serial@44380000). */
    .equ    LPUART1_BASE,   0x44380000
    .equ    LPUART_STAT,    0x14
    .equ    LPUART_CTRL,    0x18
    .equ    LPUART_DATA,    0x1c

    /* STAT.TDRE = TX data register empty. CTRL.TE = transmitter enable. */
    .equ    STAT_TDRE,      0x00800000
    .equ    CTRL_TE,        0x00080000

    .section .text.startup, "ax"
    .global _start
_start:
    /* x0 = LPUART1 base. Use literal pool so we don't fight movz/movk. */
    ldr     x0, =LPUART1_BASE

    /* Enable the transmitter. */
    ldr     w1, =CTRL_TE
    str     w1, [x0, LPUART_CTRL]

    /* x2 walks the null-terminated message. */
    adr     x2, message

.Lnext:
    ldrb    w3, [x2], #1
    cbz     w3, .Lhalt

    /* Spin until STAT.TDRE goes high, then push the byte. */
.Lpoll:
    ldr     w4, [x0, LPUART_STAT]
    tst     w4, #STAT_TDRE
    b.eq    .Lpoll
    str     w3, [x0, LPUART_DATA]

    b       .Lnext

.Lhalt:
    wfi
    b       .Lhalt

    .section .rodata
    .align  3
message:
    .asciz  "Hello from i.MX 95!\n"
