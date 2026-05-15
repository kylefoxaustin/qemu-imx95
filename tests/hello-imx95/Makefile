# Hello-world bare-metal binary for the qemu-imx95 emulator.
#
# Produces hello.bin, a flat ARM64 image loaded at 0x80000000 (DDR base)
# that prints over LPUART1 and halts.

CROSS   ?= aarch64-linux-gnu-
AS      := $(CROSS)as
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

ASFLAGS := -mcpu=cortex-a55

.PHONY: all clean
all: hello.bin

hello.o: hello.S
	$(AS) $(ASFLAGS) -o $@ $<

hello.elf: hello.o link.ld
	$(LD) -T link.ld -o $@ hello.o

hello.bin: hello.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f hello.o hello.elf hello.bin
