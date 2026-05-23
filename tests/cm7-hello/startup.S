/*
 * Minimal Cortex-M7 startup for the i.MX 95 M7 real-time core.
 *
 * Layout (matches imx_rproc_att_imx95_m7 in upstream Linux):
 *   ITCM (rwx, M7-view) at 0x00000000, 256 KiB - vector table + code
 *   DTCM (rwx, M7-view) at 0x20000000, 256 KiB - stack + data + bss
 *
 * Vector table conventions for ARMv7-M:
 *   [0]  initial SP   (loaded by hardware on reset)
 *   [1]  Reset_Handler (thumb bit applied via .thumb_func)
 *   [2]  NMI
 *   [3]  HardFault
 *   ... 60 more slots filled with Default_Handler
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

    .syntax unified
    .cpu cortex-m7
    .thumb

    .section .vectors, "a", %progbits
    .global vectors
    .type vectors, %object
vectors:
    .word _stack_top
    .word Reset_Handler
    .word Default_Handler   /* NMI */
    .word Default_Handler   /* HardFault */
    .word Default_Handler   /* MemManage */
    .word Default_Handler   /* BusFault */
    .word Default_Handler   /* UsageFault */
    .word 0; .word 0; .word 0; .word 0  /* reserved */
    .word Default_Handler   /* SVCall */
    .word Default_Handler   /* DebugMon */
    .word 0                 /* reserved */
    .word Default_Handler   /* PendSV */
    .word Default_Handler   /* SysTick */
    .rept 48                /* IRQ[0..47] - more than enough for hello */
    .word Default_Handler
    .endr
    .size vectors, . - vectors

    .section .text, "ax", %progbits

    .thumb_func
    .global Reset_Handler
    .type Reset_Handler, %function
Reset_Handler:
    /* No .data init / .bss zeroing needed - main has no globals. */
    bl main
1:  wfi
    b 1b
    .size Reset_Handler, . - Reset_Handler

    .thumb_func
    .global Default_Handler
    .type Default_Handler, %function
Default_Handler:
2:  wfi
    b 2b
    .size Default_Handler, . - Default_Handler
