#include "nfc_14a_4.h"
#include "app_timer.h"
#include "tag_persistence.h"
#include "crc_utils.h"
#include <string.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

static nfc_tag_14a_handler_t handler;
static bool reset_enabled;
static bool bridge_can_accept;
static uint8_t received_capdu[NFC_14A_4_MAX_CAPDU];
static uint16_t received_length;
static unsigned capdu_attempts;
static unsigned capdu_count;
static unsigned field_on_count;
static unsigned field_off_count;
static uint8_t last_tx[256];
static uint16_t last_tx_len;
static unsigned tx_count;
static unsigned abort_count;
static bool last_tx_async;
static test_timer_t *timer;
static unsigned critical_depth;
static bool interrupt_on_exit;
static unsigned interrupt_exit_countdown;
static uint8_t interrupt_pcb;
static bool field_loss_during_copy;

void *memcpy(void *to, const void *from, size_t length) {
    if (field_loss_during_copy && length == NFC_14A_4_MAX_RAPDU) {
        field_loss_during_copy = false;
        handler.cb_field_off();
    }
    unsigned char *d = to;
    const unsigned char *s = from;
    for (size_t i = 0; i < length; i++) d[i] = s[i];
    return to;
}
void *memset(void *to, int value, size_t length) {
    unsigned char *d = to;
    for (size_t i = 0; i < length; i++) d[i] = (unsigned char)value;
    return to;
}
int memcmp(const void *a, const void *b, size_t length) {
    const unsigned char *p = a, *q = b;
    for (size_t i = 0; i < length; i++) if (p[i] != q[i]) return p[i] - q[i];
    return 0;
}
void nfc_tag_14a_set_handler(nfc_tag_14a_handler_t *value) { handler = *value; }
void nfc_tag_14a_get_handler(nfc_tag_14a_handler_t *value) { *value = handler; }
void nfc_tag_14a_set_reset_enable(bool enabled) { reset_enabled = enabled; }
bool nfc_tag_14a_is_reset_enable(void) { return reset_enabled; }
bool is_valid_uid_size(uint8_t length) { return length == 4 || length == 7 || length == 10; }
void nfc_tag_14a_abort_tx(void) { abort_count++; }
void nfc_tag_14a_tx_bytes(uint8_t *data, uint32_t length, bool crc) {
    (void)crc;
    memcpy(last_tx, data, length);
    last_tx_len = length;
    tx_count++;
    last_tx_async = false;
}
void nfc_tag_14a_tx_bytes_now(uint8_t *data, uint32_t length, bool crc) {
    nfc_tag_14a_tx_bytes(data, length, crc);
    last_tx_async = true;
}
bool nfc_tag_14a_checks_crc(uint8_t *data, size_t length) {
    uint8_t expected[2];
    calc_14a_crc_lut(data, (int)length - 2, expected);
    return expected[0] == data[length - 2] && expected[1] == data[length - 1];
}
void get_fds_map_by_slot_sense_type_for_dump(uint8_t slot, tag_sense_type_t sense,
                                            fds_slot_record_map_t *map) {
    (void)slot; (void)sense; map->id = 0; map->key = 0;
}
bool fds_write_sync(uint16_t id, uint16_t key, uint16_t length, void *data) {
    (void)id; (void)key; (void)length; (void)data; return true;
}
uint32_t app_timer_create(app_timer_id_t const *id, unsigned mode,
                          app_timer_timeout_handler_t callback) {
    (void)mode; timer = *id; timer->handler = callback; return 0;
}
uint32_t app_timer_start(app_timer_id_t id, uint32_t ticks, void *context) {
    id->ticks = ticks; id->context = context; id->armed = true; return 0;
}
uint32_t app_timer_stop(app_timer_id_t id) { id->armed = false; return 0; }
void app_util_critical_region_enter(uint8_t *nested) {
    *nested = critical_depth != 0; critical_depth++;
}
static void receive(const uint8_t *data, uint16_t length) {
    uint8_t frame[256];
    memcpy(frame, data, length);
    calc_14a_crc_lut(frame, length, frame + length);
    handler.cb_state(frame, (length + 2U) * 8U);
}
void app_util_critical_region_exit(uint8_t nested) {
    (void)nested;
    critical_depth--;
    if (critical_depth == 0 && interrupt_on_exit && --interrupt_exit_countdown == 0) {
        interrupt_on_exit = false;
        const uint8_t reply[] = {interrupt_pcb, 59};
        receive(reply, sizeof(reply));
    }
}
static void fire_timer(void) {
    if (timer->armed) {
        timer->armed = false;
        timer->handler(timer->context);
    }
}
static void on_field_on(void) { field_on_count++; }
static void on_field_off(void) { field_off_count++; }
static bool on_capdu(const uint8_t *data, uint16_t length) {
    capdu_attempts++;
    if (!bridge_can_accept) return false;
    memcpy(received_capdu, data, length);
    received_length = length;
    capdu_count++;
    return true;
}
static void saved_reset(void) { }

