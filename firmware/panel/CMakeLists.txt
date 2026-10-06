add_executable(panel
    main.c
    encoders.c
    ${CMAKE_CURRENT_LIST_DIR}/../../common/fb.c
    ${CMAKE_CURRENT_LIST_DIR}/../../common/font5x7.c
    midi_parser.c
    nano_link.c
    render.c
    ssd1306.c)

# fb/font (shared with the Nano UI) and rvpanel.h live in the repo-level common/.
target_include_directories(panel PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../common)

pico_generate_pio_header(panel ${CMAKE_CURRENT_LIST_DIR}/quadrature.pio)

target_link_libraries(panel PRIVATE
    pico_stdlib
    hardware_pio
    hardware_i2c
    hardware_uart
    hardware_gpio
    hardware_irq
    hardware_clocks
    rv_common)

# USB CDC console (also keeps the picotool reset interface), no UART.
pico_enable_stdio_usb(panel 1)
pico_enable_stdio_uart(panel 0)

pico_add_extra_outputs(panel)

# Warnings on our sources only, not the SDK.
target_compile_options(panel PRIVATE -Wall -Wextra)

pico_set_program_name(panel "rv_drone panel (Pico B)")
pico_set_program_description(panel "Encoders, switches, 3 OLEDs and MIDI in")
