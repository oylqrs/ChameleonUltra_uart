#include "test_env.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

volatile unsigned nfc_14a_test_case;

bool g_is_tag_emulating;
bool g_usb_led_marquee_enable;

extern nfc_tag_14a_state_t m_tag_state_14a;

static nrfx_nfct_handler_t driver_callback;
static bool driver_enabled;
static bool driver_field_on;
static unsigned driver_init_count;
static unsigned driver_uninit_count;
static unsigned driver_enable_count;
static unsigned driver_disable_count;
static unsigned autocolres_disable_count;
static unsigned activate_count;

static nrf_drv_clock_handler_item_t *clock_handler_item;
static bool clock_driver_initialized;
static bool clock_running;
static unsigned clock_outstanding;
static unsigned clock_request_count;
static unsigned clock_release_count;

static uint32_t frame_delay_max;
static uint32_t interrupt_enable_mask;
static unsigned assertion_failures;
static unsigned reset_count;
static unsigned state_count;
static unsigned field_on_count;
static unsigned field_off_count;
static unsigned sleep_stop_count;
static unsigned sleep_start_count;

static nfc_tag_14a_uid_size uid_size = NFC_TAG_14A_UID_SINGLE_SIZE;
static uint8_t atqa[] = {0x04U, 0x00U};
static uint8_t sak[] = {0x20U};
static uint8_t uid[] = {0x12U, 0x34U, 0x56U, 0x78U};
static nfc_14a_ats_t ats = {{0}, 0};
static nfc_tag_14a_coll_res_reference_t collision_resources = {
    .size = &uid_size,
    .atqa = atqa,
    .sak = sak,
    .uid = uid,
    .ats = &ats,
};

void test_assert_failed(void) {
    assertion_failures++;
}

void set_slot_light_color(uint32_t color) {
    (void)color;
}

void sleep_timer_stop(void) {
    sleep_stop_count++;
}

void sleep_timer_start(uint32_t milliseconds) {
    (void)milliseconds;
    sleep_start_count++;
}

void calc_14a_crc_lut(uint8_t *data, int length, uint8_t *crc) {
    uint16_t value = 0x6363U;
    for (int index = 0; index < length; index++) {
        uint8_t byte = data[index] ^ (uint8_t)value;
        byte ^= (uint8_t)(byte << 4U);
        value = (uint16_t)((value >> 8U) ^ ((uint16_t)byte << 8U) ^
                           ((uint16_t)byte << 3U) ^ ((uint16_t)byte >> 4U));
    }
    crc[0] = (uint8_t)value;
    crc[1] = (uint8_t)(value >> 8U);
}

uint32_t nrf_nfct_frame_delay_max_get(void) {
    return frame_delay_max;
}

void nrf_nfct_frame_delay_max_set(uint32_t value) {
    frame_delay_max = value;
}

void nrf_nfct_frame_delay_mode_set(nrf_nfct_frame_delay_mode_t mode) {
    NRF_NFCT->FRAMEDELAYMODE = (uint32_t)mode;
}

void nrf_nfct_sensres_bit_frame_sdd_set(nrf_nfct_sensres_bit_frame_sdd_t value) {
    (void)value;
}

uint32_t nrf_nfct_int_enable_get(void) {
    return interrupt_enable_mask;
}

void nrf_nfct_int_enable(uint32_t mask) {
    interrupt_enable_mask |= mask;
}

void nrf_nfct_int_disable(uint32_t mask) {
    interrupt_enable_mask &= ~mask;
}

nrfx_err_t nrfx_nfct_init(const nrfx_nfct_config_t *config) {
    driver_callback = config->cb;
    driver_field_on = false;
    driver_init_count++;
    return NRFX_SUCCESS;
}

void nrfx_nfct_uninit(void) {
    driver_enabled = false;
    driver_field_on = false;
    driver_callback = NULL;
    driver_uninit_count++;
}

void nrfx_nfct_enable(void) {
    driver_enabled = true;
    driver_enable_count++;
}

void nrfx_nfct_disable(void) {
    driver_enabled = false;
    driver_disable_count++;
}

void nrfx_nfct_autocolres_disable(void) {
    autocolres_disable_count++;
}

void nrfx_nfct_state_force(nrfx_nfct_state_t state) {
    if (state == NRFX_NFCT_STATE_ACTIVATED) {
        activate_count++;
    }
}

