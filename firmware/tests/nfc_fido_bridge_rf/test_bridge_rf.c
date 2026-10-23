#include <string.h>

#include "app_timer.h"
#include "app_util_platform.h"
#include "crc_utils.h"
#include "fds_util.h"
#include "nfc_14a_4.h"
#include "nfc_fido_bridge.h"
#include "nrf.h"
#include "rfid_main.h"
#include "syssleep.h"
#include "tag_persistence.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)
#define EVENT_NONE 0U
#define EVENT_FIELD_ON 1U
#define EVENT_FIELD_OFF 2U
#define EVENT_APDU 3U

typedef struct {
    uint8_t data[NFC_FIDO_BRIDGE_EVENT_HEADER_SIZE + NFC_FIDO_BRIDGE_MAX_CAPDU];
    uint16_t length;
} test_event_t;

bridge_rf_ficr_t bridge_rf_ficr = {{0x12345678U, 0x9ABCDEF0U}};
volatile unsigned bridge_test_case;

static nfc_tag_14a_handler_t handler;
static bool reset_enabled;
static device_mode_t mode;
static bool sense_enabled;
static bool sleep_stopped;
static uint32_t sleep_delay;
static uint32_t now_ticks = 1234U;
static unsigned reload_count;
static unsigned flash_write_count;
static unsigned critical_depth;
static bool critical_fault;
static bool rf_fault;
static uint8_t last_tx[256];
static uint16_t last_tx_length;
static unsigned tx_count;
static unsigned i_block_count;
static unsigned abort_count;
static bool last_tx_async;
static test_timer_t *timer;

static uint32_t read_be32(const uint8_t *data) {
    return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) | data[3];
}

static void write_be32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)(value >> 24U);
    data[1] = (uint8_t)(value >> 16U);
    data[2] = (uint8_t)(value >> 8U);
    data[3] = (uint8_t)value;
}

static uint32_t event_session(const test_event_t *event) {
    return read_be32(event->data + 1);
}

static uint32_t event_id(const test_event_t *event) {
    return read_be32(event->data + 5);
}

static nfc_fido_bridge_result_t poll_event(uint32_t session, uint32_t id,
                                          test_event_t *event) {
    uint8_t request[8];
    write_be32(request, session);
    write_be32(request + 4, id);
    event->length = 0;
    return nfc_fido_bridge_poll(request, sizeof(request), event->data,
                                &event->length);
}

static nfc_fido_bridge_result_t send_response(const test_event_t *event,
                                              const uint8_t *rapdu,
                                              uint16_t length,
                                              uint8_t *ack,
                                              uint16_t *ack_length) {
    uint8_t request[8U + NFC_FIDO_BRIDGE_MAX_RAPDU];
    memcpy(request, event->data + 1, 8U);
    memcpy(request + 8, rapdu, length);
    *ack_length = 0;
    return nfc_fido_bridge_response(request, 8U + length, ack, ack_length);
}

void nfc_tag_14a_set_handler(nfc_tag_14a_handler_t *value) { handler = *value; }
void nfc_tag_14a_get_handler(nfc_tag_14a_handler_t *value) { *value = handler; }
void nfc_tag_14a_set_reset_enable(bool enabled) { reset_enabled = enabled; }
bool nfc_tag_14a_is_reset_enable(void) { return reset_enabled; }
bool is_valid_uid_size(uint8_t size) { return size == 4U || size == 7U || size == 10U; }
void nfc_tag_14a_sense_switch(bool enabled) { sense_enabled = enabled; }
void nfc_tag_14a_abort_tx(void) { abort_count++; }

void nfc_tag_14a_tx_bytes(uint8_t *data, uint32_t length, bool crc) {
    if (!crc || length == 0U || length > sizeof(last_tx) || !sense_enabled) {
        rf_fault = true;
        return;
    }
    memcpy(last_tx, data, length);
    last_tx_length = (uint16_t)length;
    tx_count++;
    if ((data[0] & 0xE2U) == 0x02U) i_block_count++;
    last_tx_async = false;
}

