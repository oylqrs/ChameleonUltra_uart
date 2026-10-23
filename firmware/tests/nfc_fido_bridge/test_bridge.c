#include <string.h>

#include "syssleep.h"
#include "test_env.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)
#define NONE 0u
#define FIELD_ON 1u
#define FIELD_OFF 2u
#define APDU 3u

typedef struct {
    uint8_t type;
    uint32_t session;
    uint32_t id;
    uint16_t length;
    uint8_t data[NFC_FIDO_BRIDGE_MAX_CAPDU];
} event_t;

static const uint8_t configure_enable[] = {1u, 1u, 1u, 0x29u, 0x0fu, 0xf8u};
static const uint8_t configure_disable[] = {1u, 0u, 1u, 0x29u, 0x0fu, 0xf8u};
static const uint8_t capdu[] = {0x80u, 0x10u, 0u, 0u, 1u, 4u, 0u};
static const uint8_t rapdu[] = {0x90u, 0u};
static uint8_t reply[NFC_FIDO_BRIDGE_EVENT_HEADER_SIZE + NFC_FIDO_BRIDGE_MAX_CAPDU];
static uint8_t response_request[8u + NFC_FIDO_BRIDGE_MAX_RAPDU + 1u];
static uint8_t response_ack[8];
static uint16_t reply_length;
static uint16_t response_ack_length;
volatile unsigned bridge_test_case;

static uint32_t read_be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | bytes[3];
}

static void write_be32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static int prepare(device_mode_t mode) {
    uint16_t length = 1u;
    CHECK(bridge_test_env.critical_depth == 0u && !bridge_test_env.critical_fault);
    CHECK(nfc_fido_bridge_reset(NULL, 0u, NULL, &length) == NFC_FIDO_BRIDGE_OK);
    CHECK(length == 0u);
    CHECK(bridge_test_env.critical_depth == 0u && !bridge_test_env.critical_fault);
    bridge_test_env_reset(mode);
    return 0;
}

static nfc_fido_bridge_result_t configure(bool enable) {
    const uint8_t *request = enable ? configure_enable : configure_disable;
    reply_length = 0u;
    nfc_fido_bridge_result_t result = nfc_fido_bridge_configure(
        request, 6u, reply, &reply_length);
    if (result == NFC_FIDO_BRIDGE_OK &&
        (reply_length != 6u || memcmp(reply, request, 6u) != 0)) {
        return NFC_FIDO_BRIDGE_INVALID_ARGUMENT;
    }
    return result;
}

static nfc_fido_bridge_result_t poll(uint32_t session, uint32_t id, event_t *event) {
    uint8_t request[8];
    write_be32(request, session);
    write_be32(request + 4, id);
    reply_length = 0u;
    nfc_fido_bridge_result_t result = nfc_fido_bridge_poll(
        request, sizeof(request), reply, &reply_length);
    if (result != NFC_FIDO_BRIDGE_OK) return result;
    if (reply_length < 9u || reply_length > sizeof(reply)) {
        return NFC_FIDO_BRIDGE_INVALID_ARGUMENT;
    }
    memset(event, 0, sizeof(*event));
    event->type = reply[0];
    event->session = read_be32(reply + 1);
    event->id = read_be32(reply + 5);
    event->length = reply_length - 9u;
    memcpy(event->data, reply + 9, event->length);
    return NFC_FIDO_BRIDGE_OK;
}

static nfc_fido_bridge_result_t respond(uint32_t session, uint32_t id,
                                      const uint8_t *data, uint16_t length) {
    write_be32(response_request, session);
    write_be32(response_request + 4, id);
    memcpy(response_request + 8, data, length);
    response_ack_length = 0u;
    nfc_fido_bridge_result_t result = nfc_fido_bridge_response(
        response_request, 8u + length, response_ack, &response_ack_length);
    if (result == NFC_FIDO_BRIDGE_OK &&
        (response_ack_length != 8u ||
         memcmp(response_request, response_ack, 8u) != 0)) {
        return NFC_FIDO_BRIDGE_INVALID_ARGUMENT;
    }
    return result;
}

static bool event_equal(const event_t *a, const event_t *b) {
    return a->type == b->type && a->session == b->session && a->id == b->id &&
           a->length == b->length && memcmp(a->data, b->data, a->length) == 0;
}

static bool event_none(const event_t *event) {
    return event->type == NONE && event->session == 0u && event->id == 0u &&
           event->length == 0u;
}

