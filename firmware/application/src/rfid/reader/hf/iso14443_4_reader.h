#ifndef ISO14443_4_READER_H
#define ISO14443_4_READER_H

#include <stdint.h>

#define ISO14443_4_READER_MAX_APDU_LENGTH 61U

typedef enum {
    ISO14443_4_READER_OK = 0,
    ISO14443_4_READER_INVALID_ARGUMENT,
    ISO14443_4_READER_IO_ERROR,
    ISO14443_4_READER_CRC_ERROR,
    ISO14443_4_READER_PROTOCOL_ERROR,
    ISO14443_4_READER_RESPONSE_TOO_LARGE,
} iso14443_4_reader_result_t;

iso14443_4_reader_result_t iso14443_4_reader_exchange(
    const uint8_t *apdu,
    uint16_t apdu_length,
    uint8_t *response,
    uint16_t response_capacity,
    uint16_t *response_length,
    uint8_t *block_number);

#endif /* ISO14443_4_READER_H */
