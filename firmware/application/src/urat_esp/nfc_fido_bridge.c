#include "nfc_fido_bridge.h"

#include <string.h>

#include "app_timer.h"
#include "app_util_platform.h"
#include "nfc_14a.h"
#include "nfc_14a_4.h"
#include "nrf.h"
#include "rfid_main.h"
#include "syssleep.h"
#include "tag_emulation.h"

#define NRF_LOG_MODULE_NAME nfc_fido_bridge
#include "nrf_log.h"
NRF_LOG_MODULE_REGISTER();

#define BRIDGE_EVENT_NONE       0u
#define BRIDGE_EVENT_FIELD_ON   1u
#define BRIDGE_EVENT_FIELD_OFF  2u
#define BRIDGE_EVENT_APDU       3u

#define BRIDGE_RESPONSE_OPERATION_NONE     0u
#define BRIDGE_RESPONSE_OPERATION_COMPARE  1u
#define BRIDGE_RESPONSE_OPERATION_LOAD     2u

#define BRIDGE_EVENT_QUEUE_SIZE       8u
#define BRIDGE_IDLE_WATCHDOG_MS       5000u
#define BRIDGE_APDU_WATCHDOG_MS       40000u
#define BRIDGE_CAPDU_LOG_PREVIEW      16u

_Static_assert(NFC_FIDO_BRIDGE_MAX_CAPDU == NFC_14A_4_MAX_CAPDU,
               "Bridge and ISO-DEP CAPDU limits must match");
_Static_assert(NFC_FIDO_BRIDGE_MAX_RAPDU == NFC_14A_4_MAX_RAPDU,
               "Bridge and ISO-DEP RAPDU limits must match");

typedef struct {
    uint8_t type;
    uint32_t session;
    uint32_t id;
    uint16_t data_length;
    bool response_accepted;
    uint8_t data[NFC_FIDO_BRIDGE_MAX_CAPDU];
} bridge_event_t;

typedef struct {
    bool valid;
    uint8_t operation;
    uint32_t session;
    uint32_t event_id;
    uint16_t length;
    uint32_t operation_token;
    uint8_t data[NFC_FIDO_BRIDGE_MAX_RAPDU];
} bridge_response_t;

static const uint8_t m_config_enable[] = {
    NFC_FIDO_BRIDGE_PROTOCOL_VERSION, 1u,
    (uint8_t)(NFC_FIDO_BRIDGE_MAX_CAPDU >> 8),
    (uint8_t)NFC_FIDO_BRIDGE_MAX_CAPDU,
    (uint8_t)(NFC_FIDO_BRIDGE_MAX_RAPDU >> 8),
    (uint8_t)NFC_FIDO_BRIDGE_MAX_RAPDU,
};
static const uint8_t m_config_disable[] = {
    NFC_FIDO_BRIDGE_PROTOCOL_VERSION, 0u,
    (uint8_t)(NFC_FIDO_BRIDGE_MAX_CAPDU >> 8),
    (uint8_t)NFC_FIDO_BRIDGE_MAX_CAPDU,
    (uint8_t)(NFC_FIDO_BRIDGE_MAX_RAPDU >> 8),
    (uint8_t)NFC_FIDO_BRIDGE_MAX_RAPDU,
};

static volatile bool m_active;
static bool m_snapshot_valid;
static device_mode_t m_snapshot_mode;
static bool m_field_present;
static bool m_field_active;
/* FIELD_OFF is emitted only after FIELD_ON was returned to the host. */
static bool m_session_reported;
static uint32_t m_current_session;
static uint32_t m_next_event_id;
static uint32_t m_session_sequence;
static bool m_session_sequence_seeded;
static uint32_t m_last_poll_tick;
static bridge_event_t m_events[BRIDGE_EVENT_QUEUE_SIZE];
static uint8_t m_event_count;
static bridge_response_t m_response;
static uint32_t m_next_response_operation_token;