static int test_configuration_validation(void) {
    uint8_t invalid[7];
    uint8_t empty_poll[8] = {0};
    event_t event;
    CHECK(prepare(DEVICE_MODE_READER) == 0);
    for (unsigned i = 0; i < sizeof(configure_enable); i++) {
        memcpy(invalid, configure_enable, sizeof(configure_enable));
        invalid[i] ^= 0x80u;
        CHECK(nfc_fido_bridge_configure(invalid, 6u, reply, &reply_length) ==
              NFC_FIDO_BRIDGE_UNSUPPORTED);
        CHECK(!nfc_fido_bridge_is_active() && bridge_test_env.install_count == 0u);
        CHECK(get_device_mode() == DEVICE_MODE_READER);
    }
    memcpy(invalid, configure_enable, 6u);
    invalid[1] = 2u;
    CHECK(nfc_fido_bridge_configure(invalid, 6u, reply, &reply_length) ==
          NFC_FIDO_BRIDGE_UNSUPPORTED);
    CHECK(nfc_fido_bridge_configure(invalid, 5u, reply, &reply_length) ==
          NFC_FIDO_BRIDGE_INVALID_ARGUMENT);
    CHECK(nfc_fido_bridge_configure(invalid, 7u, reply, &reply_length) ==
          NFC_FIDO_BRIDGE_INVALID_ARGUMENT);
    CHECK(nfc_fido_bridge_configure(NULL, 6u, reply, &reply_length) ==
          NFC_FIDO_BRIDGE_INVALID_ARGUMENT);
    CHECK(nfc_fido_bridge_configure(configure_enable, 6u, NULL, &reply_length) ==
          NFC_FIDO_BRIDGE_INVALID_ARGUMENT);
    CHECK(nfc_fido_bridge_configure(configure_enable, 6u, reply, NULL) ==
          NFC_FIDO_BRIDGE_INVALID_ARGUMENT);
    CHECK(poll(0u, 0u, &event) == NFC_FIDO_BRIDGE_INVALID_STATE);
    CHECK(configure(true) == NFC_FIDO_BRIDGE_OK);
    CHECK(nfc_fido_bridge_poll(empty_poll, 7u, reply, &reply_length) ==
          NFC_FIDO_BRIDGE_INVALID_ARGUMENT);
    CHECK(nfc_fido_bridge_poll(NULL, 8u, reply, &reply_length) ==
          NFC_FIDO_BRIDGE_INVALID_ARGUMENT);
    CHECK(nfc_fido_bridge_poll(empty_poll, 8u, NULL, &reply_length) ==
          NFC_FIDO_BRIDGE_INVALID_ARGUMENT);
    CHECK(nfc_fido_bridge_poll(empty_poll, 8u, reply, NULL) ==
          NFC_FIDO_BRIDGE_INVALID_ARGUMENT);
    CHECK(nfc_fido_bridge_reset(NULL, 1u, NULL, &reply_length) ==
          NFC_FIDO_BRIDGE_INVALID_ARGUMENT);
    CHECK(nfc_fido_bridge_reset(NULL, 0u, NULL, NULL) ==
          NFC_FIDO_BRIDGE_INVALID_ARGUMENT);
    CHECK(nfc_fido_bridge_is_active());
    CHECK(configure(false) == NFC_FIDO_BRIDGE_OK);
    CHECK(!nfc_fido_bridge_is_active() && get_device_mode() == DEVICE_MODE_READER);
    CHECK(configure(false) == NFC_FIDO_BRIDGE_OK);
    return 0;
}

