#include <string.h>

#include "app_timer.h"
#include "app_util_platform.h"
#include "nrf.h"
#include "syssleep.h"
#include "tag_emulation.h"
#include "test_env.h"

bridge_test_env_t bridge_test_env;
bridge_test_ficr_t bridge_test_ficr = {{0x12345678u, 0x9abcdef0u}};

void bridge_test_env_reset(device_mode_t mode) {
    memset(&bridge_test_env, 0, sizeof(bridge_test_env));
    bridge_test_env.mode = mode;
    bridge_test_env.ordinary_sense_enabled = mode == DEVICE_MODE_TAG;
    bridge_test_env.sense_enabled = mode == DEVICE_MODE_TAG;
    bridge_test_env.allow_install = true;
    bridge_test_env.allow_submit = true;
}

uint32_t app_timer_cnt_get(void) {
    return bridge_test_env.now & 0xffffffu;
}

uint32_t app_timer_cnt_diff_compute(uint32_t now, uint32_t previous) {
    return (now - previous) & 0xffffffu;
}

uint8_t bridge_test_critical_enter(void) {
    return (uint8_t)bridge_test_env.critical_depth++;
}

void bridge_test_critical_exit(uint8_t nested) {
    if (bridge_test_env.critical_depth == 0u ||
        bridge_test_env.critical_depth - 1u != nested) {
        bridge_test_env.critical_fault = true;
    } else {
        bridge_test_env.critical_depth--;
    }
}

void bridge_test_memory_operation(size_t length) {
    if (length >= 1024u && bridge_test_env.critical_depth != 0u) {
        bridge_test_env.bulk_memory_in_critical = true;
    }
}

void nfc_tag_14a_sense_switch(bool enable) {
    bridge_test_env.sense_enabled = enable;
}

bool nfc_tag_14a_4_runtime_install(
    const nfc_tag_14a_coll_res_entity_t *profile,
    const nfc_tag_14a_4_runtime_callbacks_t *callbacks) {
    bridge_test_env.install_count++;
    if (!bridge_test_env.allow_install) return false;
    bridge_test_env.profile = *profile;
    bridge_test_env.callbacks = *callbacks;
    bridge_test_env.runtime_installed = true;
    return true;
}

void nfc_tag_14a_4_runtime_uninstall(void) {
    bridge_test_env.uninstall_count++;
    bridge_test_env.runtime_installed = false;
    memset(&bridge_test_env.callbacks, 0, sizeof(bridge_test_env.callbacks));
}

void nfc_tag_14a_4_abort(void) {
    bridge_test_env.abort_count++;
    bridge_test_env.rf_sending = false;
}

bool nfc_tag_14a_4_submit_response(const uint8_t *data, uint16_t length) {
    if (!bridge_test_env.allow_submit) return false;
    if (bridge_test_env.critical_depth != 0u) {
        bridge_test_env.submit_in_critical = true;
    }
    bridge_test_env.submit_count++;
    bridge_test_env.rapdu_length = length;
    memcpy(bridge_test_env.rapdu, data, length);
    bridge_test_env.rf_sending = true;
    if (bridge_test_env.field_off_during_submit &&
        bridge_test_env.callbacks.on_field_off != NULL) {
        bridge_test_env.field_off_during_submit = false;
        bridge_test_env.callbacks.on_field_off();
        if (bridge_test_env.field_on_after_submit &&
            bridge_test_env.callbacks.on_field_on != NULL) {
            bridge_test_env.field_on_after_submit = false;
            bridge_test_env.callbacks.on_field_on();
        }
    }
    return true;
}

device_mode_t get_device_mode(void) {
    return bridge_test_env.mode;
}

void tag_mode_enter(void) {
    bridge_test_env.mode = DEVICE_MODE_TAG;
    tag_emulation_sense_run();
}

void reader_mode_enter(void) {
    bridge_test_env.mode = DEVICE_MODE_READER;
    tag_emulation_sense_end();
}

void device_mode_exit(void) {
    bridge_test_env.mode = DEVICE_MODE_NONE;
    tag_emulation_sense_end();
}

void tag_emulation_load_data(void) {
    bridge_test_env.normal_reload_count++;
}

void tag_emulation_sense_run(void) {
    bridge_test_env.ordinary_sense_enabled = true;
    bridge_test_env.sense_enabled = true;
}

void tag_emulation_sense_end(void) {
    bridge_test_env.ordinary_sense_enabled = false;
    bridge_test_env.sense_enabled = false;
}

void sleep_timer_stop(void) {
    bridge_test_env.sleep_stopped = true;
}

void sleep_timer_start(uint32_t milliseconds) {
    bridge_test_env.sleep_stopped = false;
    bridge_test_env.sleep_delay = milliseconds;
}
