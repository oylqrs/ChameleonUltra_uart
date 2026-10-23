#ifndef NFC_FIDO_BRIDGE_H
#define NFC_FIDO_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#define NFC_FIDO_BRIDGE_PROTOCOL_VERSION  1u
#define NFC_FIDO_BRIDGE_MAX_CAPDU          297u
#define NFC_FIDO_BRIDGE_MAX_RAPDU          4088u
#define NFC_FIDO_BRIDGE_EVENT_HEADER_SIZE  9u

typedef enum {
    NFC_FIDO_BRIDGE_OK = 0,
    NFC_FIDO_BRIDGE_INVALID_ARGUMENT,
    NFC_FIDO_BRIDGE_INVALID_STATE,
    NFC_FIDO_BRIDGE_UNSUPPORTED,
} nfc_fido_bridge_result_t;

nfc_fido_bridge_result_t nfc_fido_bridge_configure(
    const uint8_t *request,
    uint16_t request_length,
    uint8_t *response,
    uint16_t *response_length);

nfc_fido_bridge_result_t nfc_fido_bridge_poll(
    const uint8_t *request,
    uint16_t request_length,
    uint8_t *response,
    uint16_t *response_length);

nfc_fido_bridge_result_t nfc_fido_bridge_response(
    const uint8_t *request,
    uint16_t request_length,
    uint8_t *response,
    uint16_t *response_length);

nfc_fido_bridge_result_t nfc_fido_bridge_reset(
    const uint8_t *request,
    uint16_t request_length,
    uint8_t *response,
    uint16_t *response_length);

void nfc_fido_bridge_process(void);
bool nfc_fido_bridge_is_active(void);

/* RF callbacks run from the NFCT interrupt. */
void nfc_fido_bridge_on_field_on(void);
void nfc_fido_bridge_on_field_off(void);
bool nfc_fido_bridge_on_capdu(const uint8_t *capdu, uint16_t length);

#endif