static int test_snapshot_and_reconfiguration(void) {
    const device_mode_t modes[] = {DEVICE_MODE_READER, DEVICE_MODE_TAG, DEVICE_MODE_NONE};
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        event_t event, apdu_event;
        CHECK(prepare(modes[i]) == 0);
        CHECK(configure(true) == NFC_FIDO_BRIDGE_OK);
        CHECK(nfc_fido_bridge_is_active() && bridge_test_env.runtime_installed);
        CHECK(get_device_mode() == DEVICE_MODE_TAG && bridge_test_env.sense_enabled);
        CHECK(!bridge_test_env.ordinary_sense_enabled && bridge_test_env.sleep_stopped);
        CHECK(bridge_test_env.profile.size == NFC_TAG_14A_UID_DOUBLE_SIZE);
        CHECK(bridge_test_env.profile.uid[0] == 0x04u);
        CHECK(bridge_test_env.profile.atqa[0] == 0x44u &&
              bridge_test_env.profile.atqa[1] == 0u);
        CHECK((bridge_test_env.profile.sak[0] & 0x20u) != 0u);
        CHECK(bridge_test_env.profile.ats.length >= 2u &&
              bridge_test_env.profile.ats.data[0] == bridge_test_env.profile.ats.length);
        CHECK(bridge_test_env.callbacks.on_field_on == nfc_fido_bridge_on_field_on);
        CHECK(bridge_test_env.callbacks.on_field_off == nfc_fido_bridge_on_field_off);
        CHECK(bridge_test_env.callbacks.on_capdu == nfc_fido_bridge_on_capdu);
        bridge_test_env.callbacks.on_field_on();
        CHECK(poll(0u, 0u, &event) == NFC_FIDO_BRIDGE_OK);
        CHECK(bridge_test_env.callbacks.on_capdu(capdu, sizeof(capdu)));
        CHECK(poll(event.session, event.id, &apdu_event) == NFC_FIDO_BRIDGE_OK);
        CHECK(respond(apdu_event.session, apdu_event.id, rapdu, sizeof(rapdu)) ==
              NFC_FIDO_BRIDGE_OK);
        CHECK(bridge_test_env.rf_sending);
        CHECK(configure(true) == NFC_FIDO_BRIDGE_OK);
        CHECK(!bridge_test_env.rf_sending && bridge_test_env.install_count == 2u);
        CHECK(poll(0u, 0u, &event) == NFC_FIDO_BRIDGE_OK && event_none(&event));
        CHECK(respond(apdu_event.session, apdu_event.id, rapdu, sizeof(rapdu)) ==
              NFC_FIDO_BRIDGE_INVALID_STATE);
        CHECK(!nfc_fido_bridge_on_capdu(capdu, sizeof(capdu)));
        CHECK(nfc_fido_bridge_reset(NULL, 0u, NULL, &reply_length) == NFC_FIDO_BRIDGE_OK);
        CHECK(reply_length == 0u && !nfc_fido_bridge_is_active());
        CHECK(get_device_mode() == modes[i] && !bridge_test_env.runtime_installed);
        CHECK(bridge_test_env.ordinary_sense_enabled == (modes[i] == DEVICE_MODE_TAG));
        CHECK(!bridge_test_env.sleep_stopped &&
              bridge_test_env.sleep_delay == SLEEP_DELAY_MS_BUTTON_CLICK);
        CHECK(bridge_test_env.normal_reload_count == 0u);
        unsigned uninstall_count = bridge_test_env.uninstall_count;
        CHECK(nfc_fido_bridge_reset(NULL, 0u, NULL, &reply_length) == NFC_FIDO_BRIDGE_OK);
        CHECK(bridge_test_env.uninstall_count == uninstall_count);
        CHECK(configure(true) == NFC_FIDO_BRIDGE_OK);
        CHECK(configure(false) == NFC_FIDO_BRIDGE_OK);
        CHECK(get_device_mode() == modes[i]);
    }
    return 0;
}

static int test_install_failure_restores_mode(void) {
    CHECK(prepare(DEVICE_MODE_READER) == 0);
    bridge_test_env.allow_install = false;
    CHECK(configure(true) == NFC_FIDO_BRIDGE_INVALID_STATE);
    CHECK(!nfc_fido_bridge_is_active() && get_device_mode() == DEVICE_MODE_READER);
    CHECK(!bridge_test_env.runtime_installed && !bridge_test_env.sleep_stopped);
    bridge_test_env.allow_install = true;
    CHECK(configure(true) == NFC_FIDO_BRIDGE_OK);
    bridge_test_env.allow_install = false;
    CHECK(configure(true) == NFC_FIDO_BRIDGE_INVALID_STATE);
    CHECK(!nfc_fido_bridge_is_active() && get_device_mode() == DEVICE_MODE_READER);
    return 0;
}

