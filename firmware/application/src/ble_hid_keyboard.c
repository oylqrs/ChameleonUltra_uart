#define NRF_LOG_MODULE_NAME ble_hid_keyboard

#include "ble_hid_keyboard.h"

#include <string.h>

#include "app_error.h"
#include "ble_hids.h"
#include "ble_srv_common.h"
#include "nrf_error.h"
#include "nrf_log.h"
#include "nrf_sdh_ble.h"
#include "peer_manager.h"

NRF_LOG_MODULE_REGISTER();

#define HID_INPUT_REPORT_INDEX  0u
#define HID_OUTPUT_REPORT_INDEX 0u
#define HID_REPORT_QUEUE_SIZE   8u
#define HID_PROTOCOL_MODE_BOOT   0u
#define HID_PROTOCOL_MODE_REPORT 1u

BLE_HIDS_DEF(m_hids, NRF_SDH_BLE_TOTAL_LINK_COUNT,
             BLE_HID_KEYBOARD_REPORT_SIZE, 1u);

static const uint8_t m_report_map[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x06,       /* Usage (Keyboard) */
    0xA1, 0x01,       /* Collection (Application) */
    0x05, 0x07,       /* Usage Page (Keyboard/Keypad) */
    0x19, 0xE0,       /* Usage Minimum (Keyboard Left Control) */
    0x29, 0xE7,       /* Usage Maximum (Keyboard Right GUI) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x25, 0x01,       /* Logical Maximum (1) */
    0x75, 0x01,       /* Report Size (1) */
    0x95, 0x08,       /* Report Count (8) */
    0x81, 0x02,       /* Input (Data, Variable, Absolute) */
    0x95, 0x01,       /* Report Count (1) */
    0x75, 0x08,       /* Report Size (8) */
    0x81, 0x01,       /* Input (Constant) */
    0x95, 0x06,       /* Report Count (6) */
    0x75, 0x08,       /* Report Size (8) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x25, 0x65,       /* Logical Maximum (101) */
    0x05, 0x07,       /* Usage Page (Keyboard/Keypad) */
    0x19, 0x00,       /* Usage Minimum (Reserved) */
    0x29, 0x65,       /* Usage Maximum (Keyboard Application) */
    0x81, 0x00,       /* Input (Data, Array, Absolute) */
    0x95, 0x05,       /* Report Count (5) */
    0x75, 0x01,       /* Report Size (1) */
    0x05, 0x08,       /* Usage Page (LEDs) */
    0x19, 0x01,       /* Usage Minimum (Num Lock) */
    0x29, 0x05,       /* Usage Maximum (Kana) */
    0x91, 0x02,       /* Output (Data, Variable, Absolute) */
    0x95, 0x01,       /* Report Count (1) */
    0x75, 0x03,       /* Report Size (3) */
    0x91, 0x01,       /* Output (Constant) */
    0xC0,             /* End Collection */
};

/* ble_hids_init() retains these pointers for later BLE events. */
static ble_hids_inp_rep_init_t m_input_report;
static ble_hids_outp_rep_init_t m_output_report;
static uint8_t m_report_queue[HID_REPORT_QUEUE_SIZE][BLE_HID_KEYBOARD_REPORT_SIZE];
static uint8_t m_queue_head;
static uint8_t m_queue_tail;
static uint8_t m_queue_count;
static uint16_t m_conn_handle = BLE_CONN_HANDLE_INVALID;
static bool m_enabled;
static bool m_pairing_allowed;
static bool m_encrypted;
static bool m_report_notifications_enabled;
static bool m_boot_notifications_enabled;
static bool m_suspended;
static bool m_initialized;
static uint8_t m_protocol_mode = HID_PROTOCOL_MODE_REPORT;
static uint8_t m_led_state;

static void queue_clear(void) {
    m_queue_head = 0u;
    m_queue_tail = 0u;
    m_queue_count = 0u;
}

