#ifndef UART_ESP_H
#define UART_ESP_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UART_ESP_WRITE_OK = 0,
    UART_ESP_WRITE_BUSY,
    UART_ESP_WRITE_INVALID_ARGUMENT,
    UART_ESP_WRITE_ERROR,
} uart_esp_write_result_t;

void uart_esp_init(void);
/* Prepare the UART RX pin as a System OFF wake source. */
void uart_esp_prepare_system_off(void);
/* True when the last reset was caused by the UART RX wake signal. */
bool uart_esp_was_wakeup(void);
bool uart_esp_is_working(void);
uart_esp_write_result_t uart_esp_write(const void *data, uint16_t length);

#endif
