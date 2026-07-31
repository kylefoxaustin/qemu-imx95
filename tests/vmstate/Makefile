# CPU-state migration witness for the qemu-imx95 emulator.
#
# Produces counter.bin, a flat ARM64 image loaded at 0x80000000 (DDR base)
# that emits an incrementing letter over LPUART1 forever (the migration
# witness for tests/vmstate/run.sh). run.sh also builds it on demand.

CROSS   ?= aarch64-linux-gnu-
AS      := $(CROSS)as
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

ASFLAGS := -mcpu=cortex-a55

.PHONY: all clean
all: counter.bin

counter.o: counter.S
	$(AS) $(ASFLAGS) -o $@ $<

counter.elf: counter.o link.ld
	$(LD) -T link.ld -o $@ counter.o

counter.bin: counter.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f counter.o counter.elf counter.bin