void nfc_tag_14a_tx_bytes_now(uint8_t *data, uint32_t length, bool crc) {
    nfc_tag_14a_tx_bytes(data, length, crc);
    last_tx_async = true;
}

bool nfc_tag_14a_checks_crc(uint8_t *data, size_t length) {
    uint8_t expected[2];
    if (length < 2U) return false;
    calc_14a_crc_lut(data, (int)length - 2, expected);
    return expected[0] == data[length - 2] && expected[1] == data[length - 1];
}

void get_fds_map_by_slot_sense_type_for_dump(uint8_t slot, tag_sense_type_t sense,
                                            fds_slot_record_map_t *map) {
    (void)slot; (void)sense;
    map->id = 0; map->key = 0;
}

bool fds_write_sync(uint16_t id, uint16_t key, uint16_t length, void *data) {
    (void)id; (void)key; (void)length; (void)data;
    flash_write_count++;
    return true;
}

uint32_t app_timer_create(app_timer_id_t const *id, unsigned timer_mode,
                          app_timer_timeout_handler_t callback) {
    (void)timer_mode;
    timer = *id;
    timer->handler = callback;
    return NRF_SUCCESS;
}

uint32_t app_timer_start(app_timer_id_t id, uint32_t ticks, void *context) {
    id->ticks = ticks;
    id->context = context;
    id->armed = true;
    return NRF_SUCCESS;
}

uint32_t app_timer_stop(app_timer_id_t id) {
    id->armed = false;
    return NRF_SUCCESS;
}

uint32_t app_timer_cnt_get(void) { return now_ticks & 0xFFFFFFU; }
uint32_t app_timer_cnt_diff_compute(uint32_t now, uint32_t previous) {
    return (now - previous) & 0xFFFFFFU;
}

void app_util_critical_region_enter(uint8_t *nested) {
    *nested = critical_depth != 0U;
    critical_depth++;
}

void app_util_critical_region_exit(uint8_t nested) {
    if (critical_depth == 0U || nested != (critical_depth > 1U)) {
        critical_fault = true;
        return;
    }
    critical_depth--;
}

device_mode_t get_device_mode(void) { return mode; }
void tag_emulation_sense_run(void) { sense_enabled = true; }
void tag_emulation_sense_end(void) { sense_enabled = false; }
void tag_emulation_load_data(void) { reload_count++; }
void tag_mode_enter(void) { mode = DEVICE_MODE_TAG; tag_emulation_sense_run(); }
void reader_mode_enter(void) { mode = DEVICE_MODE_READER; tag_emulation_sense_end(); }
void device_mode_exit(void) { mode = DEVICE_MODE_NONE; tag_emulation_sense_end(); }
void sleep_timer_stop(void) { sleep_stopped = true; }
void sleep_timer_start(uint32_t milliseconds) {
    sleep_stopped = false;
    sleep_delay = milliseconds;
}

static void ordinary_reset(void) { }

static void receive(const uint8_t *data, uint16_t length) {
    uint8_t frame[256];
    if (length > sizeof(frame) - 2U || handler.cb_state == NULL) {
        rf_fault = true;
        return;
    }
    memcpy(frame, data, length);
    calc_14a_crc_lut(frame, length, frame + length);
    handler.cb_state(frame, (length + 2U) * 8U);
}

static void fire_timer(void) {
    if (timer != NULL && timer->armed) {
        timer->armed = false;
        timer->handler(timer->context);
    }
}

