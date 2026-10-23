#ifndef BLE_HID_KEYBOARD_H
#define BLE_HID_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "ble.h"

#define BLE_HID_KEYBOARD_PROTOCOL_VERSION 1u
#define BLE_HID_KEYBOARD_REPORT_SIZE      8u
#define BLE_HID_KEYBOARD_STATUS_SIZE      11u

typedef enum {
    BLE_HID_KEYBOARD_OK = 0,
    BLE_HID_KEYBOARD_INVALID_ARGUMENT,
    BLE_HID_KEYBOARD_INVALID_STATE,
    BLE_HID_KEYBOARD_BUSY,
    BLE_HID_KEYBOARD_INTERNAL_ERROR,
} ble_hid_keyboard_result_t;

void ble_hid_keyboard_init(void);
void ble_hid_keyboard_on_ble_evt(ble_evt_t const *event);

ble_hid_keyboard_result_t ble_hid_keyboard_configure(bool enabled,
                                                     bool pairing_allowed);
ble_hid_keyboard_result_t ble_hid_keyboard_send_report(
    const uint8_t report[BLE_HID_KEYBOARD_REPORT_SIZE]);
ble_hid_keyboard_result_t ble_hid_keyboard_release_all(void);
void ble_hid_keyboard_get_status(
    uint8_t status[BLE_HID_KEYBOARD_STATUS_SIZE]);

bool ble_hid_keyboard_pairing_allowed(void);

#endif