void nrf_drv_clock_hfclk_request(nrf_drv_clock_handler_item_t *handler_item) {
    clock_request_count++;
    clock_handler_item = handler_item;
    if (clock_running && handler_item != NULL) {
        clock_handler_item = NULL;
        handler_item->event_handler(NRF_DRV_CLOCK_EVT_HFCLK_STARTED);
    }
    /* The real legacy driver invokes an already-running callback before it
     * increments its request count. */
    clock_outstanding++;
}

void nrf_drv_clock_hfclk_release(void) {
    clock_release_count++;
    if (clock_outstanding == 0U) {
        assertion_failures++;
        return;
    }
    clock_outstanding--;
    if (clock_outstanding == 0U) {
        clock_running = false;
    }
}

bool nrf_drv_clock_hfclk_is_running(void) {
    return clock_running;
}

bool nrf_drv_clock_init_check(void) {
    return clock_driver_initialized;
}

ret_code_t nrf_drv_clock_init(void) {
    clock_driver_initialized = true;
    return NRF_SUCCESS;
}

static void reset_callback(void) {
    reset_count++;
}

static void state_callback(uint8_t *data, uint16_t bits) {
    (void)data;
    (void)bits;
    state_count++;
}

static void field_on_callback(void) {
    field_on_count++;
}

static void field_off_callback(void) {
    field_off_count++;
}

static void rats_callback(uint16_t fsd, uint8_t cid) {
    (void)fsd;
    (void)cid;
}

static nfc_tag_14a_coll_res_reference_t *get_collision_resources(void) {
    return &collision_resources;
}

static void emit_field_detected(void) {
    if (!driver_enabled || driver_field_on || driver_callback == NULL) {
        return;
    }
    driver_field_on = true;
    const nrfx_nfct_evt_t event = {.evt_id = NRFX_NFCT_EVT_FIELD_DETECTED};
    driver_callback(&event);
}

static void emit_field_lost(void) {
    if (!driver_enabled || !driver_field_on || driver_callback == NULL) {
        return;
    }
    driver_field_on = false;
    const nrfx_nfct_evt_t event = {.evt_id = NRFX_NFCT_EVT_FIELD_LOST};
    driver_callback(&event);
}

static void emit_hfclk_started(void) {
    nrf_drv_clock_handler_item_t *item = clock_handler_item;
    clock_handler_item = NULL;
    clock_running = true;
    if (item != NULL) {
        item->p_next = NULL;
        item->event_handler(NRF_DRV_CLOCK_EVT_HFCLK_STARTED);
    }
}

static void emit_rx(uint8_t byte, uint8_t bits, uint32_t status) {
    uint8_t *rx_buffer = (uint8_t *)(uintptr_t)NRF_NFCT->PACKETPTR;
    rx_buffer[0] = byte;
    NRF_NFCT->RXD.AMOUNT = bits;
    const nrfx_nfct_evt_t event = {
        .evt_id = NRFX_NFCT_EVT_RX_FRAMEEND,
        .params.rx_frameend = {
            .rx_status = status,
            .rx_data = {.data_size = 1U, .p_data = rx_buffer},
        },
    };
    driver_callback(&event);
}

static void emit_frame_delay_timeout(void) {
    const nrfx_nfct_evt_t event = {
        .evt_id = NRFX_NFCT_EVT_ERROR,
        .params.error = {.reason = NRFX_NFCT_ERROR_FRAMEDELAYTIMEOUT},
    };
    driver_callback(&event);
}

static void emit_tx_frameend(void) {
    const nrfx_nfct_evt_t event = {.evt_id = NRFX_NFCT_EVT_TX_FRAMEEND};
    driver_callback(&event);
}

static void prepare_case(void) {
    nfc_tag_14a_sense_switch(false);

    memset((void *)NRF_NFCT, 0, sizeof(*NRF_NFCT));
    driver_callback = NULL;
    driver_enabled = false;
    driver_field_on = false;
    driver_init_count = 0;
    driver_uninit_count = 0;
    driver_enable_count = 0;
    driver_disable_count = 0;
    autocolres_disable_count = 0;
    activate_count = 0;
    clock_handler_item = NULL;
    clock_driver_initialized = false;
    clock_running = false;
    clock_outstanding = 0;
    clock_request_count = 0;
    clock_release_count = 0;
    frame_delay_max = 0;
    interrupt_enable_mask = 0;
    assertion_failures = 0;
    reset_count = 0;
    state_count = 0;
    field_on_count = 0;
    field_off_count = 0;
    sleep_stop_count = 0;
    sleep_start_count = 0;
    g_is_tag_emulating = false;
    g_usb_led_marquee_enable = true;

    const nfc_tag_14a_handler_t handler = {
        .cb_reset = reset_callback,
        .cb_state = state_callback,
        .get_coll_res = get_collision_resources,
        .cb_field_on = field_on_callback,
        .cb_field_off = field_off_callback,
        .cb_rats = rats_callback,
    };
    nfc_tag_14a_set_handler((nfc_tag_14a_handler_t *)&handler);
    nfc_tag_14a_sense_switch(true);
}