static int test_field_order_and_response_gate(void) {
    event_t on, pending, repeated, next;
    const uint8_t different_rapdu[] = {0x6au, 0x82u};
    CHECK(prepare(DEVICE_MODE_TAG) == 0);
    CHECK(configure(true) == NFC_FIDO_BRIDGE_OK);
    CHECK(!nfc_fido_bridge_on_capdu(capdu, sizeof(capdu)));
    nfc_fido_bridge_on_field_on();
    CHECK(poll(0u, 0u, &on) == NFC_FIDO_BRIDGE_OK);
    CHECK(on.type == FIELD_ON && on.session != 0u && on.id != 0u && on.length == 0u);
    CHECK(nfc_fido_bridge_on_capdu(capdu, sizeof(capdu)));
    CHECK(!nfc_fido_bridge_on_capdu(capdu, sizeof(capdu)));
    CHECK(poll(0u, 0u, &repeated) == NFC_FIDO_BRIDGE_OK && event_equal(&on, &repeated));
    CHECK(poll(on.session + 1u, on.id, &repeated) == NFC_FIDO_BRIDGE_OK &&
          event_equal(&on, &repeated));
    CHECK(respond(on.session, on.id + 1u, rapdu, sizeof(rapdu)) ==
          NFC_FIDO_BRIDGE_INVALID_STATE);
    CHECK(bridge_test_env.submit_count == 0u);
    CHECK(poll(on.session, on.id, &pending) == NFC_FIDO_BRIDGE_OK);
    CHECK(pending.type == APDU && pending.session == on.session && pending.id > on.id);
    CHECK(pending.length == sizeof(capdu) && memcmp(pending.data, capdu, sizeof(capdu)) == 0);
    CHECK(poll(on.session, on.id, &repeated) == NFC_FIDO_BRIDGE_OK &&
          event_equal(&pending, &repeated));
    CHECK(poll(pending.session, pending.id, &repeated) == NFC_FIDO_BRIDGE_OK &&
          event_equal(&pending, &repeated));
    CHECK(respond(pending.session + 1u, pending.id, rapdu, sizeof(rapdu)) ==
          NFC_FIDO_BRIDGE_INVALID_STATE);
    CHECK(respond(pending.session, pending.id + 1u, rapdu, sizeof(rapdu)) ==
          NFC_FIDO_BRIDGE_INVALID_STATE);
    CHECK(respond(pending.session, pending.id, rapdu, sizeof(rapdu)) == NFC_FIDO_BRIDGE_OK);
    CHECK(bridge_test_env.submit_count == 1u);
    CHECK(nfc_fido_bridge_on_capdu(capdu, sizeof(capdu)));
    CHECK(!nfc_fido_bridge_on_capdu(capdu, sizeof(capdu)));
    CHECK(poll(0u, 0u, &repeated) == NFC_FIDO_BRIDGE_OK && event_equal(&pending, &repeated));
    CHECK(respond(pending.session, pending.id, rapdu, sizeof(rapdu)) == NFC_FIDO_BRIDGE_OK);
    CHECK(bridge_test_env.submit_count == 1u);
    CHECK(respond(pending.session, pending.id, different_rapdu, sizeof(different_rapdu)) ==
          NFC_FIDO_BRIDGE_INVALID_ARGUMENT);
    CHECK(poll(pending.session, pending.id, &next) == NFC_FIDO_BRIDGE_OK);
    CHECK(next.type == APDU && next.id > pending.id && next.session == pending.session);
    CHECK(respond(pending.session, pending.id, rapdu, sizeof(rapdu)) == NFC_FIDO_BRIDGE_OK);
    CHECK(bridge_test_env.submit_count == 1u);
    bridge_test_env.allow_submit = false;
    CHECK(respond(next.session, next.id, rapdu, sizeof(rapdu)) == NFC_FIDO_BRIDGE_INVALID_STATE);
    CHECK(poll(next.session, next.id, &repeated) == NFC_FIDO_BRIDGE_OK &&
          event_equal(&next, &repeated));
    CHECK(respond(pending.session, pending.id, rapdu, sizeof(rapdu)) ==
          NFC_FIDO_BRIDGE_OK);
    CHECK(bridge_test_env.submit_count == 1u);
    bridge_test_env.allow_submit = true;
    CHECK(respond(next.session, next.id, rapdu, sizeof(rapdu)) == NFC_FIDO_BRIDGE_OK);
    CHECK(bridge_test_env.submit_count == 2u);
    CHECK(respond(pending.session, pending.id, rapdu, sizeof(rapdu)) ==
          NFC_FIDO_BRIDGE_INVALID_STATE);
    CHECK(poll(next.session, next.id, &repeated) == NFC_FIDO_BRIDGE_OK && event_none(&repeated));
    CHECK(respond(next.session, next.id, rapdu, sizeof(rapdu)) == NFC_FIDO_BRIDGE_OK);
    CHECK(bridge_test_env.submit_count == 2u);
    return 0;
}

