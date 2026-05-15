# tests/hello-imx95

Bare-metal ARM64 binary that prints "Hello from i.MX 95!" through
LPUART1 on the `qemu-imx95` emulator, then halts in WFI. Used as the
v0.0.2 end-to-end smoke test: real ARM instructions running on the
emulated A55 cluster, real LPUART register accesses going through the
new device model, real characters flowing through QEMU's chardev
backend to the host terminal.

## Build

Requires the GNU binutils aarch64-linux-gnu cross package (no compiler
needed — just `as`, `ld`, `objcopy`):

    sudo apt install -y binutils-aarch64-linux-gnu
    make

Produces `hello.bin`, a flat binary linked for execution at
`0x80000000` (the DDR base address per the imx95 memory map).

## Run

From the QEMU build directory:

    ./qemu-system-aarch64 \
        -M imx95-19x19-evk \
        -nographic \
        -m 2G \
        -kernel ../tests/hello-imx95/hello.bin

Expected output:

    Hello from i.MX 95!

The CPU then idles in WFI. Quit with `Ctrl-A x`.

## How it works

The binary is loaded at `0x80000000` (DDR base). QEMU's
`arm_load_kernel()` boot stub starts CPU 0 there with the MMU off, so
the assembly can hit MMIO addresses directly:

  - Writes `CTRL.TE` (bit 0x00080000 at LPUART1 + 0x18) to enable TX
  - For each byte of the message:
    - Polls `STAT.TDRE` (bit 0x00800000 at LPUART1 + 0x14) until set
    - Writes the byte to `DATA` (LPUART1 + 0x1c)
  - Halts with WFI when the null terminator is hit

LPUART1's base of `0x44380000` and register offsets come from the i.MX
95 BSP device tree (`arch/arm64/boot/dts/freescale/imx95.dtsi`,
`serial@44380000`).