static void log_capdu_preview(uint32_t session, uint32_t event_id,
                              const uint8_t *data, uint16_t length) {
    uint8_t preview[BRIDGE_CAPDU_LOG_PREVIEW] = {0};
    uint16_t preview_length = length < BRIDGE_CAPDU_LOG_PREVIEW ?
                              length : BRIDGE_CAPDU_LOG_PREVIEW;

    (void)session;
    (void)event_id;
    memcpy(preview, data, preview_length);
    NRF_LOG_INFO("NFC FIDO CAPDU: session=%lu event=%lu len=%u",
                 (unsigned long)session, (unsigned long)event_id,
                 (unsigned int)length);
    NRF_LOG_INFO("NFC FIDO CAPDU bytes[0..3]=%02x %02x %02x %02x",
                 preview[0], preview[1], preview[2], preview[3]);
    NRF_LOG_INFO("NFC FIDO CAPDU bytes[4..7]=%02x %02x %02x %02x",
                 preview[4], preview[5], preview[6], preview[7]);
    if (preview_length > 8u) {
        NRF_LOG_INFO("NFC FIDO CAPDU bytes[8..11]=%02x %02x %02x %02x",
                     preview[8], preview[9], preview[10], preview[11]);
        NRF_LOG_INFO("NFC FIDO CAPDU bytes[12..15]=%02x %02x %02x %02x",
                     preview[12], preview[13], preview[14], preview[15]);
    }
}

