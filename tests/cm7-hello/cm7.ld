/*
 * Linker script for the Cortex-M7 hello firmware.
 *
 * Memory map matches the upstream Linux imx_rproc driver's
 * imx_rproc_att_imx95_m7 attribute table:
 *   ITCM  (M7-view)  0x00000000, 256 KiB  (rx code + vector table)
 *   DTCM  (M7-view)  0x20000000, 256 KiB  (stack + data + bss)
 *
 * Stack lives at the top of DTCM and grows down.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

MEMORY {
    ITCM (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
    DTCM (rwx) : ORIGIN = 0x20000000, LENGTH = 256K
}

_stack_top = ORIGIN(DTCM) + LENGTH(DTCM);

ENTRY(Reset_Handler)

SECTIONS {
    .vectors : {
        KEEP(*(.vectors))
    } > ITCM

    .text : {
        *(.text*)
        *(.rodata*)
    } > ITCM

    .data : {
        __data_start = .;
        *(.data*)
        __data_end = .;
    } > DTCM AT > ITCM
    __data_load = LOADADDR(.data);

    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    } > DTCM

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }
}