static bool queue_push(const uint8_t report[BLE_HID_KEYBOARD_REPORT_SIZE]) {
    if (m_queue_count >= HID_REPORT_QUEUE_SIZE) {
        return false;
    }
    memcpy(m_report_queue[m_queue_tail], report, BLE_HID_KEYBOARD_REPORT_SIZE);
    m_queue_tail = (uint8_t)((m_queue_tail + 1u) % HID_REPORT_QUEUE_SIZE);
    m_queue_count++;
    return true;
}

static void queue_pop(void) {
    if (m_queue_count == 0u) {
        return;
    }
    m_queue_head = (uint8_t)((m_queue_head + 1u) % HID_REPORT_QUEUE_SIZE);
    m_queue_count--;
}

static bool notifications_ready(void) {
    return m_protocol_mode == HID_PROTOCOL_MODE_BOOT
               ? m_boot_notifications_enabled
               : m_report_notifications_enabled;
}

static bool transport_ready(void) {
    return m_conn_handle != BLE_CONN_HANDLE_INVALID && m_encrypted &&
           notifications_ready() && !m_suspended;
}

static uint32_t send_one(uint8_t *report) {
    if (m_protocol_mode == HID_PROTOCOL_MODE_BOOT) {
        return ble_hids_boot_kb_inp_rep_send(&m_hids,
                                             BLE_HID_KEYBOARD_REPORT_SIZE,
                                             report, m_conn_handle);
    }
    return ble_hids_inp_rep_send(&m_hids, HID_INPUT_REPORT_INDEX,
                                 BLE_HID_KEYBOARD_REPORT_SIZE, report,
                                 m_conn_handle);
}

static ble_hid_keyboard_result_t flush_queue(void) {
    if (!transport_ready()) {
        return BLE_HID_KEYBOARD_INVALID_STATE;
    }

    while (m_queue_count != 0u) {
        uint32_t err = send_one(m_report_queue[m_queue_head]);
        if (err == NRF_SUCCESS) {
            queue_pop();
            continue;
        }
        if (err == NRF_ERROR_RESOURCES || err == NRF_ERROR_BUSY) {
            return BLE_HID_KEYBOARD_OK;
        }
        if (err == NRF_ERROR_INVALID_STATE || err == NRF_ERROR_NOT_FOUND) {
            return BLE_HID_KEYBOARD_INVALID_STATE;
        }
        NRF_LOG_ERROR("HID report send failed: 0x%08x", err);
        return BLE_HID_KEYBOARD_INTERNAL_ERROR;
    }
    return BLE_HID_KEYBOARD_OK;
}

static void hids_error_handler(uint32_t error) {
    NRF_LOG_ERROR("HIDS error: 0x%08x", error);
}

static void hids_evt_handler(ble_hids_t *hids, ble_hids_evt_t *event) {
    (void)hids;
    switch (event->evt_type) {
        case BLE_HIDS_EVT_NOTIF_ENABLED:
            if (event->params.notification.char_id.uuid == BLE_UUID_REPORT_CHAR &&
                    event->params.notification.char_id.rep_type == BLE_HIDS_REP_TYPE_INPUT &&
                    event->params.notification.char_id.rep_index == HID_INPUT_REPORT_INDEX) {
                m_report_notifications_enabled = true;
            } else if (event->params.notification.char_id.uuid ==
                       BLE_UUID_BOOT_KEYBOARD_INPUT_REPORT_CHAR) {
                m_boot_notifications_enabled = true;
            }
            (void)flush_queue();
            break;
        case BLE_HIDS_EVT_NOTIF_DISABLED:
            if (event->params.notification.char_id.uuid == BLE_UUID_REPORT_CHAR &&
                    event->params.notification.char_id.rep_type == BLE_HIDS_REP_TYPE_INPUT &&
                    event->params.notification.char_id.rep_index == HID_INPUT_REPORT_INDEX) {
                m_report_notifications_enabled = false;
            } else if (event->params.notification.char_id.uuid ==
                       BLE_UUID_BOOT_KEYBOARD_INPUT_REPORT_CHAR) {
                m_boot_notifications_enabled = false;
            }
            break;
        case BLE_HIDS_EVT_HOST_SUSP:
            m_suspended = true;
            break;
        case BLE_HIDS_EVT_HOST_EXIT_SUSP:
            m_suspended = false;
            (void)flush_queue();
            break;
        case BLE_HIDS_EVT_BOOT_MODE_ENTERED:
            m_protocol_mode = HID_PROTOCOL_MODE_BOOT;
            (void)flush_queue();
            break;
        case BLE_HIDS_EVT_REPORT_MODE_ENTERED:
            m_protocol_mode = HID_PROTOCOL_MODE_REPORT;
            (void)flush_queue();
            break;
        case BLE_HIDS_EVT_REP_CHAR_WRITE:
            if (event->params.char_write.char_id.rep_type == BLE_HIDS_REP_TYPE_OUTPUT &&
                event->params.char_write.char_id.rep_index == HID_OUTPUT_REPORT_INDEX &&
                event->params.char_write.len > 0u) {
                m_led_state = event->params.char_write.data[0];
            }
            break;
        default:
            break;
    }
}