static int test_field_loss_priority_and_new_field(void) {
    event_t on, pending, off, repeated, next_on, next_apdu;
    CHECK(prepare(DEVICE_MODE_TAG) == 0);
    CHECK(configure(true) == NFC_FIDO_BRIDGE_OK);
    nfc_fido_bridge_on_field_on();
    CHECK(poll(0u, 0u, &on) == NFC_FIDO_BRIDGE_OK);
    CHECK(nfc_fido_bridge_on_capdu(capdu, sizeof(capdu)));
    CHECK(poll(on.session, on.id, &pending) == NFC_FIDO_BRIDGE_OK);
    CHECK(respond(pending.session, pending.id, rapdu, sizeof(rapdu)) == NFC_FIDO_BRIDGE_OK);
    unsigned abort_count = bridge_test_env.abort_count;
    nfc_fido_bridge_on_field_off();
    CHECK(bridge_test_env.abort_count == abort_count + 1u && !bridge_test_env.rf_sending);
    CHECK(!nfc_fido_bridge_on_capdu(capdu, sizeof(capdu)));
    CHECK(respond(pending.session, pending.id, rapdu, sizeof(rapdu)) ==
          NFC_FIDO_BRIDGE_INVALID_STATE);
    CHECK(poll(pending.session, pending.id, &off) == NFC_FIDO_BRIDGE_OK);
    CHECK(off.type == FIELD_OFF && off.session == on.session && off.id > pending.id &&
          off.length == 0u);
    nfc_fido_bridge_on_field_off();
    CHECK(poll(0u, 0u, &repeated) == NFC_FIDO_BRIDGE_OK && event_equal(&off, &repeated));
    nfc_fido_bridge_on_field_on();
    CHECK(nfc_fido_bridge_on_capdu(capdu, sizeof(capdu)));
    CHECK(poll(0u, 0u, &repeated) == NFC_FIDO_BRIDGE_OK && event_equal(&off, &repeated));
    CHECK(respond(pending.session, pending.id, rapdu, sizeof(rapdu)) ==
          NFC_FIDO_BRIDGE_INVALID_STATE);
    CHECK(poll(off.session, off.id, &next_on) == NFC_FIDO_BRIDGE_OK);
    CHECK(next_on.type == FIELD_ON && next_on.session != 0u && next_on.session != on.session);
    CHECK(poll(off.session, off.id, &repeated) == NFC_FIDO_BRIDGE_OK &&
          event_equal(&next_on, &repeated));
    CHECK(poll(next_on.session, next_on.id, &next_apdu) == NFC_FIDO_BRIDGE_OK);
    CHECK(next_apdu.type == APDU && next_apdu.session == next_on.session);
    CHECK(respond(next_apdu.session, next_apdu.id, rapdu, sizeof(rapdu)) == NFC_FIDO_BRIDGE_OK);
    CHECK(bridge_test_env.submit_count == 2u);
    nfc_fido_bridge_on_field_off();
    CHECK(poll(next_apdu.session, next_apdu.id, &off) == NFC_FIDO_BRIDGE_OK &&
          off.type == FIELD_OFF);
    CHECK(poll(off.session, off.id, &repeated) == NFC_FIDO_BRIDGE_OK && event_none(&repeated));
    nfc_fido_bridge_on_field_on();
    CHECK(poll(0u, 0u, &on) == NFC_FIDO_BRIDGE_OK && on.type == FIELD_ON);
    /* Once FIELD_ON has been returned, the host can observe the session even
     * if it cannot issue the follow-up POLL ACK before RF disappears. */
    nfc_fido_bridge_on_field_off();
    CHECK(poll(0u, 0u, &off) == NFC_FIDO_BRIDGE_OK && off.type == FIELD_OFF &&
          off.session == on.session);
    CHECK(poll(off.session, off.id, &repeated) == NFC_FIDO_BRIDGE_OK &&
          event_none(&repeated));

    nfc_fido_bridge_on_field_on();
    CHECK(poll(0u, 0u, &on) == NFC_FIDO_BRIDGE_OK && on.type == FIELD_ON);
    CHECK(poll(on.session, on.id, &repeated) == NFC_FIDO_BRIDGE_OK &&
          event_none(&repeated));
    nfc_fido_bridge_on_field_off();
    CHECK(poll(0u, 0u, &off) == NFC_FIDO_BRIDGE_OK && off.type == FIELD_OFF &&
          off.session == on.session);
    return 0;
}

