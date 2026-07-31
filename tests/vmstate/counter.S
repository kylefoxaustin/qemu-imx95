/*
 * Bare-metal migration witness for the qemu-imx95 VMState round-trip test.
 *
 * Loaded flat at 0x80000000 (DDR base). Prints a one-shot banner, then loops
 * forever emitting a single incrementing letter ('A', 'B', 'C', ...) over
 * LPUART1 with a delay between each. The letter index lives in a callee-saved
 * register (x20), so:
 *
 *   - If a savevm/migrate round-trip preserves CPU state, the restored guest
 *     resumes MID-LOOP: it does NOT reprint the banner (PC round-tripped) and
 *     the letter sequence CONTINUES from where it left off (x20 round-tripped).
 *   - If VMState is broken and the guest resets instead, it reprints the banner
 *     and restarts the sequence at 'A' — which the harness flags as a failure.
 *
 * Polling-only, MMU off, runs at whatever EL the boot stub hands us. Same
 * LPUART1 conventions as tests/hello-imx95.
 */

    .equ    LPUART1_BASE,   0x44380000
    .equ    LPUART_STAT,    0x14
    .equ    LPUART_CTRL,    0x18
    .equ    LPUART_DATA,    0x1c
    .equ    STAT_TDRE,      0x00800000
    .equ    CTRL_TE,        0x00080000

    /* ~0.5 s per letter under TCG (calibrated on the reference host), so the
       harness can catch the guest mid-stream and the sequence stays well under
       26 letters within a test window. */
    .equ    DELAY,          0x60000000

    .section .text.startup, "ax"
    .global _start
_start:
    ldr     x19, =LPUART1_BASE
    ldr     w0, =CTRL_TE
    str     w0, [x19, LPUART_CTRL]

    /* One-shot banner: its absence after restore proves PC round-tripped. */
    adr     x2, banner
.Lbnext:
    ldrb    w3, [x2], #1
    cbz     w3, .Lbdone
.Lbpoll:
    ldr     w4, [x19, LPUART_STAT]
    tst     w4, #STAT_TDRE
    b.eq    .Lbpoll
    str     w3, [x19, LPUART_DATA]
    b       .Lbnext
.Lbdone:

    mov     x20, #0                     /* letter index, survives migration */
.Lloop:
    and     w5, w20, #0x1f              /* keep it a printable letter A.. */
    add     w3, w5, #'A'
.Lcpoll:
    ldr     w4, [x19, LPUART_STAT]
    tst     w4, #STAT_TDRE
    b.eq    .Lcpoll
    str     w3, [x19, LPUART_DATA]

    add     x20, x20, #1

    ldr     x6, =DELAY
.Ldelay:
    subs    x6, x6, #1
    b.ne    .Ldelay
    b       .Lloop

    .section .rodata
    .align  3
banner:
    .asciz  "vmstate-counter boot\n"