void ble_hid_keyboard_init(void) {
    ble_hids_init_t init;

    memset(&init, 0, sizeof(init));
    memset(&m_input_report, 0, sizeof(m_input_report));
    memset(&m_output_report, 0, sizeof(m_output_report));

    m_input_report.max_len = BLE_HID_KEYBOARD_REPORT_SIZE;
    m_input_report.rep_ref.report_id = 0u;
    m_input_report.rep_ref.report_type = BLE_HIDS_REP_TYPE_INPUT;
    m_input_report.sec.rd = SEC_MITM;
    m_input_report.sec.wr = SEC_NO_ACCESS;
    m_input_report.sec.cccd_wr = SEC_MITM;

    m_output_report.max_len = 1u;
    m_output_report.rep_ref.report_id = 0u;
    m_output_report.rep_ref.report_type = BLE_HIDS_REP_TYPE_OUTPUT;
    m_output_report.sec.rd = SEC_MITM;
    m_output_report.sec.wr = SEC_MITM;
    m_output_report.sec.cccd_wr = SEC_NO_ACCESS;

    init.evt_handler = hids_evt_handler;
    init.error_handler = hids_error_handler;
    init.is_kb = true;
    init.is_mouse = false;
    init.inp_rep_count = 1u;
    init.p_inp_rep_array = &m_input_report;
    init.outp_rep_count = 1u;
    init.p_outp_rep_array = &m_output_report;
    init.rep_map.p_data = (uint8_t *)m_report_map;
    init.rep_map.data_len = sizeof(m_report_map);
    init.rep_map.rd_sec = SEC_MITM;
    init.hid_information.bcd_hid = 0x0111u;
    init.hid_information.b_country_code = 0u;
    init.hid_information.flags = HID_INFO_FLAG_NORMALLY_CONNECTABLE_MSK;
    init.hid_information.rd_sec = SEC_MITM;
    init.protocol_mode_rd_sec = SEC_MITM;
    init.protocol_mode_wr_sec = SEC_MITM;
    init.ctrl_point_wr_sec = SEC_MITM;
    init.boot_kb_inp_rep_sec.rd = SEC_MITM;
    init.boot_kb_inp_rep_sec.wr = SEC_NO_ACCESS;
    init.boot_kb_inp_rep_sec.cccd_wr = SEC_MITM;
    init.boot_kb_outp_rep_sec.rd = SEC_MITM;
    init.boot_kb_outp_rep_sec.wr = SEC_MITM;
    init.boot_kb_outp_rep_sec.cccd_wr = SEC_NO_ACCESS;

    uint32_t err = ble_hids_init(&m_hids, &init);
    if (err != NRF_SUCCESS) {
        NRF_LOG_ERROR("HIDS initialization failed: 0x%08x", err);
        m_initialized = false;
        return;
    }
    m_initialized = true;
    queue_clear();
}

