# Minimal Cortex-M7 firmware for the i.MX 95 M7 (v1.x bring-up).
#
# Produces cm7_image.elf, loadable onto the M7 instance with
#   -device loader,file=cm7_image.elf,cpu-num=7
# (the cpu-num is provisional; the v1.x machine-side patch wires up the
# M7 as CPU#7 in the heterogeneous topology, after the 6 A55s + M33).
#
# Needs arm-none-eabi-gcc. Override TOOLCHAIN if installed elsewhere:
#   make TOOLCHAIN=/opt/.../bin/arm-none-eabi-

TOOLCHAIN ?= $(HOME)/tools/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-

CC      := $(TOOLCHAIN)gcc
OBJCOPY := $(TOOLCHAIN)objcopy
READELF := $(TOOLCHAIN)readelf

CPU_FLAGS := -mcpu=cortex-m7 -mthumb -mfloat-abi=soft
CFLAGS    := $(CPU_FLAGS) -ffreestanding -nostdlib -nostartfiles \
             -Wall -Wextra -O2 -g
LDFLAGS   := $(CPU_FLAGS) -T cm7.ld -nostdlib -nostartfiles \
             -Wl,--gc-sections

OBJS := startup.o main.o

.PHONY: all info clean

all: cm7_image.elf cm7_image.bin cm7_fault.elf cm7_fault.bin

cm7_image.elf: $(OBJS) cm7.ld
	$(CC) $(LDFLAGS) -Wl,-Map=cm7_image.map -o $@ $(OBJS)

cm7_image.bin: cm7_image.elf
	$(OBJCOPY) -O binary $< $@

# Fault-recovery fixture (v1.x Step 5): self-faults via SYSRESETREQ on the
# first boot so the SM cold-resets the M7 LM; bumps a DTCM boot counter so
# the A55-side test can confirm the cycle happened.
cm7_fault.elf: startup.o main_fault.o cm7.ld
	$(CC) $(LDFLAGS) -Wl,-Map=cm7_fault.map -o $@ startup.o main_fault.o

cm7_fault.bin: cm7_fault.elf
	$(OBJCOPY) -O binary $< $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

info: cm7_image.elf
	@echo "=== ELF header (machine + entry) ==="
	@$(READELF) -h $< | grep -E "Machine|Entry|Type|Class"
	@echo "=== sections (vectors / text / data / bss) ==="
	@$(READELF) -S $< | grep -E "Name|\.vectors|\.text|\.data|\.bss"

clean:
	rm -f *.o cm7_image.elf cm7_image.bin cm7_image.map \
	      cm7_fault.elf cm7_fault.bin cm7_fault.map