static uint32_t read_be32(const uint8_t *data) {
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

static void write_be32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static uint32_t new_session_id(void) {
    if (!m_session_sequence_seeded) {
        m_session_sequence = NRF_FICR->DEVICEID[0] ^
                             NRF_FICR->DEVICEID[1] ^ app_timer_cnt_get();
        m_session_sequence_seeded = true;
    }
    m_session_sequence++;
    if (m_session_sequence == 0u) m_session_sequence = 1u;
    return m_session_sequence;
}

static uint32_t allocate_event_id(void) {
    return ++m_next_event_id;
}

static void invalidate_response_locked(void) {
    m_response.valid = false;
    m_response.session = 0u;
    m_response.event_id = 0u;
    m_response.length = 0u;
}

static uint32_t reserve_response_operation_locked(uint8_t operation) {
    m_next_response_operation_token++;
    if (m_next_response_operation_token == 0u) {
        m_next_response_operation_token = 1u;
    }
    m_response.operation = operation;
    m_response.operation_token = m_next_response_operation_token;
    return m_response.operation_token;
}

static bool response_operation_matches_locked(uint8_t operation,
                                              uint32_t token) {
    return m_response.operation == operation &&
           m_response.operation_token == token;
}

static void finish_response_operation_locked(uint8_t operation,
                                             uint32_t token) {
    if (response_operation_matches_locked(operation, token)) {
        m_response.operation = BRIDGE_RESPONSE_OPERATION_NONE;
        m_response.operation_token = 0u;
    }
}

static void clear_protocol_state_locked(void) {
    m_field_present = false;
    m_field_active = false;
    m_session_reported = false;
    m_current_session = 0u;
    m_next_event_id = 0u;
    m_event_count = 0u;
    invalidate_response_locked();
}

static bool queue_append_locked(uint8_t type, uint32_t session, uint32_t id,
                                const uint8_t *data, uint16_t data_length) {
    if (m_event_count >= BRIDGE_EVENT_QUEUE_SIZE ||
        data_length > NFC_FIDO_BRIDGE_MAX_CAPDU ||
        (data_length != 0u && data == NULL)) {
        return false;
    }

    bridge_event_t *event = &m_events[m_event_count++];
    event->type = type;
    event->session = session;
    event->id = id;
    event->data_length = data_length;
    event->response_accepted = false;
    if (data_length != 0u) memcpy(event->data, data, data_length);
    return true;
}

static void queue_pop_locked(void) {
    if (m_event_count == 0u) return;
    m_event_count--;
    if (m_event_count != 0u) {
        memmove(&m_events[0], &m_events[1],
                (size_t)m_event_count * sizeof(m_events[0]));
    }
}

static void queue_remove_session_apdus_locked(uint32_t session) {
    uint8_t write_index = 0u;
    for (uint8_t read_index = 0u; read_index < m_event_count; read_index++) {
        if (m_events[read_index].session != session ||
            m_events[read_index].type != BRIDGE_EVENT_APDU) {
            if (write_index != read_index) m_events[write_index] = m_events[read_index];
            write_index++;
        }
    }
    m_event_count = write_index;
}

static void queue_remove_session_locked(uint32_t session) {
    uint8_t write_index = 0u;
    for (uint8_t read_index = 0u; read_index < m_event_count; read_index++) {
        if (m_events[read_index].session != session) {
            if (write_index != read_index) m_events[write_index] = m_events[read_index];
            write_index++;
        }
    }
    m_event_count = write_index;
}

static bool queue_has_apdu_locked(void) {
    for (uint8_t i = 0u; i < m_event_count; i++) {
        if (m_events[i].type == BRIDGE_EVENT_APDU) return true;
    }
    return false;
}

static bool queue_has_unanswered_apdu_locked(void) {
    for (uint8_t i = 0u; i < m_event_count; i++) {
        if (m_events[i].type == BRIDGE_EVENT_APDU &&
            !m_events[i].response_accepted) return true;
    }
    return false;
}

static void start_session_locked(void) {
    // Reserve one slot for FIELD_OFF before admitting the ISO-DEP session.
    if (!m_field_present || m_field_active ||
        m_event_count >= BRIDGE_EVENT_QUEUE_SIZE - 1u) {
        return;
    }
    m_current_session = new_session_id();
    m_next_event_id = 0u;
    m_field_active = true;
    m_session_reported = false;
    invalidate_response_locked();
    uint32_t event_id = allocate_event_id();
    (void)queue_append_locked(BRIDGE_EVENT_FIELD_ON, m_current_session,
                              event_id, NULL, 0u);
}

static void end_session_locked(void) {
    if (!m_field_active) return;

    uint32_t ended_session = m_current_session;
    uint32_t event_id = allocate_event_id();
    /* FIELD_ON now means a valid RATS was received. Preserve an unreported
     * boundary so ESP32 can diagnose a short ISO-DEP activation. Once it has
     * already been returned, replace the unacknowledged event with FIELD_OFF. */
    if (m_session_reported) {
        queue_remove_session_locked(ended_session);
    } else {
        queue_remove_session_apdus_locked(ended_session);
    }
    invalidate_response_locked();
    m_field_active = false;
    m_session_reported = false;
    m_current_session = 0u;
    (void)queue_append_locked(BRIDGE_EVENT_FIELD_OFF, ended_session,
                              event_id, NULL, 0u);
}

static void make_runtime_profile(nfc_tag_14a_coll_res_entity_t *profile) {
    static const uint8_t ats[] = {0x05u, 0x78u, 0x80u, 0x80u, 0x02u};
    uint64_t device_id = ((uint64_t)NRF_FICR->DEVICEID[1] << 32) |
                         NRF_FICR->DEVICEID[0];

    memset(profile, 0, sizeof(*profile));
    profile->size = NFC_TAG_14A_UID_DOUBLE_SIZE;
    profile->atqa[0] = 0x44u;
    profile->atqa[1] = 0x00u;
    profile->sak[0] = 0x20u;
    profile->uid[0] = 0x04u;
    for (uint8_t i = 1u; i < 7u; i++) {
        profile->uid[i] = (uint8_t)(device_id >> ((i - 1u) * 8u));
    }
    profile->ats.length = sizeof(ats);
    memcpy(profile->ats.data, ats, sizeof(ats));
}

static void restore_normal_mode(void) {
    device_mode_t mode = m_snapshot_mode;
    bool restore = m_snapshot_valid;

    m_active = false;
    nfc_tag_14a_sense_switch(false);
    nfc_tag_14a_4_abort();
    nfc_tag_14a_4_runtime_uninstall();

    CRITICAL_REGION_ENTER();
    clear_protocol_state_locked();
    CRITICAL_REGION_EXIT();

    if (restore) {
        if (mode == DEVICE_MODE_READER) {
            reader_mode_enter();
        } else if (mode == DEVICE_MODE_TAG) {
            tag_mode_enter();
            tag_emulation_sense_run();
        } else {
            device_mode_exit();
        }
    }
    m_snapshot_valid = false;
    sleep_timer_start(SLEEP_DELAY_MS_BUTTON_CLICK);
}

static bool activate_bridge(void) {
    const bool first_activation = !m_active;
    nfc_tag_14a_coll_res_entity_t profile;
    const nfc_tag_14a_4_runtime_callbacks_t callbacks = {
        .on_field_on = nfc_fido_bridge_on_field_on,
        .on_field_off = nfc_fido_bridge_on_field_off,
        .on_capdu = nfc_fido_bridge_on_capdu,
    };

    if (first_activation) {
        m_snapshot_mode = get_device_mode();
        m_snapshot_valid = true;
    }

    m_active = false;
    tag_mode_enter();
    tag_emulation_sense_end();
    nfc_tag_14a_4_abort();

    CRITICAL_REGION_ENTER();
    clear_protocol_state_locked();
    m_last_poll_tick = app_timer_cnt_get();
    CRITICAL_REGION_EXIT();

    make_runtime_profile(&profile);
    if (!nfc_tag_14a_4_runtime_install(&profile, &callbacks)) {
        restore_normal_mode();
        return false;
    }

    sleep_timer_stop();
    m_active = true;
    nfc_tag_14a_sense_switch(true);
    return true;
}

nfc_fido_bridge_result_t nfc_fido_bridge_configure(
    const uint8_t *request,
    uint16_t request_length,
    uint8_t *response,
    uint16_t *response_length) {
    if (request == NULL || response == NULL || response_length == NULL ||
        request_length != sizeof(m_config_enable)) {
        return NFC_FIDO_BRIDGE_INVALID_ARGUMENT;
    }

    if (memcmp(request, m_config_disable, sizeof(m_config_disable)) == 0) {
        if (m_active || m_snapshot_valid) restore_normal_mode();
        memcpy(response, request, request_length);
        *response_length = request_length;
        return NFC_FIDO_BRIDGE_OK;
    }
    if (memcmp(request, m_config_enable, sizeof(m_config_enable)) != 0) {
        return NFC_FIDO_BRIDGE_UNSUPPORTED;
    }
    if (!activate_bridge()) return NFC_FIDO_BRIDGE_INVALID_STATE;

    memcpy(response, request, request_length);
    *response_length = request_length;
    return NFC_FIDO_BRIDGE_OK;
}

nfc_fido_bridge_result_t nfc_fido_bridge_poll(
    const uint8_t *request,
    uint16_t request_length,
    uint8_t *response,
    uint16_t *response_length) {
    if (request == NULL || response == NULL || response_length == NULL ||
        request_length != 8u) {
        return NFC_FIDO_BRIDGE_INVALID_ARGUMENT;
    }
    if (!m_active) return NFC_FIDO_BRIDGE_INVALID_STATE;

    uint32_t ack_session = read_be32(request);
    uint32_t ack_event = read_be32(request + 4);
    bool acked = false;
    uint8_t returned_type = BRIDGE_EVENT_NONE;
    uint32_t returned_session = 0u;
    uint32_t returned_event = 0u;

    CRITICAL_REGION_ENTER();
    m_last_poll_tick = app_timer_cnt_get();
    if (m_event_count != 0u &&
        m_events[0].session == ack_session && m_events[0].id == ack_event &&
        (m_events[0].type != BRIDGE_EVENT_APDU || m_events[0].response_accepted)) {
        acked = true;
        queue_pop_locked();
    }
    start_session_locked();

    if (m_event_count == 0u) {
        memset(response, 0, NFC_FIDO_BRIDGE_EVENT_HEADER_SIZE);
        *response_length = NFC_FIDO_BRIDGE_EVENT_HEADER_SIZE;
    } else {
        const bridge_event_t *event = &m_events[0];
        returned_type = event->type;
        returned_session = event->session;
        returned_event = event->id;
        if (event->type == BRIDGE_EVENT_FIELD_ON &&
            event->session == m_current_session && m_field_active) {
            m_session_reported = true;
        }
        response[0] = event->type;
        write_be32(response + 1, event->session);
        write_be32(response + 5, event->id);
        if (event->data_length != 0u) {
            memcpy(response + NFC_FIDO_BRIDGE_EVENT_HEADER_SIZE,
                   event->data, event->data_length);
        }
        *response_length = NFC_FIDO_BRIDGE_EVENT_HEADER_SIZE + event->data_length;
    }
    CRITICAL_REGION_EXIT();
    (void)returned_session;
    (void)returned_event;
    if (acked || returned_type != BRIDGE_EVENT_NONE) {
        NRF_LOG_INFO("NFC FIDO POLL: ack=%u session=%lu event=%lu return=%u session=%lu event=%lu",
                     acked ? 1u : 0u,
                     (unsigned long)ack_session, (unsigned long)ack_event,
                     returned_type,
                     (unsigned long)returned_session,
                     (unsigned long)returned_event);
    }
    return NFC_FIDO_BRIDGE_OK;
}

nfc_fido_bridge_result_t nfc_fido_bridge_response(
    const uint8_t *request,
    uint16_t request_length,
    uint8_t *response,
    uint16_t *response_length) {
    if (request == NULL || response == NULL || response_length == NULL ||
        request_length < 10u || request_length > 8u + NFC_FIDO_BRIDGE_MAX_RAPDU) {
        return NFC_FIDO_BRIDGE_INVALID_ARGUMENT;
    }
    if (!m_active) return NFC_FIDO_BRIDGE_INVALID_STATE;

    uint32_t session = read_be32(request);
    uint32_t event_id = read_be32(request + 4);
    uint16_t rapdu_length = request_length - 8u;
    nfc_fido_bridge_result_t result = NFC_FIDO_BRIDGE_INVALID_STATE;
    uint8_t operation = BRIDGE_RESPONSE_OPERATION_NONE;
    uint32_t operation_token = 0u;

    CRITICAL_REGION_ENTER();
    if (!m_active || !m_field_active || session != m_current_session) goto done;

    /* Keep the response cache immutable while a large comparison or copy is
     * running outside the interrupt-masking critical section. */
    if (m_response.operation != BRIDGE_RESPONSE_OPERATION_NONE) goto done;

    if (m_response.valid && m_response.session == session &&
        m_response.event_id == event_id) {
        if (m_response.length != rapdu_length) {
            result = NFC_FIDO_BRIDGE_INVALID_ARGUMENT;
            goto done;
        }
        operation = BRIDGE_RESPONSE_OPERATION_COMPARE;
        operation_token = reserve_response_operation_locked(operation);
        goto done;
    }

    if (m_event_count == 0u) goto done;
    bridge_event_t *event = &m_events[0];
    if (event->type != BRIDGE_EVENT_APDU || event->session != session ||
        event->id != event_id || event->response_accepted) goto done;
    operation = BRIDGE_RESPONSE_OPERATION_LOAD;
    operation_token = reserve_response_operation_locked(operation);

done:
    CRITICAL_REGION_EXIT();

    if (operation == BRIDGE_RESPONSE_OPERATION_COMPARE) {
        bool response_matches =
            memcmp(m_response.data, request + 8, rapdu_length) == 0;

        CRITICAL_REGION_ENTER();
        if (response_operation_matches_locked(operation, operation_token) &&
            m_active && m_field_active && session == m_current_session &&
            m_response.valid && m_response.session == session &&
            m_response.event_id == event_id &&
            m_response.length == rapdu_length) {
            result = response_matches ? NFC_FIDO_BRIDGE_OK :
                                        NFC_FIDO_BRIDGE_INVALID_ARGUMENT;
        }
        finish_response_operation_locked(operation, operation_token);
        CRITICAL_REGION_EXIT();
    } else if (operation == BRIDGE_RESPONSE_OPERATION_LOAD) {
        bool submitted =
            nfc_tag_14a_4_submit_response(request + 8, rapdu_length);
        if (submitted) {
            memcpy(m_response.data, request + 8, rapdu_length);
        }

        CRITICAL_REGION_ENTER();
        if (response_operation_matches_locked(operation, operation_token)) {
            if (submitted && m_active && m_field_active &&
                session == m_current_session && m_event_count != 0u) {
                bridge_event_t *event = &m_events[0];
                if (event->type == BRIDGE_EVENT_APDU &&
                    event->session == session && event->id == event_id &&
                    !event->response_accepted) {
                    m_response.valid = true;
                    m_response.session = session;
                    m_response.event_id = event_id;
                    m_response.length = rapdu_length;
                    event->response_accepted = true;
                    result = NFC_FIDO_BRIDGE_OK;
                }
            }
            finish_response_operation_locked(operation, operation_token);
        }
        CRITICAL_REGION_EXIT();
    }

    if (result == NFC_FIDO_BRIDGE_OK) {
        write_be32(response, session);
        write_be32(response + 4, event_id);
        *response_length = 8u;
    }
    return result;
}

nfc_fido_bridge_result_t nfc_fido_bridge_reset(
    const uint8_t *request,
    uint16_t request_length,
    uint8_t *response,
    uint16_t *response_length) {
    (void)request;
    (void)response;
    if (request_length != 0u || response_length == NULL) {
        return NFC_FIDO_BRIDGE_INVALID_ARGUMENT;
    }
    if (m_active || m_snapshot_valid) restore_normal_mode();
    *response_length = 0u;
    return NFC_FIDO_BRIDGE_OK;
}

void nfc_fido_bridge_process(void) {
    if (!m_active) return;

    uint32_t now = app_timer_cnt_get();
    uint32_t last_poll;
    bool apdu_pending;

    CRITICAL_REGION_ENTER();
    last_poll = m_last_poll_tick;
    apdu_pending = queue_has_apdu_locked();
    CRITICAL_REGION_EXIT();

    uint32_t timeout_ms = apdu_pending ? BRIDGE_APDU_WATCHDOG_MS :
                                         BRIDGE_IDLE_WATCHDOG_MS;
    if (app_timer_cnt_diff_compute(now, last_poll) >= APP_TIMER_TICKS(timeout_ms)) {
        NRF_LOG_WARNING("NFC FIDO poll watchdog expired");
        restore_normal_mode();
    }
}

bool nfc_fido_bridge_is_active(void) {
    return m_active;
}

void nfc_fido_bridge_on_field_on(void) {
    if (!m_active) return;

    CRITICAL_REGION_ENTER();
    if (m_active) {
        m_field_present = true;
        start_session_locked();
    }
    CRITICAL_REGION_EXIT();
}

void nfc_fido_bridge_on_field_off(void) {
    if (!m_active) return;

    CRITICAL_REGION_ENTER();
    if (m_active) {
        m_field_present = false;
        end_session_locked();
    }
    CRITICAL_REGION_EXIT();

    nfc_tag_14a_4_abort();
}

bool nfc_fido_bridge_on_capdu(const uint8_t *capdu, uint16_t length) {
    if (!m_active || capdu == NULL || length == 0u ||
        length > NFC_FIDO_BRIDGE_MAX_CAPDU) {
        return false;
    }

    bool accepted = false;
    uint32_t session = 0u;
    uint32_t event_id = 0u;
    CRITICAL_REGION_ENTER();
    if (m_active && m_field_active && m_current_session != 0u &&
        !queue_has_unanswered_apdu_locked()) {
        // Reserve the last event ID for FIELD_OFF before starting a new session.
        if (m_next_event_id >= UINT32_MAX - 1u) {
            end_session_locked();
            start_session_locked();
        }
        if (m_field_active && m_event_count < BRIDGE_EVENT_QUEUE_SIZE) {
            event_id = allocate_event_id();
            session = m_current_session;
            accepted = queue_append_locked(BRIDGE_EVENT_APDU, session,
                                           event_id, capdu, length);
        }
    }
    CRITICAL_REGION_EXIT();
    if (accepted) {
        log_capdu_preview(session, event_id, capdu, length);
    }
    return accepted;
}