void ble_hid_keyboard_on_ble_evt(ble_evt_t const *event) {
    if (event == NULL) {
        return;
    }

    switch (event->header.evt_id) {
        case BLE_GAP_EVT_CONNECTED:
            m_conn_handle = event->evt.gap_evt.conn_handle;
            m_encrypted = false;
            m_report_notifications_enabled = false;
            m_boot_notifications_enabled = false;
            m_suspended = false;
            m_protocol_mode = HID_PROTOCOL_MODE_REPORT;
            queue_clear();
            break;
        case BLE_GAP_EVT_DISCONNECTED:
            m_conn_handle = BLE_CONN_HANDLE_INVALID;
            m_encrypted = false;
            m_report_notifications_enabled = false;
            m_boot_notifications_enabled = false;
            m_suspended = false;
            queue_clear();
            break;
        case BLE_GAP_EVT_CONN_SEC_UPDATE:
            m_encrypted =
                event->evt.gap_evt.params.conn_sec_update.conn_sec.sec_mode.lv >= 2u;
            if (m_encrypted) {
                (void)flush_queue();
            }
            break;
        case BLE_GATTS_EVT_HVN_TX_COMPLETE:
            (void)flush_queue();
            break;
        default:
            break;
    }
}

ble_hid_keyboard_result_t ble_hid_keyboard_configure(bool enabled,
                                                     bool pairing_allowed) {
    if (!m_initialized) {
        return BLE_HID_KEYBOARD_INTERNAL_ERROR;
    }
    if (pairing_allowed && !enabled) {
        return BLE_HID_KEYBOARD_INVALID_ARGUMENT;
    }

    if (!enabled && m_enabled) {
        uint8_t release[BLE_HID_KEYBOARD_REPORT_SIZE] = {0};
        queue_clear();
        (void)queue_push(release);
        (void)flush_queue();
    }

    m_enabled = enabled;
    m_pairing_allowed = pairing_allowed;
    if (!enabled && m_queue_count == 0u) {
        queue_clear();
    }
    return BLE_HID_KEYBOARD_OK;
}

ble_hid_keyboard_result_t ble_hid_keyboard_send_report(
    const uint8_t report[BLE_HID_KEYBOARD_REPORT_SIZE]) {
    if (report == NULL) {
        return BLE_HID_KEYBOARD_INVALID_ARGUMENT;
    }
    if (!m_initialized) {
        return BLE_HID_KEYBOARD_INTERNAL_ERROR;
    }
    if (!m_enabled || !transport_ready()) {
        return BLE_HID_KEYBOARD_INVALID_STATE;
    }
    if (!queue_push(report)) {
        return BLE_HID_KEYBOARD_BUSY;
    }
    return flush_queue();
}

ble_hid_keyboard_result_t ble_hid_keyboard_release_all(void) {
    if (!m_initialized) {
        return BLE_HID_KEYBOARD_INTERNAL_ERROR;
    }
    uint8_t release[BLE_HID_KEYBOARD_REPORT_SIZE] = {0};
    queue_clear();
    if (!queue_push(release)) {
        return BLE_HID_KEYBOARD_BUSY;
    }
    return flush_queue();
}

void ble_hid_keyboard_get_status(
    uint8_t status[BLE_HID_KEYBOARD_STATUS_SIZE]) {
    if (status == NULL) {
        return;
    }
    bool bonded = false;
    if (m_conn_handle != BLE_CONN_HANDLE_INVALID) {
        pm_peer_id_t peer_id = PM_PEER_ID_INVALID;
        bonded = pm_peer_id_get(m_conn_handle, &peer_id) == NRF_SUCCESS &&
                 peer_id != PM_PEER_ID_INVALID;
    }
    status[0] = BLE_HID_KEYBOARD_PROTOCOL_VERSION;
    status[1] = m_enabled ? 1u : 0u;
    status[2] = m_pairing_allowed ? 1u : 0u;
    status[3] = m_conn_handle != BLE_CONN_HANDLE_INVALID ? 1u : 0u;
    status[4] = m_encrypted ? 1u : 0u;
    status[5] = bonded ? 1u : 0u;
    status[6] = notifications_ready() ? 1u : 0u;
    status[7] = m_suspended ? 1u : 0u;
    status[8] = m_protocol_mode;
    status[9] = m_led_state;
    status[10] = m_queue_count;
}

bool ble_hid_keyboard_pairing_allowed(void) {
    return m_initialized && m_pairing_allowed;
}