static int test_parity(void) {
    uint8_t input[256], parity[256], raw[289], result[257], received_parity[256];
    for (unsigned i = 0; i < 256; i++) {
        input[i] = (uint8_t)(i * 113U); parity[i] = i & 1U;
    }
    for (unsigned count = 2; count <= 256; count++) {
        memset(raw, 0xA5, sizeof(raw));
        uint16_t bits = nfc_tag_14a_wrap_frame(input, count * 8U, parity, raw);
        CHECK(bits == count * 9U);
        CHECK(raw[(bits + 7U) / 8U] == 0xA5);
        memset(result, 0xA5, sizeof(result));
        CHECK(nfc_tag_14a_unwrap_frame(raw, bits, result, received_parity) == count * 8U);
        CHECK(memcmp(result, input, count) == 0);
        CHECK(memcmp(received_parity, parity, count) == 0);
        CHECK(result[count] == 0xA5);
        CHECK(nfc_tag_14a_unwrap_frame(raw, bits, raw, NULL) == count * 8U);
        CHECK(memcmp(raw, input, count) == 0);
    }
    uint8_t short_frame = 0x26, short_result = 0;
    CHECK(nfc_tag_14a_unwrap_frame(&short_frame, 7, &short_result, NULL) == 7);
    CHECK(short_result == short_frame);
    const uint8_t golden_bytes[] = {0x12, 0x34};
    const uint8_t golden_parity[] = {1, 0};
    const uint8_t golden_frame[] = {0x12, 0x69, 0};
    CHECK(nfc_tag_14a_wrap_frame(golden_bytes, 16, golden_parity, raw) == 18);
    CHECK(memcmp(raw, golden_frame, sizeof(golden_frame)) == 0);
    return 0;
}

