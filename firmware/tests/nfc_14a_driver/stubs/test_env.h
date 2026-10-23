#ifndef TEST_NFC_14A_DRIVER_ENV_H
#define TEST_NFC_14A_DRIVER_ENV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "nfc_14a.h"

#define NRF52840_XXAA 1

#define NRF_LOG_INFO(...)  do { } while (0)
#define NRF_LOG_ERROR(...) do { } while (0)
#define NRF_LOG_DEBUG(...) do { } while (0)
#define NRF_LOG_WARNING(...) do { } while (0)

void test_assert_failed(void);
#define ASSERT(condition) do { if (!(condition)) test_assert_failed(); } while (0)

#define RGB_GREEN 1U
#define SLEEP_DELAY_MS_FIELD_NFC_LOST 1000U
#define TAG_FIELD_LED_ON()  do { } while (0)
#define TAG_FIELD_LED_OFF() do { } while (0)
#define CRITICAL_REGION_ENTER() do { } while (0)
#define CRITICAL_REGION_EXIT()  do { } while (0)

void set_slot_light_color(uint32_t color);
void sleep_timer_stop(void);
void sleep_timer_start(uint32_t milliseconds);

void calc_14a_crc_lut(uint8_t *data, int length, uint8_t *crc);

typedef struct {
    volatile uint32_t FRAMECONFIG;
    volatile uint32_t AMOUNT;
} test_nfct_data_registers_t;

typedef struct {
    volatile uint32_t TASKS_ACTIVATE;       /* 0x000 */
    volatile uint32_t TASKS_DISABLE;        /* 0x004 */
    volatile uint32_t TASKS_SENSE;          /* 0x008 */
    volatile uint32_t TASKS_STARTTX;        /* 0x00c */
    volatile uint32_t RESERVED0[3];         /* 0x010 */
    volatile uint32_t TASKS_ENABLERXDATA;   /* 0x01c */
    uint8_t RESERVED1[0x304U - 0x020U];
    volatile uint32_t INTENSET;             /* 0x304 */
    uint8_t RESERVED2[0x50cU - 0x308U];
    volatile uint32_t FRAMEDELAYMODE;       /* 0x50c */
    volatile uint32_t PACKETPTR;            /* 0x510 */
    volatile uint32_t MAXLEN;               /* 0x514 */
    test_nfct_data_registers_t TXD;         /* 0x518 */
    test_nfct_data_registers_t RXD;         /* 0x520 */
} test_nfct_registers_t;

#define NRF_NFCT ((test_nfct_registers_t *)(uintptr_t)0x40005000U)
#define TEST_NFCT_STOPTX_TASK (*(volatile uint32_t *)(uintptr_t)0x40005010U)

#define NFCT_MAXLEN_MAXLEN_Pos 0U
#define NFCT_MAXLEN_MAXLEN_Msk 0x1ffU
#define NFCT_TXD_AMOUNT_TXDATABITS_Pos 0U
#define NFCT_TXD_AMOUNT_TXDATABITS_Msk 0x7U
#define NFCT_TXD_AMOUNT_TXDATABYTES_Pos 3U
#define NFCT_TXD_AMOUNT_TXDATABYTES_Msk (0x1ffU << 3U)
#define NFCT_RXD_AMOUNT_RXDATABITS_Pos 0U
#define NFCT_RXD_AMOUNT_RXDATABITS_Msk 0x7U
#define NFCT_RXD_AMOUNT_RXDATABYTES_Pos 3U
#define NFCT_RXD_AMOUNT_RXDATABYTES_Msk (0x1ffU << 3U)

#define NFCT_TXD_FRAMECONFIG_PARITY_Msk      (1U << 0U)
#define NFCT_TXD_FRAMECONFIG_DISCARDMODE_Msk (1U << 1U)
#define NFCT_TXD_FRAMECONFIG_SOF_Msk         (1U << 2U)
#define NFCT_TXD_FRAMECONFIG_CRCMODETX_Msk   (1U << 4U)

#define NRF_NFCT_INT_FIELDDETECTED_MASK (1U << 0U)
#define NRF_NFCT_INT_FIELDLOST_MASK     (1U << 1U)
#define NRF_NFCT_INT_SELECTED_MASK      (1U << 2U)
#define NRF_NFCT_INT_RXFRAMESTART_MASK  (1U << 3U)
#define NRF_NFCT_INT_RXFRAMEEND_MASK    (1U << 4U)
#define NRF_NFCT_INT_RXERROR_MASK       (1U << 5U)
#define NRF_NFCT_INT_TXFRAMESTART_MASK  (1U << 6U)
#define NRF_NFCT_INT_TXFRAMEEND_MASK    (1U << 7U)

