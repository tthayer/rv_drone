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
           -MMD -MP -DBOARD_$(BOARD_UP) -Isrc/board -Isrc/hal -Isrc/boot -Isrc/drivers -Isrc/engine -Isrc/ui -Isrc/fs -Ithird_party/fatfs -Icommon -Isrc/libc $(EXTRA_CFLAGS)
ASFLAGS := -march=rv64gc -mabi=lp64d -mcmodel=medany -DBOARD_$(BOARD_UP) -Wall -Werror
LDFLAGS := -nostdlib -static -Wl,-T,src/boot/link.ld -Wl,-Map,$(NAME).map -Wl,--gc-sections -Wl,--no-warn-rwx-segments

SRCS := src/boot/start.S src/boot/trap.S src/hal/uart.c src/hal/trap.c \
       src/hal/timer.c src/hal/plic.c src/hal/reset.c src/hal/cache.c src/hal/dma.c src/app/main.c \
       src/engine/engine.c src/engine/osc.c src/engine/params.c src/ui/ui.c src/hal/vec.c
ifeq ($(BOARD),nano)
SRCS += src/hal/pinmux.c src/hal/gpio.c src/hal/spi.c src/hal/sd.c src/fs/diskio.c src/app/preset_fs.c
FATFS := third_party/fatfs/ff.c
endif
SRCS += src/drivers/audio_link.c src/drivers/panel_link.c src/app/panel_ui.c
CSRCS := common/fb.c common/font5x7.c
# RVV (XTheadVector) only for the files that use it; nowhere else gets vector code.
ifeq ($(BOARD),nano)
CFLAGS += -DENGINE_RVV
VEC_CFLAGS := -march=rv64gc_xtheadvector
$(BUILD)/engine/osc.c.o: CFLAGS += $(VEC_CFLAGS)
$(BUILD)/hal/vec.c.o: CFLAGS += $(VEC_CFLAGS)
endif
OBJS := $(patsubst src/%,$(BUILD)/%.o,$(SRCS)) $(patsubst common/%,$(BUILD)/common/%.o,$(CSRCS)) \
        $(patsubst third_party/%,$(BUILD)/third_party/%.o,$(FATFS))

all: $(NAME).elf $(NAME).bin $(NAME).lst

$(BUILD)/%.c.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/common/%.c.o: common/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
# FatFs (vendor, fetched by tools/get-vendor.sh): our flags minus -Wextra.
$(BUILD)/third_party/%.c.o: third_party/%.c src/fs/ffconf.h
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -Wextra,$(CFLAGS)) -c $< -o $@
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

# C906L parking image (replaces the vendor cvirtos.bin in fip.bin).
build/park.bin: src/park/park.S
	@mkdir -p build
	$(CC) -march=rv64gc -mabi=lp64d -nostdlib -Wl,-Ttext=0x83F40000 -Wl,--no-warn-rwx-segments $< -o build/park.elf
	$(OBJCOPY) -O binary build/park.elf $@

export RTOS_BIN ?= build/park.bin

# Vendor files (not in git): tools/get-vendor.sh fetches them into third_party/.
export FIPTOOL       ?= $(CURDIR)/third_party/fiptool
export FSBL_BIN      ?= $(FIPTOOL)/data/fsbl/cv181x.bin
export DDR_PARAM_BIN ?= $(FIPTOOL)/data/ddr_param.bin
USB_DL_MAGIC         ?= $(CURDIR)/third_party/usb_dl/cv_dl_magic.bin

# OPENSBI_BIN defaults to our own build; override from the environment to use another.
export OPENSBI_BIN ?= build/opensbi/fw_dynamic.bin

fip: $(if $(filter command line environment,$(origin OPENSBI_BIN)),,opensbi) build/park.bin
	$(MAKE) BOARD=nano
	tools/mkfip.sh

# Boot fip.bin over USB (Nano without a bootable SD). RESET_PORT, if set, is the
# UART0 serial device: Ctrl-R is sent there first so a running image resets
# into the ROM's USB download mode. USB_DL_MAGIC: vendor cv_dl_magic.bin.
PYTHON ?= $(if $(wildcard .venv/bin/python),.venv/bin/python,python3)
usbboot: fip
	@[ -n "$(USB_DL_MAGIC)" ] || { echo "set USB_DL_MAGIC (vendor cv_dl_magic.bin)"; exit 1; }
	$(if $(RESET_PORT),stty -f $(RESET_PORT) 115200 raw -echo clocal && printf '\022' > $(RESET_PORT))
	$(PYTHON) tools/usbboot.py build/nano/fip.bin --magic $(USB_DL_MAGIC)

