#include "nano_link.h"

#include "hardware/gpio.h"
#include "hardware/uart.h"

#define NANO_UART   uart0
#define PIN_TX      16
#define PIN_RX      17

static uint32_t rx_bytes;

void nano_link_init(void) {
    uart_init(NANO_UART, NANO_LINK_BAUD);
    gpio_set_function(PIN_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_RX, GPIO_FUNC_UART);
    gpio_pull_up(PIN_RX);       // keep an unconnected RX idle-high
}

void nano_link_poll(void) {
    while (uart_is_readable(NANO_UART)) {
        (void)uart_getc(NANO_UART);     // M6: feed the COBS decoder
        rx_bytes++;
    }
}

uint32_t nano_link_rx_bytes(void) {
    return rx_bytes;
}
