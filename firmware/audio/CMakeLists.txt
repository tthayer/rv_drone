add_executable(audio main.c link_validate.c link_spi.c)

# rvlink.h lives in the repo-level common/ dir (shared with the Nano image).
target_include_directories(audio PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../common)

pico_generate_pio_header(audio ${CMAKE_CURRENT_LIST_DIR}/audio_i2s.pio)

target_link_libraries(audio PRIVATE
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_vreg
    hardware_irq
    hardware_spi
    hardware_gpio
    hardware_resets
    rv_common)

# USB CDC console (also keeps the picotool reset interface), no UART.
pico_enable_stdio_usb(audio 1)
pico_enable_stdio_uart(audio 0)

pico_add_extra_outputs(audio)

# Warnings on our sources only, not the SDK.
target_compile_options(audio PRIVATE -Wall -Wextra)

pico_set_program_name(audio "rv_drone audio (Pico A)")
pico_set_program_description(audio "PIO I2S 48 kHz / 64 fs to PCM5102A")