static int test_sessions_emit_field_off(void) {
    event_t event, on, off;
    CHECK(prepare(DEVICE_MODE_TAG) == 0);
    CHECK(configure(true) == NFC_FIDO_BRIDGE_OK);

    /* A short ISO-DEP session is retained even when it ends before POLL. */
    nfc_fido_bridge_on_field_on();
    nfc_fido_bridge_on_field_off();
    CHECK(poll(0u, 0u, &on) == NFC_FIDO_BRIDGE_OK && on.type == FIELD_ON);
    CHECK(poll(on.session, on.id, &off) == NFC_FIDO_BRIDGE_OK &&
          off.type == FIELD_OFF && off.session == on.session);
    CHECK(poll(off.session, off.id, &event) == NFC_FIDO_BRIDGE_OK &&
          event_none(&event));

    for (unsigned i = 0u; i < 16u; i++) {
        nfc_fido_bridge_on_field_on();
        CHECK(poll(0u, 0u, &on) == NFC_FIDO_BRIDGE_OK && on.type == FIELD_ON);
        nfc_fido_bridge_on_field_off();
        CHECK(poll(0u, 0u, &off) == NFC_FIDO_BRIDGE_OK &&
              off.type == FIELD_OFF && off.session == on.session);
        CHECK(poll(off.session, off.id, &event) == NFC_FIDO_BRIDGE_OK &&
              event_none(&event));
    }

    nfc_fido_bridge_on_field_on();
    CHECK(poll(0u, 0u, &on) == NFC_FIDO_BRIDGE_OK && on.type == FIELD_ON);
    nfc_fido_bridge_on_field_off();
    CHECK(poll(0u, 0u, &off) == NFC_FIDO_BRIDGE_OK && off.type == FIELD_OFF &&
          off.session == on.session);
    CHECK(poll(off.session, off.id, &event) == NFC_FIDO_BRIDGE_OK &&
          event_none(&event));

    nfc_fido_bridge_on_field_on();
    CHECK(poll(0u, 0u, &on) == NFC_FIDO_BRIDGE_OK && on.type == FIELD_ON);
    CHECK(poll(on.session, on.id, &event) == NFC_FIDO_BRIDGE_OK && event_none(&event));
    nfc_fido_bridge_on_field_off();
    CHECK(poll(0u, 0u, &off) == NFC_FIDO_BRIDGE_OK && off.type == FIELD_OFF);
    CHECK(off.session == on.session && off.id > on.id);
    CHECK(poll(off.session, off.id, &event) == NFC_FIDO_BRIDGE_OK && event_none(&event));

    nfc_fido_bridge_on_field_on();
    CHECK(nfc_fido_bridge_on_capdu(capdu, sizeof(capdu)));
    CHECK(poll(0u, 0u, &on) == NFC_FIDO_BRIDGE_OK && on.type == FIELD_ON);
    CHECK(poll(on.session, on.id, &event) == NFC_FIDO_BRIDGE_OK && event.type == APDU);
    CHECK(respond(event.session, event.id, rapdu, sizeof(rapdu)) == NFC_FIDO_BRIDGE_OK);
    return 0;
}

static int test_transparent_fido_and_status_bytes(void) {
    static const uint8_t unrelated_aid[] = {0u, 0xa4u, 4u, 0u, 1u, 0xffu};
    static const uint8_t cancel[] = {0x80u, 0x11u, 0x11u, 0u, 0u};
    static const uint8_t response_61[] = {0x42u, 0x61u, 0x80u};
    static const uint8_t response_91[] = {0x01u, 0x91u, 0u};
    static const uint8_t response_cancel[] = {0x2du, 0x90u, 0u};
    const uint8_t *commands[] = {unrelated_aid, capdu, cancel};
    const uint8_t *responses[] = {response_61, response_91, response_cancel};
    const uint16_t lengths[] = {sizeof(unrelated_aid), sizeof(capdu), sizeof(cancel)};
    event_t event, next;
    CHECK(prepare(DEVICE_MODE_TAG) == 0);
    CHECK(configure(true) == NFC_FIDO_BRIDGE_OK);
    nfc_fido_bridge_on_field_on();
    CHECK(poll(0u, 0u, &event) == NFC_FIDO_BRIDGE_OK);
    CHECK(poll(event.session, event.id, &next) == NFC_FIDO_BRIDGE_OK && event_none(&next));
    for (unsigned i = 0u; i < 3u; i++) {
        CHECK(nfc_fido_bridge_on_capdu(commands[i], lengths[i]));
        CHECK(poll(0u, 0u, &event) == NFC_FIDO_BRIDGE_OK && event.type == APDU);
        CHECK(event.length == lengths[i] && memcmp(event.data, commands[i], lengths[i]) == 0);
        CHECK(respond(event.session, event.id, responses[i], 3u) == NFC_FIDO_BRIDGE_OK);
        CHECK(bridge_test_env.rapdu_length == 3u &&
              memcmp(bridge_test_env.rapdu, responses[i], 3u) == 0);
        CHECK(poll(event.session, event.id, &next) == NFC_FIDO_BRIDGE_OK && event_none(&next));
    }
    CHECK(bridge_test_env.submit_count == 3u);
    return 0;
}

