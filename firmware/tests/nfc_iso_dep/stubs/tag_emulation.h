#ifndef TEST_TAG_EMULATION_H
#define TEST_TAG_EMULATION_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "tag_base_type.h"
typedef struct {
    uint16_t length;
    uint8_t *buffer;
    uint16_t *crc;
} tag_data_buffer_t;
#endif
