#ifndef TEST_RC522_H
#define TEST_RC522_H

#include <stddef.h>
#include <stdint.h>

#define PCD_TRANSCEIVE 0x0CU
#define ComIrqReg      0x04U

uint16_t pcd_14a_reader_timeout_get(void);
void pcd_14a_reader_timeout_set(uint16_t timeout_ms);
uint8_t pcd_14a_reader_bytes_transfer(
    uint8_t command,
    uint8_t *input,
    uint8_t input_length,
    uint8_t *output,
    uint16_t *output_length_bits,
    uint16_t max_output_length_bits);
void write_register_single(uint8_t reg, uint8_t value);
void crc_14a_calculate(uint8_t *data, size_t length, uint8_t *crc);
void crc_14a_append(uint8_t *data, size_t length);

#endif