static int test_apdu_limits(void) {
    static uint8_t command[NFC_FIDO_BRIDGE_MAX_CAPDU + 1u];
    static uint8_t response[NFC_FIDO_BRIDGE_MAX_RAPDU + 1u];
    event_t on, event;
    CHECK(prepare(DEVICE_MODE_TAG) == 0);
    CHECK(configure(true) == NFC_FIDO_BRIDGE_OK);
    nfc_fido_bridge_on_field_on();
    CHECK(poll(0u, 0u, &on) == NFC_FIDO_BRIDGE_OK);
    for (unsigned i = 0; i < sizeof(command); i++) command[i] = (uint8_t)(i * 7u);
    for (unsigned i = 0; i < sizeof(response); i++) response[i] = (uint8_t)(i * 11u);
    response[NFC_FIDO_BRIDGE_MAX_RAPDU - 2u] = 0x90u;
    response[NFC_FIDO_BRIDGE_MAX_RAPDU - 1u] = 0u;
    CHECK(!nfc_fido_bridge_on_capdu(NULL, 1u));
    CHECK(!nfc_fido_bridge_on_capdu(command, 0u));
    CHECK(!nfc_fido_bridge_on_capdu(command, sizeof(command)));
    CHECK(nfc_fido_bridge_on_capdu(command, NFC_FIDO_BRIDGE_MAX_CAPDU));
    CHECK(poll(on.session, on.id, &event) == NFC_FIDO_BRIDGE_OK);
    CHECK(event.length == NFC_FIDO_BRIDGE_MAX_CAPDU &&
          memcmp(event.data, command, event.length) == 0);
    CHECK(respond(event.session, event.id, response, 0u) == NFC_FIDO_BRIDGE_INVALID_ARGUMENT);
    CHECK(respond(event.session, event.id, response, 1u) == NFC_FIDO_BRIDGE_INVALID_ARGUMENT);
    CHECK(respond(event.session, event.id, response, sizeof(response)) ==
          NFC_FIDO_BRIDGE_INVALID_ARGUMENT);
    CHECK(nfc_fido_bridge_response(NULL, 10u, response_ack, &response_ack_length) ==
          NFC_FIDO_BRIDGE_INVALID_ARGUMENT);
    CHECK(nfc_fido_bridge_response(response_request, 10u, NULL, &response_ack_length) ==
          NFC_FIDO_BRIDGE_INVALID_ARGUMENT);
    CHECK(nfc_fido_bridge_response(response_request, 10u, response_ack, NULL) ==
          NFC_FIDO_BRIDGE_INVALID_ARGUMENT);
    CHECK(bridge_test_env.submit_count == 0u);
    CHECK(respond(event.session, event.id, response, NFC_FIDO_BRIDGE_MAX_RAPDU) ==
          NFC_FIDO_BRIDGE_OK);
    CHECK(bridge_test_env.rapdu_length == NFC_FIDO_BRIDGE_MAX_RAPDU &&
          memcmp(bridge_test_env.rapdu, response, NFC_FIDO_BRIDGE_MAX_RAPDU) == 0);
    CHECK(!bridge_test_env.submit_in_critical &&
          !bridge_test_env.bulk_memory_in_critical);
    CHECK(respond(event.session, event.id, response, NFC_FIDO_BRIDGE_MAX_RAPDU) ==
          NFC_FIDO_BRIDGE_OK);
    CHECK(bridge_test_env.submit_count == 1u);
    CHECK(!bridge_test_env.bulk_memory_in_critical);
    return 0;
}

static int test_field_loss_during_response_submit(void) {
    static uint8_t response[NFC_FIDO_BRIDGE_MAX_RAPDU];
    event_t on, apdu_event, off, next_on, next_apdu;
    CHECK(prepare(DEVICE_MODE_TAG) == 0);
    CHECK(configure(true) == NFC_FIDO_BRIDGE_OK);
    nfc_fido_bridge_on_field_on();
    CHECK(poll(0u, 0u, &on) == NFC_FIDO_BRIDGE_OK && on.type == FIELD_ON);
    CHECK(nfc_fido_bridge_on_capdu(capdu, sizeof(capdu)));
    CHECK(poll(on.session, on.id, &apdu_event) == NFC_FIDO_BRIDGE_OK &&
          apdu_event.type == APDU);

    for (unsigned i = 0u; i < sizeof(response); i++) {
        response[i] = (uint8_t)(i * 13u);
    }
    response[sizeof(response) - 2u] = 0x90u;
    response[sizeof(response) - 1u] = 0u;
    bridge_test_env.field_off_during_submit = true;
    bridge_test_env.field_on_after_submit = true;
    CHECK(respond(apdu_event.session, apdu_event.id, response,
                  sizeof(response)) == NFC_FIDO_BRIDGE_INVALID_STATE);
    CHECK(bridge_test_env.submit_count == 1u &&
          !bridge_test_env.submit_in_critical &&
          !bridge_test_env.bulk_memory_in_critical);
    CHECK(poll(0u, 0u, &off) == NFC_FIDO_BRIDGE_OK &&
          off.type == FIELD_OFF && off.session == on.session);
    CHECK(poll(off.session, off.id, &next_on) == NFC_FIDO_BRIDGE_OK &&
          next_on.type == FIELD_ON && next_on.session != on.session);

    CHECK(nfc_fido_bridge_on_capdu(capdu, sizeof(capdu)));
    CHECK(poll(next_on.session, next_on.id, &next_apdu) ==
          NFC_FIDO_BRIDGE_OK && next_apdu.type == APDU);
    CHECK(respond(next_apdu.session, next_apdu.id, rapdu,
                  sizeof(rapdu)) == NFC_FIDO_BRIDGE_OK);
    CHECK(bridge_test_env.submit_count == 2u);
    return 0;
}

