BOARD ?= nano
ifeq ($(filter $(BOARD),nano qemu),)
$(error BOARD must be nano or qemu)
endif

CROSS   ?= riscv64-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

BUILD := build/$(BOARD)
NAME  := $(BUILD)/rv_drone
BOARD_UP := $(shell echo $(BOARD) | tr a-z A-Z)

CFLAGS  := -std=c11 -march=rv64gc -mabi=lp64d -mcmodel=medany -ffreestanding \
           -nostdlib -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
           -DBOARD_$(BOARD_UP) -Isrc/board -Isrc/hal -Isrc/boot $(EXTRA_CFLAGS)
ASFLAGS := -march=rv64gc -mabi=lp64d -mcmodel=medany -DBOARD_$(BOARD_UP) -Wall -Werror
LDFLAGS := -nostdlib -static -Wl,-T,src/boot/link.ld -Wl,-Map,$(NAME).map -Wl,--gc-sections -Wl,--no-warn-rwx-segments

SRCS := src/boot/start.S src/boot/trap.S src/hal/uart.c src/hal/trap.c \
       src/hal/timer.c src/hal/plic.c src/app/main.c
OBJS := $(patsubst src/%,$(BUILD)/%.o,$(SRCS))

all: $(NAME).elf $(NAME).bin $(NAME).lst

$(BUILD)/%.c.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.S.o: src/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(NAME).elf: $(OBJS) src/boot/link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@
$(NAME).bin: $(NAME).elf
	$(OBJCOPY) -O binary $< $@
$(NAME).lst: $(NAME).elf
	$(OBJDUMP) -d -S $< > $@

run-qemu:
	$(MAKE) BOARD=qemu
	qemu-system-riscv64 -M virt -m 256M -nographic -bios default -kernel build/qemu/rv_drone.bin

opensbi:
	tools/build-opensbi.sh

# OPENSBI_BIN defaults to our own build; override from the environment to use another.
export OPENSBI_BIN ?= build/opensbi/fw_dynamic.bin

fip: $(if $(filter command line environment,$(origin OPENSBI_BIN)),,opensbi)
	$(MAKE) BOARD=nano
	tools/mkfip.sh

clean:
	rm -rf build

.PHONY: all run-qemu opensbi fip clean