typedef enum {
    NRF_NFCT_FRAME_DELAY_MODE_FREERUN = 0,
    NRF_NFCT_FRAME_DELAY_MODE_WINDOW = 1,
    NRF_NFCT_FRAME_DELAY_MODE_EXACTVAL = 2,
    NRF_NFCT_FRAME_DELAY_MODE_WINDOWGRID = 3,
} nrf_nfct_frame_delay_mode_t;

typedef enum {
    NRF_NFCT_SENSRES_BIT_FRAME_SDD_00001 = 0,
} nrf_nfct_sensres_bit_frame_sdd_t;

uint32_t nrf_nfct_frame_delay_max_get(void);
void nrf_nfct_frame_delay_max_set(uint32_t value);
void nrf_nfct_frame_delay_mode_set(nrf_nfct_frame_delay_mode_t mode);
void nrf_nfct_sensres_bit_frame_sdd_set(nrf_nfct_sensres_bit_frame_sdd_t value);
uint32_t nrf_nfct_int_enable_get(void);
void nrf_nfct_int_enable(uint32_t mask);
void nrf_nfct_int_disable(uint32_t mask);

typedef enum {
    NRFX_NFCT_EVT_FIELD_DETECTED = 1,
    NRFX_NFCT_EVT_FIELD_LOST,
    NRFX_NFCT_EVT_SELECTED,
    NRFX_NFCT_EVT_RX_FRAMESTART,
    NRFX_NFCT_EVT_RX_FRAMEEND,
    NRFX_NFCT_EVT_TX_FRAMESTART,
    NRFX_NFCT_EVT_TX_FRAMEEND,
    NRFX_NFCT_EVT_ERROR,
} nrfx_nfct_evt_id_t;

typedef enum {
    NRFX_NFCT_ERROR_FRAMEDELAYTIMEOUT = 0,
    NRFX_NFCT_ERROR_NUM,
} nrfx_nfct_error_t;

typedef struct {
    uint32_t data_size;
    const uint8_t *p_data;
} nrfx_nfct_data_desc_t;

typedef struct {
    uint32_t rx_status;
    nrfx_nfct_data_desc_t rx_data;
} nrfx_nfct_evt_rx_frameend_t;

typedef struct {
    nrfx_nfct_data_desc_t tx_data;
} nrfx_nfct_evt_tx_framestart_t;

typedef struct {
    nrfx_nfct_error_t reason;
} nrfx_nfct_evt_error_t;

typedef struct {
    nrfx_nfct_evt_id_t evt_id;
    union {
        nrfx_nfct_evt_rx_frameend_t rx_frameend;
        nrfx_nfct_evt_tx_framestart_t tx_framestart;
        nrfx_nfct_evt_error_t error;
    } params;
} nrfx_nfct_evt_t;

typedef void (*nrfx_nfct_handler_t)(const nrfx_nfct_evt_t *event);

typedef struct {
    uint32_t rxtx_int_mask;
    nrfx_nfct_handler_t cb;
} nrfx_nfct_config_t;

typedef enum {
    NRFX_NFCT_STATE_SENSING = 0,
    NRFX_NFCT_STATE_ACTIVATED = 1,
} nrfx_nfct_state_t;

typedef int nrfx_err_t;
#define NRFX_SUCCESS 0

nrfx_err_t nrfx_nfct_init(const nrfx_nfct_config_t *config);
void nrfx_nfct_uninit(void);
void nrfx_nfct_enable(void);
void nrfx_nfct_disable(void);
void nrfx_nfct_autocolres_disable(void);
void nrfx_nfct_state_force(nrfx_nfct_state_t state);

typedef enum {
    NRF_DRV_CLOCK_EVT_HFCLK_STARTED,
    NRF_DRV_CLOCK_EVT_LFCLK_STARTED,
    NRF_DRV_CLOCK_EVT_CAL_DONE,
    NRF_DRV_CLOCK_EVT_CAL_ABORTED,
} nrf_drv_clock_evt_type_t;

typedef uint32_t ret_code_t;
#define NRF_SUCCESS 0U
#define NRF_ERROR_MODULE_ALREADY_INITIALIZED 1U

typedef void (*nrf_drv_clock_event_handler_t)(nrf_drv_clock_evt_type_t event);

typedef struct nrf_drv_clock_handler_item_s nrf_drv_clock_handler_item_t;
struct nrf_drv_clock_handler_item_s {
    nrf_drv_clock_handler_item_t *p_next;
    nrf_drv_clock_event_handler_t event_handler;
};

void nrf_drv_clock_hfclk_request(nrf_drv_clock_handler_item_t *handler_item);
void nrf_drv_clock_hfclk_release(void);
bool nrf_drv_clock_hfclk_is_running(void);
bool nrf_drv_clock_init_check(void);
ret_code_t nrf_drv_clock_init(void);

#endif
