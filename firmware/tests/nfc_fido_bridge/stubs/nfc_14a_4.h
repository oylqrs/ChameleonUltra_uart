#ifndef TEST_NFC_14A_4_H
#define TEST_NFC_14A_4_H

#include "nfc_14a.h"

#define NFC_14A_4_MAX_CAPDU 297u
#define NFC_14A_4_MAX_RAPDU 4088u

typedef struct {
    void (*on_field_on)(void);
    void (*on_field_off)(void);
    bool (*on_capdu)(const uint8_t *capdu, uint16_t length);
} nfc_tag_14a_4_runtime_callbacks_t;

bool nfc_tag_14a_4_runtime_install(
    const nfc_tag_14a_coll_res_entity_t *profile,
    const nfc_tag_14a_4_runtime_callbacks_t *callbacks);
void nfc_tag_14a_4_runtime_uninstall(void);
void nfc_tag_14a_4_abort(void);
bool nfc_tag_14a_4_submit_response(const uint8_t *data, uint16_t length);

#endif
