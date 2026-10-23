#ifndef BRIDGE_TEST_ENV_H
#define BRIDGE_TEST_ENV_H

#include <stddef.h>

#include "nfc_14a_4.h"
#include "nfc_fido_bridge.h"
#include "rfid_main.h"

typedef struct {
    uint32_t now;
    device_mode_t mode;
    bool sense_enabled;
    bool ordinary_sense_enabled;
    bool runtime_installed;
    bool allow_install;
    bool allow_submit;
    bool field_off_during_submit;
    bool field_on_after_submit;
    bool rf_sending;
    bool sleep_stopped;
    bool critical_fault;
    bool submit_in_critical;
    bool bulk_memory_in_critical;
    unsigned critical_depth;
    unsigned install_count;
    unsigned uninstall_count;
    unsigned abort_count;
    unsigned submit_count;
    unsigned normal_reload_count;
    uint32_t sleep_delay;
    uint16_t rapdu_length;
    uint8_t rapdu[NFC_FIDO_BRIDGE_MAX_RAPDU];
    nfc_tag_14a_coll_res_entity_t profile;
    nfc_tag_14a_4_runtime_callbacks_t callbacks;
} bridge_test_env_t;

extern bridge_test_env_t bridge_test_env;
void bridge_test_env_reset(device_mode_t mode);
void bridge_test_memory_operation(size_t length);

#endif