static int test_deferred_capdu(void) {
    const uint8_t configure[] = {1U, 1U, 1U, 0x29U, 0x0FU, 0xF8U};
    uint8_t reply[NFC_FIDO_BRIDGE_EVENT_HEADER_SIZE + NFC_FIDO_BRIDGE_MAX_CAPDU];
    uint16_t reply_length = 0;
    mode = DEVICE_MODE_READER;
    handler.cb_reset = ordinary_reset;
    reset_enabled = true;
    CHECK(nfc_fido_bridge_configure(configure, sizeof(configure), reply,
                                    &reply_length) == NFC_FIDO_BRIDGE_OK);
    CHECK(reply_length == sizeof(configure));
    CHECK(memcmp(reply, configure, sizeof(configure)) == 0);
    CHECK(nfc_tag_14a_4_runtime_is_installed());
    CHECK(!reset_enabled && sense_enabled && sleep_stopped);
    CHECK(mode == DEVICE_MODE_TAG);

    test_event_t inactive, on, first, replay, next;
    handler.cb_field_on();
    CHECK(poll_event(0, 0, &inactive) == NFC_FIDO_BRIDGE_OK);
    CHECK(inactive.length == NFC_FIDO_BRIDGE_EVENT_HEADER_SIZE &&
          inactive.data[0] == EVENT_NONE);
    handler.cb_rats(256U, 0);
    CHECK(poll_event(0, 0, &on) == NFC_FIDO_BRIDGE_OK);
    CHECK(on.length == NFC_FIDO_BRIDGE_EVENT_HEADER_SIZE && on.data[0] == EVENT_FIELD_ON);
    CHECK(event_session(&on) != 0U && event_id(&on) != 0U);
    CHECK(poll_event(event_session(&on), event_id(&on), &replay) == NFC_FIDO_BRIDGE_OK);
    CHECK(replay.length == NFC_FIDO_BRIDGE_EVENT_HEADER_SIZE && replay.data[0] == EVENT_NONE);

    const uint8_t first_capdu[] = {0x02U, 0x00U, 0xA4U, 0x04U, 0x00U};
    const uint8_t wtx[] = {0xF2U, 0x3BU};
    receive(first_capdu, sizeof(first_capdu));
    CHECK(last_tx_length == sizeof(wtx) && memcmp(last_tx, wtx, sizeof(wtx)) == 0);
    CHECK(poll_event(0, 0, &first) == NFC_FIDO_BRIDGE_OK);
    CHECK(first.data[0] == EVENT_APDU && event_session(&first) == event_session(&on));
    CHECK(first.length == NFC_FIDO_BRIDGE_EVENT_HEADER_SIZE + sizeof(first_capdu) - 1U);
    CHECK(memcmp(first.data + NFC_FIDO_BRIDGE_EVENT_HEADER_SIZE,
                 first_capdu + 1, sizeof(first_capdu) - 1U) == 0);

    const uint8_t first_rapdu[] = {0x61U, 0x20U};
    uint8_t ack[8];
    uint16_t ack_length;
    unsigned before = tx_count;
    CHECK(send_response(&first, first_rapdu, sizeof(first_rapdu), ack,
                         &ack_length) == NFC_FIDO_BRIDGE_OK);
    CHECK(ack_length == sizeof(ack) && memcmp(ack, first.data + 1, sizeof(ack)) == 0);
    CHECK(tx_count == before && i_block_count == 0U);
    receive(wtx, sizeof(wtx));
    CHECK(tx_count == before + 1U && i_block_count == 1U);
    CHECK(last_tx_length == 3U && last_tx[0] == 0x02U);
    CHECK(memcmp(last_tx + 1, first_rapdu, sizeof(first_rapdu)) == 0);

    /* Reader has its RAPDU; the old UART event still awaits POLL ACK. */
    const uint8_t next_capdu[] = {0x03U, 0x80U, 0x10U, 0x00U, 0x00U, 0x01U, 0x04U};
    receive(next_capdu, sizeof(next_capdu));
    CHECK(last_tx_length == sizeof(wtx) && memcmp(last_tx, wtx, sizeof(wtx)) == 0);
    CHECK(poll_event(0, 0, &replay) == NFC_FIDO_BRIDGE_OK);
    CHECK(replay.length == first.length && memcmp(replay.data, first.data, first.length) == 0);
    before = tx_count;
    CHECK(send_response(&first, first_rapdu, sizeof(first_rapdu), ack,
                         &ack_length) == NFC_FIDO_BRIDGE_OK);
    CHECK(ack_length == sizeof(ack) && memcmp(ack, first.data + 1, sizeof(ack)) == 0);
    CHECK(tx_count == before && i_block_count == 1U);
    receive(wtx, sizeof(wtx));
    CHECK(tx_count == before && timer->armed);
    CHECK(poll_event(0, 0, &replay) == NFC_FIDO_BRIDGE_OK);
    CHECK(replay.length == first.length && memcmp(replay.data, first.data, first.length) == 0);

    CHECK(poll_event(event_session(&first), event_id(&first), &next) == NFC_FIDO_BRIDGE_OK);
    CHECK(next.data[0] == EVENT_APDU && event_session(&next) == event_session(&first));
    CHECK(event_id(&next) == event_id(&first) + 1U);
    CHECK(next.length == NFC_FIDO_BRIDGE_EVENT_HEADER_SIZE + sizeof(next_capdu) - 1U);
    CHECK(memcmp(next.data + NFC_FIDO_BRIDGE_EVENT_HEADER_SIZE,
                 next_capdu + 1, sizeof(next_capdu) - 1U) == 0);

    const uint8_t next_rapdu[] = {0x91U, 0x00U};
    before = tx_count;
    CHECK(send_response(&next, next_rapdu, sizeof(next_rapdu), ack,
                         &ack_length) == NFC_FIDO_BRIDGE_OK);
    CHECK(tx_count == before + 1U && i_block_count == 2U && last_tx_async);
    CHECK(last_tx_length == 3U && last_tx[0] == 0x03U);
    CHECK(memcmp(last_tx + 1, next_rapdu, sizeof(next_rapdu)) == 0);
    CHECK(poll_event(event_session(&next), event_id(&next), &replay) == NFC_FIDO_BRIDGE_OK);
    CHECK(replay.data[0] == EVENT_NONE);
    CHECK(poll_event(0, 0, &replay) == NFC_FIDO_BRIDGE_OK && replay.data[0] == EVENT_NONE);
    CHECK(!timer->armed && !rf_fault && !critical_fault && critical_depth == 0U);
    return 0;
}

