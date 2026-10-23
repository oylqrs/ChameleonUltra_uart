#include "uart_esp.h"

#include "dataframe.h"
#include "netdata.h"
#include "app_error.h"
#include "nrf_gpio.h"
#include "nrf_drv_uart.h"
#include <string.h>

#define UART_ESP_TX_PIN NRF_GPIO_PIN_MAP(0, 6)
#define UART_ESP_RX_PIN NRF_GPIO_PIN_MAP(0, 8)
#define UART_ESP_BAUDRATE NRF_UART_BAUDRATE_115200
#define UART_ESP_RX_RING_SIZE 8192U
#define UART_ESP_RX_RING_MASK (UART_ESP_RX_RING_SIZE - 1U)

static nrf_drv_uart_t m_uart = NRF_DRV_UART_INSTANCE(1);
static uint8_t m_rx_byte;
static uint8_t m_rx_ring[UART_ESP_RX_RING_SIZE];
static uint8_t m_tx_buffer[sizeof(netdata_frame_raw_t)];
static volatile uint16_t m_rx_head;
static volatile uint16_t m_rx_tail;
static volatile bool m_rx_error;
static volatile bool m_rx_rearm_pending;
static volatile bool m_initialized;
static volatile bool m_tx_busy;
static volatile bool m_wakeup_from_system_off;

static void uart_esp_rx_enqueue(uint8_t byte) {
    uint16_t next = (uint16_t)((m_rx_head + 1U) & UART_ESP_RX_RING_MASK);
    if (next == m_rx_tail) {
        m_rx_error = true;
        return;
    }

    m_rx_ring[m_rx_head] = byte;
    m_rx_head = next;
}

static ret_code_t uart_esp_arm_rx(void) {
    m_rx_rearm_pending = false;
    ret_code_t result = nrf_drv_uart_rx(&m_uart, &m_rx_byte, 1);
    if (result != NRF_SUCCESS) {
        m_rx_rearm_pending = true;
    }
    return result;
}

static void uart_esp_process_rx(void) {
    if (m_rx_error) {
        m_rx_error = false;
        m_rx_tail = m_rx_head;
        data_frame_reset_source(DATA_FRAME_SOURCE_UART);
    }
    if (m_rx_rearm_pending) {
        (void)uart_esp_arm_rx();
    }

    /* Keep command processing half-duplex so every parsed request has a TX
     * buffer available for its response. Bytes can continue accumulating in
     * the hardware-independent ring while EasyDMA is transmitting. */
    if (m_tx_busy) {
        return;
    }

    while (m_rx_tail != m_rx_head &&
           !data_frame_source_ready(DATA_FRAME_SOURCE_UART)) {
        uint8_t byte = m_rx_ring[m_rx_tail];
        m_rx_tail = (uint16_t)((m_rx_tail + 1U) & UART_ESP_RX_RING_MASK);
        data_frame_receive_from(DATA_FRAME_SOURCE_UART, &byte, 1);
    }
}

static void uart_esp_evt_handler(nrf_drv_uart_event_t *event, void *context) {
    (void)context;
    if (event->type == NRF_DRV_UART_EVT_RX_DONE) {
        for (size_t i = 0; i < event->data.rxtx.bytes; i++) {
            uart_esp_rx_enqueue(event->data.rxtx.p_data[i]);
        }
        (void)uart_esp_arm_rx();
    } else if (event->type == NRF_DRV_UART_EVT_TX_DONE) {
        m_tx_busy = false;
    } else if (event->type == NRF_DRV_UART_EVT_ERROR) {
        m_rx_error = true;
        (void)uart_esp_arm_rx();
    }
}

void uart_esp_init(void) {
    /* The wake latch is only valid until the RX pin is reconfigured by UARTE. */
    m_wakeup_from_system_off = nrf_gpio_pin_latch_get(UART_ESP_RX_PIN) != 0;
    nrf_gpio_pin_latch_clear(UART_ESP_RX_PIN);

    nrf_drv_uart_config_t config = NRF_DRV_UART_DEFAULT_CONFIG;
    config.pseltxd = UART_ESP_TX_PIN;
    config.pselrxd = UART_ESP_RX_PIN;
    config.pselrts = NRF_UART_PSEL_DISCONNECTED;
    config.pselcts = NRF_UART_PSEL_DISCONNECTED;
    config.hwfc = NRF_UART_HWFC_DISABLED;
    config.baudrate = UART_ESP_BAUDRATE;

    APP_ERROR_CHECK(nrf_drv_uart_init(&m_uart, &config, uart_esp_evt_handler));
    APP_ERROR_CHECK(uart_esp_arm_rx());
    data_frame_register_source_pump(DATA_FRAME_SOURCE_UART, uart_esp_process_rx);
    m_initialized = true;
}

void uart_esp_prepare_system_off(void) {
    if (m_initialized) {
        nrf_drv_uart_uninit(&m_uart);
        m_initialized = false;
    }
    m_rx_rearm_pending = false;
    m_tx_busy = false;

    /* UART idles high; the first low start bit wakes the nRF52840. */
    nrf_gpio_cfg_sense_input(UART_ESP_RX_PIN, NRF_GPIO_PIN_PULLUP,
                             NRF_GPIO_PIN_SENSE_LOW);
}

bool uart_esp_was_wakeup(void) {
    bool was_wakeup = m_wakeup_from_system_off;
    m_wakeup_from_system_off = false;
    return was_wakeup;
}

bool uart_esp_is_working(void) {
    return m_initialized;
}

uart_esp_write_result_t uart_esp_write(const void *data, uint16_t length) {
    if (!m_initialized || data == NULL || length == 0 || length > sizeof(m_tx_buffer)) {
        return UART_ESP_WRITE_INVALID_ARGUMENT;
    }
    if (m_tx_busy) {
        return UART_ESP_WRITE_BUSY;
    }

    memcpy(m_tx_buffer, data, length);
    m_tx_busy = true;
    ret_code_t ret = nrfx_uarte_tx(&m_uart.uarte, m_tx_buffer, length);
    if (ret != NRF_SUCCESS) {
        m_tx_busy = false;
        return ret == NRF_ERROR_BUSY ? UART_ESP_WRITE_BUSY : UART_ESP_WRITE_ERROR;
    }
    return UART_ESP_WRITE_OK;
}