static void activate_field(void) {
    emit_field_detected();
    emit_hfclk_started();
}

static int test_delayed_hfclk_start(void) {
    prepare_case();
    CHECK(driver_init_count == 1U && driver_enable_count == 1U);

    emit_field_detected();
    CHECK(clock_request_count == 1U && clock_outstanding == 1U);
    CHECK(activate_count == 0U && autocolres_disable_count == 0U);
    CHECK(NRF_NFCT->TASKS_ENABLERXDATA == 0U);

    emit_hfclk_started();
    CHECK(activate_count == 1U && autocolres_disable_count == 1U);
    CHECK(NRF_NFCT->TASKS_ENABLERXDATA == 1U);
    CHECK(g_is_tag_emulating && field_on_count == 1U);

    emit_field_lost();
    CHECK(clock_release_count == 1U && clock_outstanding == 0U);
    CHECK(!g_is_tag_emulating && field_off_count == 1U);
    CHECK(assertion_failures == 0U);
    return 0;
}

static int test_field_lost_before_hfclk_start(void) {
    prepare_case();
    emit_field_detected();
    CHECK(clock_request_count == 1U && activate_count == 0U);

    emit_field_lost();
    CHECK(clock_release_count == 1U && clock_outstanding == 0U);
    CHECK(!g_is_tag_emulating && field_off_count == 1U);

    NRF_NFCT->TASKS_ENABLERXDATA = 0U;
    emit_hfclk_started();
    CHECK(activate_count == 0U && autocolres_disable_count == 0U);
    CHECK(NRF_NFCT->TASKS_ENABLERXDATA == 0U);
    CHECK(assertion_failures == 0U);
    return 0;
}

static int test_hfclk_request_release_pairing(void) {
    prepare_case();
    for (unsigned index = 0; index < 3U; index++) {
        emit_field_detected();
        emit_hfclk_started();
        emit_field_lost();
        CHECK(clock_request_count == index + 1U);
        CHECK(clock_release_count == index + 1U);
        CHECK(clock_outstanding == 0U);
    }

    emit_field_detected();
    emit_hfclk_started();
    nfc_tag_14a_sense_switch(false);
    CHECK(clock_request_count == 4U && clock_release_count == 4U);
    CHECK(clock_outstanding == 0U && assertion_failures == 0U);
    return 0;
}

static int test_frame_delay_timeout_recovers_rx(void) {
    prepare_case();
    activate_field();
    emit_rx(NFC_TAG_14A_CMD_REQA, 7U, 0U);
    CHECK(NRF_NFCT->TASKS_STARTTX == 1U);

    TEST_NFCT_STOPTX_TASK = 0U;
    NRF_NFCT->TASKS_ENABLERXDATA = 0U;
    unsigned reset_before_timeout = reset_count;
    emit_frame_delay_timeout();
    CHECK(TEST_NFCT_STOPTX_TASK == 1U);
    CHECK(NRF_NFCT->TASKS_ENABLERXDATA == 1U);
    CHECK(reset_count == reset_before_timeout + 1U);
    CHECK(m_tag_state_14a == NFC_TAG_STATE_14A_IDLE);
    CHECK(assertion_failures == 0U);

    /* Recovery must not depend on whether software believes TX was armed. */
    TEST_NFCT_STOPTX_TASK = 0U;
    NRF_NFCT->TASKS_ENABLERXDATA = 0U;
    reset_before_timeout = reset_count;
    emit_frame_delay_timeout();
    CHECK(TEST_NFCT_STOPTX_TASK == 1U);
    CHECK(NRF_NFCT->TASKS_ENABLERXDATA == 1U);
    CHECK(reset_count == reset_before_timeout + 1U);
    CHECK(m_tag_state_14a == NFC_TAG_STATE_14A_IDLE);
    return 0;
}