int test_iso_dep(void) {
    int parity_result = test_parity();
    if (parity_result != 0) return parity_result;
    nfc_tag_14a_coll_res_entity_t profile;
    memset(&profile, 0, sizeof(profile));
    profile.size = NFC_TAG_14A_UID_DOUBLE_SIZE;
    profile.sak[0] = 0x20;
    profile.uid[0] = 0x04;
    const uint8_t ats[] = {5, 0x78, 0x80, 0x80, 2};
    profile.ats.length = sizeof(ats);
    memcpy(profile.ats.data, ats, sizeof(ats));
    handler.cb_reset = saved_reset;
    reset_enabled = true;
    const nfc_tag_14a_4_runtime_callbacks_t callbacks = {
        .on_field_on = on_field_on, .on_field_off = on_field_off, .on_capdu = on_capdu,
    };
    CHECK(nfc_tag_14a_4_runtime_install(&profile, &callbacks));
    CHECK(nfc_tag_14a_4_runtime_install(&profile, &callbacks));
    CHECK(!reset_enabled);

    /* Raw NFC technology polling may raise and drop the RF field without
     * activating ISO-DEP.  Bridge sessions begin only after a valid RATS. */
    handler.cb_field_on();
    CHECK(field_on_count == 0);
    handler.cb_field_off();
    CHECK(field_off_count == 0);
    handler.cb_field_on();
    handler.cb_rats(256, 0);
    CHECK(field_on_count == 1);
    bridge_can_accept = true;

    uint8_t command[NFC_14A_4_MAX_CAPDU];
    for (unsigned i = 0; i < sizeof(command); i++) command[i] = (uint8_t)i;
    uint8_t frame[254];
    frame[0] = 0x12;
    memcpy(frame + 1, command, 253);
    receive(frame, 254);
    CHECK(last_tx_len == 1 && last_tx[0] == 0xA3);
    CHECK(capdu_count == 0);
    receive(frame, 254);
    CHECK(last_tx_len == 1 && last_tx[0] == 0xA3);
    frame[0] = 0x03;
    memcpy(frame + 1, command + 253, 44);
    receive(frame, 45);
    CHECK(capdu_count == 1 && received_length == sizeof(command));
    CHECK(memcmp(command, received_capdu, sizeof(command)) == 0);
    CHECK(last_tx_len == 2 && last_tx[0] == 0xF2 && last_tx[1] == 59);
    unsigned before = tx_count;
    const uint8_t wrong_wtx[] = {0xF2, 58};
    receive(wrong_wtx, sizeof(wrong_wtx));
    CHECK(tx_count == before);
    const uint8_t wtx[] = {0xF2, 59};
    receive(wtx, sizeof(wtx));
    CHECK(tx_count == before && timer->armed);
    CHECK(timer->ticks >= 2200 && timer->ticks <= 2300);
    fire_timer();
    CHECK(tx_count == before + 1 && last_tx_async && last_tx[0] == 0xF2);

    uint8_t response[NFC_14A_4_MAX_RAPDU];
    for (unsigned i = 0; i < sizeof(response); i++) response[i] = (uint8_t)(i * 31U);
    response[sizeof(response) - 2] = 0x61;
    response[sizeof(response) - 1] = 0xFF;
    CHECK(nfc_tag_14a_4_submit_response(response, sizeof(response)));
    CHECK(!nfc_tag_14a_4_submit_response(response, sizeof(response)));
    CHECK(last_tx[0] == 0xF2);
    receive(wtx, sizeof(wtx));
    uint16_t received_response = 0;
    uint8_t tx_number = 0;
    unsigned fragments = 0;
    while (true) {
        CHECK(last_tx_len <= 254 && last_tx_len >= 2);
        CHECK((last_tx[0] & 0xEFU) == (0x02U | tx_number));
        CHECK(memcmp(last_tx + 1, response + received_response, last_tx_len - 1) == 0);
        received_response += last_tx_len - 1;
        fragments++;
        if ((last_tx[0] & 0x10) == 0) break;
        if (fragments == 1) {
            uint8_t previous[256];
            memcpy(previous, last_tx, last_tx_len);
            uint16_t old_length = last_tx_len;
            const uint8_t nak[] = {0xB2};
            receive(nak, sizeof(nak));
            CHECK(last_tx_len == old_length && memcmp(last_tx, previous, old_length) == 0);
            const uint8_t duplicate_ack[] = {0xA2};
            receive(duplicate_ack, sizeof(duplicate_ack));
            CHECK(last_tx_len == old_length && memcmp(last_tx, previous, old_length) == 0);
        }
        tx_number ^= 1U;
        uint8_t ack = 0xA2U | tx_number;
        receive(&ack, 1);
    }
    CHECK(received_response == sizeof(response) && fragments == 17);
    CHECK(!timer->armed);

    bridge_can_accept = false;
    const uint8_t next[] = {0x02, 0, 0xA4, 4, 0};
    receive(next, sizeof(next));
    CHECK(capdu_count == 1 && last_tx[0] == 0xF2);
    before = capdu_attempts;
    receive(wtx, sizeof(wtx));
    CHECK(capdu_attempts == before + 1 && timer->armed);
    bridge_can_accept = true;
    fire_timer();
    CHECK(capdu_count == 2 && received_length == 4);
    CHECK(memcmp(received_capdu, next + 1, 4) == 0);
    const uint8_t cancel_response[] = {0x2D, 0x90, 0x00};
    CHECK(nfc_tag_14a_4_submit_response(cancel_response, sizeof(cancel_response)));
    receive(wtx, sizeof(wtx));
    CHECK(last_tx_len == 4 && memcmp(last_tx + 1, cancel_response, 3) == 0);

    const uint8_t third[] = {0x03, 0x80, 0x10, 0, 0};
    receive(third, sizeof(third));
    receive(wtx, sizeof(wtx));
    CHECK(timer->armed);
    before = tx_count;
    const uint8_t continue_response[] = {0x91, 0x00};
    CHECK(nfc_tag_14a_4_submit_response(continue_response, sizeof(continue_response)));
    CHECK(tx_count == before + 1 && last_tx_async && !timer->armed);
    CHECK(last_tx_len == 3 && memcmp(last_tx + 1, continue_response, 2) == 0);

    const uint8_t deselect[] = {0xC2};
    receive(deselect, sizeof(deselect));
    CHECK(field_off_count == 1 && last_tx_len == 1 && last_tx[0] == 0xC2);
    CHECK(!nfc_tag_14a_4_submit_response(cancel_response, sizeof(cancel_response)));
    handler.cb_reset();
    handler.cb_rats(16, 3);
    CHECK(field_on_count == 2);
    const uint8_t cid_command[] = {0x0A, 3, 0x80, 0x10, 0, 0};
    receive(cid_command, sizeof(cid_command));
    CHECK(last_tx_len == 3 && last_tx[0] == 0xFA && last_tx[1] == 3);
    const uint8_t cid_wtx[] = {0xFA, 3, 59};
    CHECK(nfc_tag_14a_4_submit_response(response, 30));
    receive(cid_wtx, sizeof(cid_wtx));
    CHECK(last_tx_len == 14 && last_tx[0] == 0x1A && last_tx[1] == 3);
    CHECK(memcmp(last_tx + 2, response, 12) == 0);
    const uint8_t cid_ack[] = {0xAB, 3};
    receive(cid_ack, sizeof(cid_ack));
    CHECK(last_tx_len == 14 && last_tx[0] == 0x1B && last_tx[1] == 3);
    handler.cb_field_off();
    CHECK(field_off_count == 2 && abort_count > 0);
    before = tx_count;
    receive(cid_ack, sizeof(cid_ack));
    CHECK(tx_count == before && !timer->armed);
    CHECK(!nfc_tag_14a_4_submit_response(response, 30));

    handler.cb_field_on();
    handler.cb_rats(256, 0);
    receive(next, sizeof(next));
    before = field_off_count;
    handler.cb_reset();
    CHECK(field_off_count == before + 1 && field_on_count == 3);
    CHECK(!nfc_tag_14a_4_submit_response(cancel_response, sizeof(cancel_response)));

    handler.cb_rats(256, 0);
    CHECK(field_on_count == 4);
    receive(next, sizeof(next));
    interrupt_on_exit = true;
    interrupt_exit_countdown = 2;
    interrupt_pcb = 0xF2;
    before = tx_count;
    CHECK(nfc_tag_14a_4_submit_response(cancel_response, sizeof(cancel_response)));
    CHECK(tx_count == before + 1 && last_tx_len == 4);
    CHECK(!interrupt_on_exit && !timer->armed);

    receive(third, sizeof(third));
    receive(wtx, sizeof(wtx));
    CHECK(timer->armed);
    app_timer_timeout_handler_t stale_handler = timer->handler;
    void *stale_context = timer->context;
    before = tx_count;
    field_loss_during_copy = true;
    CHECK(!nfc_tag_14a_4_submit_response(response, sizeof(response)));
    CHECK(!field_loss_during_copy && !timer->armed && tx_count == before);
    stale_handler(stale_context);
    CHECK(tx_count == before);
    handler.cb_field_on();
    handler.cb_rats(256, 0);
    receive(next, sizeof(next));
    receive(wtx, sizeof(wtx));
    CHECK(timer->armed);
    before = tx_count;
    stale_handler(stale_context);
    CHECK(tx_count == before && timer->armed);
    fire_timer();
    CHECK(tx_count == before + 1 && last_tx_async);

    /* A protocol reset must retire an activated bridge session even after the
     * response completed.  The following RATS then creates a fresh session. */
    CHECK(nfc_tag_14a_4_submit_response(cancel_response,
                                        sizeof(cancel_response)));
    receive(wtx, sizeof(wtx));
    CHECK(last_tx_len == 4 &&
          memcmp(last_tx + 1, cancel_response,
                 sizeof(cancel_response)) == 0);
    unsigned completed_field_on = field_on_count;
    unsigned completed_field_off = field_off_count;
    handler.cb_reset();
    CHECK(field_off_count == completed_field_off + 1);
    CHECK(field_on_count == completed_field_on);
    CHECK(!nfc_tag_14a_4_submit_response(cancel_response,
                                         sizeof(cancel_response)));
    handler.cb_rats(256, 0);
    CHECK(field_on_count == completed_field_on + 1);
    handler.cb_field_off();
    CHECK(field_off_count == completed_field_off + 2);

    nfc_tag_14a_4_runtime_uninstall();
    CHECK(!nfc_tag_14a_4_runtime_is_installed());
    CHECK(handler.cb_reset == saved_reset && reset_enabled);

    nfc_tag_14a_4_information_t ordinary;
    memset(&ordinary, 0, sizeof(ordinary));
    ordinary.res_coll = profile;
    ordinary.res_coll.ats.data[3] = 0x70;
    ordinary.static_resp_count = 1;
    ordinary.static_resp[0].cmd_len = 1;
    ordinary.static_resp[0].cmd[0] = 0;
    ordinary.static_resp[0].resp_len = 2;
    ordinary.static_resp[0].resp[0] = 0x90;
    ordinary.static_resp[0].resp[1] = 0;
    tag_data_buffer_t slot_buffer = {
        .buffer = (uint8_t *)&ordinary, .length = sizeof(ordinary),
    };
    CHECK(nfc_tag_14a_4_data_loadcb(TAG_TYPE_HF14A_4, &slot_buffer) == sizeof(ordinary));
    nfc_tag_14a_handler_t ordinary_handler = handler;
    CHECK(nfc_tag_14a_4_runtime_install(&profile, &callbacks));
    CHECK(nfc_tag_14a_4_runtime_install(&profile, &callbacks));
    nfc_tag_14a_4_runtime_uninstall();
    CHECK(handler.cb_state == ordinary_handler.cb_state && reset_enabled);
    CHECK(handler.get_coll_res()->uid == ordinary.res_coll.uid);
    handler.cb_field_on();
    handler.cb_rats(256, 0);
    receive(next, sizeof(next));
    CHECK(last_tx_len == 3 && last_tx[0] == 0x02 && last_tx[1] == 0x90);
    uint8_t pending[NFC_14A_4_MAX_CAPDU];
    uint16_t pending_len;
    CHECK(nfc_tag_14a_4_get_pending_apdu(pending, &pending_len));
    CHECK(pending_len == 4 && memcmp(pending, next + 1, 4) == 0);
    const uint8_t ordinary_second[] = {0x03, 0, 0xA4, 4, 0};
    receive(ordinary_second, sizeof(ordinary_second));
    CHECK(last_tx_len == 3 && last_tx[0] == 0x03 && last_tx[1] == 0x90);
    const uint8_t partial[] = {0x12, 0x80, 0x10};
    receive(partial, sizeof(partial));
    CHECK(last_tx_len == 1 && last_tx[0] == 0xA3);
    CHECK(!nfc_tag_14a_4_get_pending_apdu(pending, &pending_len));
    const uint8_t tail[] = {0x03, 0, 0};
    receive(tail, sizeof(tail));
    CHECK(last_tx_len == 2 && last_tx[0] == 0xF2);
    CHECK(nfc_tag_14a_4_get_pending_apdu(pending, &pending_len));
    CHECK(pending_len == 4 && memcmp(pending, third + 1, 4) == 0);
    receive(wtx, sizeof(wtx));
    CHECK(timer->armed && timer->ticks >= 1100 && timer->ticks <= 1150);
    handler.cb_field_off();
    CHECK(critical_depth == 0);
    return 0;
}
