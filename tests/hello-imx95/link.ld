/*
 * Linker script for the hello-imx95 bare-metal binary.
 *
 * QEMU's arm_load_kernel() loads a flat -kernel image at loader_start,
 * which for imx95-19x19-evk is FSL_IMX95_RAM_START = 0x80000000, and
 * starts CPU 0 executing there.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x80000000;

    .text : {
        *(.text.startup)
        *(.text*)
    }

    .rodata : ALIGN(8) {
        *(.rodata*)
    }

    .data : ALIGN(8) {
        *(.data*)
    }

    .bss : ALIGN(8) {
        *(.bss*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.note*)
        *(.comment*)
        *(.eh_frame*)
    }
}