static int test_bad_rx_status_is_discarded(void) {
    prepare_case();
    activate_field();
    nfc_tag_14a_set_state(NFC_TAG_STATE_14A_ACTIVE);
    reset_count = 0;
    state_count = 0;
    NRF_NFCT->TASKS_STARTTX = 0U;
    NRF_NFCT->TASKS_ENABLERXDATA = 0U;

    emit_rx(0x40U, 7U, 1U);
    CHECK(reset_count == 0U && state_count == 0U);
    CHECK(NRF_NFCT->TASKS_STARTTX == 0U);
    CHECK(NRF_NFCT->TASKS_ENABLERXDATA == 1U);
    CHECK(assertion_failures == 0U);
    return 0;
}

static int test_reqa_wupa_retries_send_atqa(void) {
    const nfc_tag_14a_state_t states[] = {
        NFC_TAG_STATE_14A_READY,
        NFC_TAG_STATE_14A_READY,
        NFC_TAG_STATE_14A_ACTIVE,
        NFC_TAG_STATE_14A_ACTIVE,
    };
    const uint8_t commands[] = {
        NFC_TAG_14A_CMD_REQA,
        NFC_TAG_14A_CMD_WUPA,
        NFC_TAG_14A_CMD_REQA,
        NFC_TAG_14A_CMD_WUPA,
    };

    for (unsigned index = 0; index < 4U; index++) {
        prepare_case();
        activate_field();
        nfc_tag_14a_set_state(states[index]);
        reset_count = 0;
        NRF_NFCT->TASKS_STARTTX = 0U;

        emit_rx(commands[index], 7U, 0U);
        CHECK(reset_count == 1U);
        CHECK(m_tag_state_14a == NFC_TAG_STATE_14A_READY);
        CHECK(NRF_NFCT->TASKS_STARTTX == 1U);
        CHECK(NRF_NFCT->TXD.AMOUNT == (2U << NFCT_TXD_AMOUNT_TXDATABYTES_Pos));
        const uint8_t *tx = (const uint8_t *)(uintptr_t)NRF_NFCT->PACKETPTR;
        CHECK(tx[0] == atqa[0] && tx[1] == atqa[1]);
        CHECK(assertion_failures == 0U);
    }
    return 0;
}

static int test_disable_reenable_accepts_new_field(void) {
    prepare_case();
    activate_field();
    CHECK(field_on_count == 1U && activate_count == 1U);

    nfc_tag_14a_sense_switch(false);
    CHECK(driver_uninit_count == 1U);
    CHECK(clock_release_count == 1U && clock_outstanding == 0U);
    CHECK(field_off_count == 1U && !g_is_tag_emulating);

    nfc_tag_14a_sense_switch(true);
    CHECK(driver_init_count == 2U && driver_enable_count == 2U);
    emit_field_detected();
    CHECK(clock_request_count == 2U && activate_count == 1U);
    emit_hfclk_started();
    CHECK(field_on_count == 2U && activate_count == 2U);
    CHECK(NRF_NFCT->TASKS_ENABLERXDATA == 1U);

    emit_field_lost();
    CHECK(clock_release_count == 2U && clock_outstanding == 0U);
    CHECK(field_off_count == 2U && assertion_failures == 0U);
    return 0;
}

static int test_late_field_events_after_disable_are_ignored(void) {
    prepare_case();
    activate_field();
    nrfx_nfct_handler_t late_callback = driver_callback;

    nfc_tag_14a_sense_switch(false);
    CHECK(field_on_count == 1U && field_off_count == 1U);
    CHECK(clock_request_count == 1U && clock_release_count == 1U);

    const nrfx_nfct_evt_t field_lost = {.evt_id = NRFX_NFCT_EVT_FIELD_LOST};
    const nrfx_nfct_evt_t field_detected = {.evt_id = NRFX_NFCT_EVT_FIELD_DETECTED};
    late_callback(&field_lost);
    late_callback(&field_detected);

    CHECK(field_on_count == 1U && field_off_count == 1U);
    CHECK(clock_request_count == 1U && clock_release_count == 1U);
    CHECK(clock_outstanding == 0U && !g_is_tag_emulating);
    CHECK(assertion_failures == 0U);
    return 0;
}