static int test_watchdog_restores_mode(void) {
    event_t on, event;
    uint8_t malformed_poll[7] = {0};
    CHECK(prepare(DEVICE_MODE_READER) == 0);
    CHECK(configure(true) == NFC_FIDO_BRIDGE_OK);
    bridge_test_env.now = 4000u;
    CHECK(poll(0u, 0u, &event) == NFC_FIDO_BRIDGE_OK && event_none(&event));
    bridge_test_env.now = 8000u;
    nfc_fido_bridge_process();
    CHECK(nfc_fido_bridge_is_active());
    bridge_test_env.now = 8999u;
    CHECK(nfc_fido_bridge_poll(malformed_poll, sizeof(malformed_poll), reply, &reply_length) ==
          NFC_FIDO_BRIDGE_INVALID_ARGUMENT);
    bridge_test_env.now = 9001u;
    nfc_fido_bridge_process();
    CHECK(!nfc_fido_bridge_is_active() && get_device_mode() == DEVICE_MODE_READER);
    CHECK(!bridge_test_env.runtime_installed && !bridge_test_env.rf_sending);
    CHECK(poll(0u, 0u, &event) == NFC_FIDO_BRIDGE_INVALID_STATE);

    CHECK(prepare(DEVICE_MODE_TAG) == 0);
    CHECK(configure(true) == NFC_FIDO_BRIDGE_OK);
    nfc_fido_bridge_on_field_on();
    CHECK(poll(0u, 0u, &on) == NFC_FIDO_BRIDGE_OK);
    CHECK(nfc_fido_bridge_on_capdu(capdu, sizeof(capdu)));
    CHECK(poll(on.session, on.id, &event) == NFC_FIDO_BRIDGE_OK);
    bridge_test_env.now = 35000u;
    nfc_fido_bridge_process();
    CHECK(nfc_fido_bridge_is_active());
    CHECK(respond(event.session, event.id, rapdu, sizeof(rapdu)) == NFC_FIDO_BRIDGE_OK);
    bridge_test_env.now = 40001u;
    nfc_fido_bridge_process();
    CHECK(!nfc_fido_bridge_is_active() && get_device_mode() == DEVICE_MODE_TAG);
    CHECK(bridge_test_env.ordinary_sense_enabled && !bridge_test_env.rf_sending);
    CHECK(respond(event.session, event.id, rapdu, sizeof(rapdu)) ==
          NFC_FIDO_BRIDGE_INVALID_STATE);

    CHECK(prepare(DEVICE_MODE_NONE) == 0);
    bridge_test_env.now = 0xffff00u;
    CHECK(configure(true) == NFC_FIDO_BRIDGE_OK);
    bridge_test_env.now += 4000u;
    nfc_fido_bridge_process();
    CHECK(nfc_fido_bridge_is_active());
    bridge_test_env.now += 2000u;
    nfc_fido_bridge_process();
    CHECK(!nfc_fido_bridge_is_active() && get_device_mode() == DEVICE_MODE_NONE);
    return 0;
}

int test_nfc_fido_bridge(void) {
    int (*tests[])(void) = {
        test_configuration_validation,
        test_snapshot_and_reconfiguration,
        test_install_failure_restores_mode,
        test_field_order_and_response_gate,
        test_field_loss_priority_and_new_field,
        test_sessions_emit_field_off,
        test_transparent_fido_and_status_bytes,
        test_apdu_limits,
        test_field_loss_during_response_submit,
        test_watchdog_restores_mode,
    };
    for (unsigned i = 0u; i < sizeof(tests) / sizeof(tests[0]); i++) {
        bridge_test_case = i + 1u;
        int result = tests[i]();
        if (result != 0) return result;
        CHECK(bridge_test_env.critical_depth == 0u && !bridge_test_env.critical_fault);
    }
    CHECK(prepare(DEVICE_MODE_NONE) == 0);
    return 0;
}

int main(void) {
    return test_nfc_fido_bridge();
}