# Pico firmware (Pico A audio, Pico B panel). Needs:
#   tools/get-pico-sdk.sh, arm-none-eabi-gcc with newlib, ninja, cmake, picotool.
# PICO_TOOLCHAIN_PATH is a bin dir; defaults to a user-local Arm GNU toolchain
# (~/opt/arm-gnu-toolchain-*) if one is installed, else PATH is used.
PICO_SDK_PATH ?= $(CURDIR)/third_party/pico-sdk
PICO_TOOLCHAIN_PATH ?= $(lastword $(wildcard $(HOME)/opt/arm-gnu-toolchain-*/bin))
pico:
	cmake -S firmware -B build/firmware -G Ninja -DPICO_SDK_PATH=$(PICO_SDK_PATH) \
	      $(if $(PICO_TOOLCHAIN_PATH),-DPICO_TOOLCHAIN_PATH=$(PICO_TOOLCHAIN_PATH))
	ninja -C build/firmware

# Reboots a running Pico (picotool reset interface) and loads the UF2. With two
# Picos on USB, select by USB serial: PICO_A_SER / PICO_B_SER (README, `picotool
# info -a`); unset, picotool takes the only device it can find.
pico-flash-audio: pico
	picotool load $(if $(PICO_A_SER),--ser $(PICO_A_SER)) -fx build/firmware/audio/audio.uf2

pico-flash-panel: pico
	picotool load $(if $(PICO_B_SER),--ser $(PICO_B_SER)) -fx build/firmware/panel/panel.uf2

# Host tests for the panel logic (plain cc); also writes build/panel-test/*.pbm,
# the OLED layouts rendered by the firmware's own render.c.
test-panel:
	@mkdir -p build/panel-test
	cc -std=c11 -Wall -Wextra -Werror -Ifirmware/panel firmware/panel/test/test_midi.c firmware/panel/midi_parser.c -o build/panel-test/test_midi
	cc -std=c11 -Wall -Wextra -Werror -Ifirmware/panel firmware/panel/test/test_debounce.c -o build/panel-test/test_debounce
	cc -std=c11 -Wall -Wextra -Werror -Ifirmware/panel -Icommon firmware/panel/test/test_render.c firmware/panel/render.c common/fb.c common/font5x7.c -o build/panel-test/test_render
	cc -std=c11 -Wall -Wextra -Werror -Icommon firmware/panel/test/test_rvpanel.c -o build/panel-test/test_rvpanel
	build/panel-test/test_midi && build/panel-test/test_debounce && build/panel-test/test_render build/panel-test && build/panel-test/test_rvpanel

# Host test for the Pico A rvlink validator (plain cc).
test-link:
	@mkdir -p build/link-test
	cc -std=c11 -Wall -Wextra -Werror -Icommon -Ifirmware/audio firmware/audio/test/test_link.c firmware/audio/link_validate.c -o build/link-test/test_link
	build/link-test/test_link

# Host emulator: engine + UI on macOS with SDL2 (window + audio, or --wav offline).
EMU_SRCS := emu/main.c src/engine/engine.c src/engine/osc.c src/engine/params.c src/ui/ui.c common/fb.c common/font5x7.c
emu: build/emu/rv_drone_emu
build/emu/rv_drone_emu: $(EMU_SRCS) $(wildcard src/engine/*.h src/ui/*.h common/*.h)
	@mkdir -p build/emu
	cc -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/engine -Isrc/ui -Icommon $(shell sdl2-config --cflags) \
	   $(EMU_SRCS) $(shell sdl2-config --libs) -lm -o $@

# Wiring diagrams (WireViz + Graphviz): docs/wiring/*.yml -> .svg/.png.
#   .venv/bin/pip install wireviz; brew install graphviz
wiring:
	.venv/bin/wireviz -f sp docs/wiring/system.yml
	.venv/bin/wireviz -f sp docs/wiring/panel.yml

# Host test: preset save/load round trip through ui.c with an in-memory store.
test-presets:
	@mkdir -p build/emu
	cc -std=gnu11 -O2 -Wall -Wextra -Isrc/engine -Isrc/ui -Icommon emu/test_presets.c \
	   src/engine/engine.c src/engine/osc.c src/engine/params.c src/ui/ui.c common/fb.c common/font5x7.c -o build/emu/test_presets
	build/emu/test_presets

# Offline render (20 s, default drone) + level/sanity stats.
emu-wav: emu
	build/emu/rv_drone_emu --wav build/emu/drone.wav --seconds 20

clean:
	rm -rf build

.PHONY: emu emu-wav test-presets wiring all run-qemu opensbi fip usbboot pico pico-flash-audio pico-flash-panel test-panel test-link clean

# Header dependencies (after all rules so 'all' stays the default goal).
-include $(OBJS:.o=.d)