static int test_frameend_after_field_lost_is_ignored(void) {
    prepare_case();
    activate_field();
    nfc_tag_14a_set_state(NFC_TAG_STATE_14A_ACTIVE);
    emit_field_lost();

    reset_count = 0U;
    state_count = 0U;
    NRF_NFCT->TASKS_STARTTX = 0U;
    NRF_NFCT->TASKS_ENABLERXDATA = 0U;
    emit_rx(0x40U, 7U, 0U);
    emit_tx_frameend();

    CHECK(reset_count == 0U && state_count == 0U);
    CHECK(NRF_NFCT->TASKS_STARTTX == 0U);
    CHECK(NRF_NFCT->TASKS_ENABLERXDATA == 0U);
    CHECK(clock_outstanding == 0U && !g_is_tag_emulating);
    CHECK(assertion_failures == 0U);
    return 0;
}

static int test_synchronous_hfclk_callback(void) {
    prepare_case();
    /* Model an HFXO already owned by another clock client. */
    clock_running = true;
    clock_outstanding = 1U;

    emit_field_detected();
    CHECK(clock_request_count == 1U && clock_outstanding == 2U);
    CHECK(activate_count == 1U && autocolres_disable_count == 1U);
    CHECK(NRF_NFCT->TASKS_ENABLERXDATA == 1U);

    emit_field_lost();
    CHECK(clock_release_count == 1U && clock_outstanding == 1U);
    CHECK(clock_running && field_on_count == 1U && field_off_count == 1U);
    CHECK(assertion_failures == 0U);
    return 0;
}

static int test_reenable_while_hfclk_pending(void) {
    prepare_case();
    emit_field_detected();
    CHECK(clock_request_count == 1U && activate_count == 0U);

    nfc_tag_14a_sense_switch(false);
    CHECK(clock_release_count == 1U && clock_outstanding == 0U);
    CHECK(field_on_count == 1U && field_off_count == 1U);

    nfc_tag_14a_sense_switch(true);
    emit_field_detected();
    CHECK(clock_request_count == 2U && activate_count == 0U);
    emit_hfclk_started();
    CHECK(activate_count == 1U && autocolres_disable_count == 1U);
    CHECK(field_on_count == 2U && NRF_NFCT->TASKS_ENABLERXDATA == 1U);

    emit_field_lost();
    CHECK(clock_release_count == 2U && clock_outstanding == 0U);
    CHECK(field_off_count == 2U && assertion_failures == 0U);
    return 0;
}

static int test_spurious_field_lost_does_not_claim_other_mode(void) {
    prepare_case();
    g_is_tag_emulating = true;
    const nrfx_nfct_evt_t field_lost = {.evt_id = NRFX_NFCT_EVT_FIELD_LOST};
    driver_callback(&field_lost);

    CHECK(field_off_count == 0U && clock_release_count == 0U);
    CHECK(g_is_tag_emulating && driver_enabled);
    CHECK(assertion_failures == 0U);
    g_is_tag_emulating = false;
    return 0;
}

int test_nfc_14a_driver(void) {
    int result;

    nfc_14a_test_case = 1U;
    result = test_delayed_hfclk_start();
    if (result != 0) return result;

    nfc_14a_test_case = 2U;
    result = test_field_lost_before_hfclk_start();
    if (result != 0) return result;

    nfc_14a_test_case = 3U;
    result = test_hfclk_request_release_pairing();
    if (result != 0) return result;

    nfc_14a_test_case = 4U;
    result = test_frame_delay_timeout_recovers_rx();
    if (result != 0) return result;

    nfc_14a_test_case = 5U;
    result = test_bad_rx_status_is_discarded();
    if (result != 0) return result;

    nfc_14a_test_case = 6U;
    result = test_reqa_wupa_retries_send_atqa();
    if (result != 0) return result;

    nfc_14a_test_case = 7U;
    result = test_disable_reenable_accepts_new_field();
    if (result != 0) return result;

    nfc_14a_test_case = 8U;
    result = test_late_field_events_after_disable_are_ignored();
    if (result != 0) return result;

    nfc_14a_test_case = 9U;
    result = test_frameend_after_field_lost_is_ignored();
    if (result != 0) return result;

    nfc_14a_test_case = 10U;
    result = test_synchronous_hfclk_callback();
    if (result != 0) return result;

    nfc_14a_test_case = 11U;
    result = test_reenable_while_hfclk_pending();
    if (result != 0) return result;

    nfc_14a_test_case = 12U;
    result = test_spurious_field_lost_does_not_claim_other_mode();
    if (result != 0) return result;

    nfc_tag_14a_sense_switch(false);
    return 0;
}