static int test_field_off_stale_timer(void) {
    const uint8_t capdu[] = {0x02U, 0x80U, 0x10U, 0x00U, 0x00U, 0x01U, 0x01U};
    const uint8_t wtx[] = {0xF2U, 0x3BU};
    const uint8_t rapdu[] = {0x2DU, 0x90U, 0x00U};
    test_event_t old, off, new_on, current, replay;
    uint8_t ack[8];
    uint16_t ack_length;
    receive(capdu, sizeof(capdu));
    CHECK(poll_event(0, 0, &old) == NFC_FIDO_BRIDGE_OK && old.data[0] == EVENT_APDU);
    receive(wtx, sizeof(wtx));
    CHECK(timer->armed);
    app_timer_timeout_handler_t stale_handler = timer->handler;
    void *stale_context = timer->context;
    unsigned before = tx_count;
    unsigned old_abort_count = abort_count;
    handler.cb_field_off();
    CHECK(abort_count > old_abort_count && !timer->armed && tx_count == before);
    CHECK(send_response(&old, rapdu, sizeof(rapdu), ack,
                         &ack_length) == NFC_FIDO_BRIDGE_INVALID_STATE);
    CHECK(ack_length == 0U && tx_count == before);
    CHECK(poll_event(0, 0, &off) == NFC_FIDO_BRIDGE_OK && off.data[0] == EVENT_FIELD_OFF);
    CHECK(off.length == NFC_FIDO_BRIDGE_EVENT_HEADER_SIZE);
    CHECK(event_session(&off) == event_session(&old) && event_id(&off) > event_id(&old));
    receive(wtx, sizeof(wtx));
    stale_handler(stale_context);
    CHECK(tx_count == before && !timer->armed);

    handler.cb_field_on();
    handler.cb_rats(256U, 0);
    receive(capdu, sizeof(capdu));
    receive(wtx, sizeof(wtx));
    CHECK(timer->armed);
    void *current_context = timer->context;
    CHECK(current_context != stale_context);
    before = tx_count;
    stale_handler(stale_context);
    CHECK(tx_count == before && timer->armed && timer->context == current_context);
    CHECK(poll_event(0, 0, &replay) == NFC_FIDO_BRIDGE_OK);
    CHECK(replay.length == off.length && memcmp(replay.data, off.data, off.length) == 0);
    CHECK(send_response(&old, rapdu, sizeof(rapdu), ack,
                         &ack_length) == NFC_FIDO_BRIDGE_INVALID_STATE);
    CHECK(tx_count == before);
    fire_timer();
    CHECK(tx_count == before + 1U && last_tx_async);
    CHECK(last_tx_length == sizeof(wtx) && memcmp(last_tx, wtx, sizeof(wtx)) == 0);

    CHECK(poll_event(event_session(&off), event_id(&off), &new_on) == NFC_FIDO_BRIDGE_OK);
    CHECK(new_on.data[0] == EVENT_FIELD_ON && new_on.length == NFC_FIDO_BRIDGE_EVENT_HEADER_SIZE);
    CHECK(event_session(&new_on) != 0U && event_session(&new_on) != event_session(&old));
    CHECK(poll_event(event_session(&new_on), event_id(&new_on), &current) == NFC_FIDO_BRIDGE_OK);
    CHECK(current.data[0] == EVENT_APDU && event_session(&current) == event_session(&new_on));
    CHECK(event_id(&current) == event_id(&new_on) + 1U);
    CHECK(current.length == NFC_FIDO_BRIDGE_EVENT_HEADER_SIZE + sizeof(capdu) - 1U);
    CHECK(memcmp(current.data + NFC_FIDO_BRIDGE_EVENT_HEADER_SIZE,
                 capdu + 1, sizeof(capdu) - 1U) == 0);
    before = tx_count;
    CHECK(send_response(&current, rapdu, sizeof(rapdu), ack,
                         &ack_length) == NFC_FIDO_BRIDGE_OK);
    CHECK(tx_count == before);
    receive(wtx, sizeof(wtx));
    CHECK(tx_count == before + 1U && last_tx_length == sizeof(rapdu) + 1U && last_tx[0] == 0x02U);
    CHECK(memcmp(last_tx + 1, rapdu, sizeof(rapdu)) == 0);
    CHECK(poll_event(event_session(&current), event_id(&current), &replay) == NFC_FIDO_BRIDGE_OK);
    CHECK(replay.data[0] == EVENT_NONE);
    CHECK(poll_event(0, 0, &replay) == NFC_FIDO_BRIDGE_OK && replay.data[0] == EVENT_NONE);

    uint16_t reset_reply_length;
    CHECK(nfc_fido_bridge_reset(NULL, 0, NULL, &reset_reply_length) == NFC_FIDO_BRIDGE_OK);
    CHECK(reset_reply_length == 0U && !nfc_fido_bridge_is_active());
    CHECK(!nfc_tag_14a_4_runtime_is_installed() && handler.cb_reset == ordinary_reset);
    CHECK(reset_enabled && mode == DEVICE_MODE_READER && !sense_enabled);
    CHECK(!timer->armed && !sleep_stopped && sleep_delay == SLEEP_DELAY_MS_BUTTON_CLICK);
    CHECK(!rf_fault && !critical_fault && critical_depth == 0U);
    CHECK(reload_count == 0U && flash_write_count == 0U);
    return 0;
}

int test_nfc_fido_bridge_rf(void) {
    bridge_test_case = 1U;
    int result = test_deferred_capdu();
    if (result != 0) return result;
    bridge_test_case = 2U;
    return test_field_off_stale_timer();
}
