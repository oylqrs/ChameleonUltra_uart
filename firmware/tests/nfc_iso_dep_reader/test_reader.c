#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_status.h"
#include "iso14443_4_reader.h"
#include "rc522.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)
#define MAX_STEPS 12U
#define MAX_FRAME 64U

typedef struct {
    uint8_t tx[MAX_FRAME];
    uint8_t tx_length;
    uint8_t rx[MAX_FRAME];
    uint8_t rx_length;
    uint8_t status;
    uint16_t timeout_ms;
} transfer_step_t;

static transfer_step_t steps[MAX_STEPS];
static uint8_t step_count;
static uint8_t step_index;
static uint16_t timeout_ms;
static uint16_t initial_timeout_ms;
static uint8_t irq_clear_count;
static bool transport_fault;

volatile unsigned bridge_test_case;

void crc_14a_calculate(uint8_t *data, size_t length, uint8_t *crc) {
    uint16_t value = 0x6363U;
    for (size_t i = 0; i < length; i++) {
        uint8_t byte = data[i] ^ (uint8_t)value;
        byte ^= (uint8_t)(byte << 4);
        value = (value >> 8) ^ ((uint16_t)byte << 8) ^
                ((uint16_t)byte << 3) ^ ((uint16_t)byte >> 4);
    }
    crc[0] = (uint8_t)value;
    crc[1] = (uint8_t)(value >> 8);
}

void crc_14a_append(uint8_t *data, size_t length) {
    crc_14a_calculate(data, length, &data[length]);
}

uint16_t pcd_14a_reader_timeout_get(void) {
    return timeout_ms;
}

void pcd_14a_reader_timeout_set(uint16_t value) {
    timeout_ms = value;
}

void write_register_single(uint8_t reg, uint8_t value) {
    if (reg == ComIrqReg && value == 0x7FU) {
        irq_clear_count++;
    } else {
        transport_fault = true;
    }
}

uint8_t pcd_14a_reader_bytes_transfer(
    uint8_t command,
    uint8_t *input,
    uint8_t input_length,
    uint8_t *output,
    uint16_t *output_length_bits,
    uint16_t max_output_length_bits) {
    if (step_index >= step_count) {
        transport_fault = true;
        *output_length_bits = 0;
        return 0xFFU;
    }

    transfer_step_t *step = &steps[step_index];
    if (command != PCD_TRANSCEIVE || input_length != step->tx_length ||
            memcmp(input, step->tx, input_length) != 0 ||
            max_output_length_bits != MAX_FRAME * 8U ||
            timeout_ms != step->timeout_ms ||
            irq_clear_count != step_index + 1U) {
        transport_fault = true;
    }

    memcpy(output, step->rx, step->rx_length);
    *output_length_bits = (uint16_t)step->rx_length * 8U;
    step_index++;
    return step->status;
}

static void reset_script(uint16_t timeout) {
    memset(steps, 0, sizeof(steps));
    step_count = 0;
    step_index = 0;
    timeout_ms = timeout;
    initial_timeout_ms = timeout;
    irq_clear_count = 0;
    transport_fault = false;
}

static void add_step(
    const uint8_t *tx,
    uint8_t tx_length,
    const uint8_t *rx,
    uint8_t rx_length) {
    transfer_step_t *step = &steps[step_count++];
    memcpy(step->tx, tx, tx_length);
    step->tx_length = tx_length + 2U;
    crc_14a_append(step->tx, tx_length);
    memcpy(step->rx, rx, rx_length);
    step->rx_length = rx_length + 2U;
    crc_14a_append(step->rx, rx_length);
    step->status = STATUS_HF_TAG_OK;
    step->timeout_ms = 600U;
}

static bool script_finished(void) {
    return !transport_fault && step_index == step_count &&
           irq_clear_count == step_count && timeout_ms == initial_timeout_ms;
}

static int test_chaining_ack_and_request_block_number(void) {
    static const uint8_t apdu1[] = {0x00U, 0xA4U};
    static const uint8_t tx1[] = {0x02U, 0x00U, 0xA4U};
    static const uint8_t rx1[] = {0x12U, 0xDEU, 0xADU};
    static const uint8_t ack[] = {0xA3U};
    static const uint8_t rx2[] = {0x03U, 0xBEU, 0xEFU};
    static const uint8_t expected[] = {0xDEU, 0xADU, 0xBEU, 0xEFU};
    uint8_t response[8] = {0};
    uint16_t response_length = 0;
    uint8_t block_number = 0;

    reset_script(37U);
    add_step(tx1, sizeof(tx1), rx1, sizeof(rx1));
    add_step(ack, sizeof(ack), rx2, sizeof(rx2));
    CHECK(iso14443_4_reader_exchange(
              apdu1, sizeof(apdu1), response, sizeof(response),
              &response_length, &block_number) == ISO14443_4_READER_OK);
    CHECK(response_length == sizeof(expected));
    CHECK(memcmp(response, expected, sizeof(expected)) == 0);
    CHECK(block_number == 1U);
    CHECK(script_finished());

    static const uint8_t apdu2[] = {0x00U, 0xB0U};
    static const uint8_t tx2[] = {0x03U, 0x00U, 0xB0U};
    static const uint8_t rx3[] = {0x02U, 0x90U, 0x00U};
    reset_script(91U);
    add_step(tx2, sizeof(tx2), rx3, sizeof(rx3));
    CHECK(iso14443_4_reader_exchange(
              apdu2, sizeof(apdu2), response, sizeof(response),
              &response_length, &block_number) == ISO14443_4_READER_OK);
    CHECK(response_length == 2U && response[0] == 0x90U && response[1] == 0U);
    CHECK(block_number == 0U);
    CHECK(script_finished());
    return 0;
}

static int test_initial_wtx_echo(void) {
    static const uint8_t apdu[] = {0xCAU};
    static const uint8_t request[] = {0x02U, 0xCAU};
    static const uint8_t wtx[] = {0xF2U, 0x3BU};
    static const uint8_t response_frame[] = {0x02U, 0x90U, 0x00U};
    uint8_t response[4] = {0};
    uint16_t response_length = 0;
    uint8_t block_number = 0;

    reset_script(25U);
    add_step(request, sizeof(request), wtx, sizeof(wtx));
    add_step(wtx, sizeof(wtx), response_frame, sizeof(response_frame));
    steps[1].timeout_ms = 600U * 0x3BU;
    CHECK(iso14443_4_reader_exchange(
              apdu, sizeof(apdu), response, sizeof(response),
              &response_length, &block_number) == ISO14443_4_READER_OK);
    CHECK(response_length == 2U && response[0] == 0x90U && response[1] == 0U);
    CHECK(block_number == 1U);
    CHECK(script_finished());
    return 0;
}

static int test_wtx_between_response_fragments(void) {
    static const uint8_t apdu[] = {0x01U};
    static const uint8_t request[] = {0x02U, 0x01U};
    static const uint8_t first[] = {0x12U, 0x11U};
    static const uint8_t ack[] = {0xA3U};
    static const uint8_t wtx[] = {0xF2U, 0x02U};
    static const uint8_t last[] = {0x03U, 0x22U};
    uint8_t response[2] = {0};
    uint16_t response_length = 0;
    uint8_t block_number = 0;

    reset_script(44U);
    add_step(request, sizeof(request), first, sizeof(first));
    add_step(ack, sizeof(ack), wtx, sizeof(wtx));
    add_step(wtx, sizeof(wtx), last, sizeof(last));
    steps[2].timeout_ms = 600U * 2U;
    CHECK(iso14443_4_reader_exchange(
              apdu, sizeof(apdu), response, sizeof(response),
              &response_length, &block_number) == ISO14443_4_READER_OK);
    CHECK(response_length == 2U && response[0] == 0x11U && response[1] == 0x22U);
    CHECK(script_finished());
    return 0;
}

static void add_four_byte_chained_response(void) {
    static const uint8_t request[] = {0x02U, 0x02U};
    static const uint8_t first[] = {0x12U, 0x10U, 0x20U};
    static const uint8_t ack[] = {0xA3U};
    static const uint8_t last[] = {0x03U, 0x30U, 0x40U};
    add_step(request, sizeof(request), first, sizeof(first));
    add_step(ack, sizeof(ack), last, sizeof(last));
}

static int test_response_capacity(void) {
    static const uint8_t apdu[] = {0x02U};
    static const uint8_t expected[] = {0x10U, 0x20U, 0x30U, 0x40U};
    uint8_t response[4] = {0};
    uint16_t response_length = 0xFFFFU;
    uint8_t block_number = 0;

    reset_script(25U);
    add_four_byte_chained_response();
    CHECK(iso14443_4_reader_exchange(
              apdu, sizeof(apdu), response, sizeof(response),
              &response_length, &block_number) == ISO14443_4_READER_OK);
    CHECK(response_length == sizeof(expected));
    CHECK(memcmp(response, expected, sizeof(expected)) == 0);
    CHECK(script_finished());

    reset_script(25U);
    add_four_byte_chained_response();
    block_number = 0;
    response_length = 0xFFFFU;
    CHECK(iso14443_4_reader_exchange(
              apdu, sizeof(apdu), response, sizeof(response) - 1U,
              &response_length, &block_number) ==
          ISO14443_4_READER_RESPONSE_TOO_LARGE);
    CHECK(response_length == 0U);
    CHECK(script_finished());
    return 0;
}

static int test_crc_error(void) {
    static const uint8_t apdu[] = {0x03U};
    static const uint8_t request[] = {0x02U, 0x03U};
    static const uint8_t reply[] = {0x02U, 0x90U, 0x00U};
    uint8_t response[4] = {0};
    uint16_t response_length = 7U;
    uint8_t block_number = 0;

    reset_script(25U);
    add_step(request, sizeof(request), reply, sizeof(reply));
    steps[0].rx[steps[0].rx_length - 1U] ^= 0x80U;
    CHECK(iso14443_4_reader_exchange(
              apdu, sizeof(apdu), response, sizeof(response),
              &response_length, &block_number) == ISO14443_4_READER_CRC_ERROR);
    CHECK(response_length == 0U);
    CHECK(script_finished());
    return 0;
}

static int test_protocol_errors(void) {
    static const uint8_t apdu[] = {0x04U};
    static const uint8_t request[] = {0x02U, 0x04U};
    static const uint8_t first[] = {0x12U, 0xAAU};
    static const uint8_t ack[] = {0xA3U};
    static const uint8_t wrong_block[] = {0x02U, 0xBBU};
    static const uint8_t invalid_pcb[] = {0xA2U};
    uint8_t response[4] = {0};
    uint16_t response_length = 8U;
    uint8_t block_number = 0;

    reset_script(25U);
    add_step(request, sizeof(request), first, sizeof(first));
    add_step(ack, sizeof(ack), wrong_block, sizeof(wrong_block));
    CHECK(iso14443_4_reader_exchange(
              apdu, sizeof(apdu), response, sizeof(response),
              &response_length, &block_number) ==
          ISO14443_4_READER_PROTOCOL_ERROR);
    CHECK(response_length == 0U);
    CHECK(script_finished());

    reset_script(25U);
    add_step(request, sizeof(request), invalid_pcb, sizeof(invalid_pcb));
    block_number = 0;
    response_length = 8U;
    CHECK(iso14443_4_reader_exchange(
              apdu, sizeof(apdu), response, sizeof(response),
              &response_length, &block_number) ==
          ISO14443_4_READER_PROTOCOL_ERROR);
    CHECK(response_length == 0U);
    CHECK(script_finished());
    return 0;
}

int test_iso_dep_reader(void) {
    int result;
    bridge_test_case = 1U;
    result = test_chaining_ack_and_request_block_number();
    if (result != 0) return result;
    bridge_test_case = 2U;
    result = test_initial_wtx_echo();
    if (result != 0) return result;
    bridge_test_case = 3U;
    result = test_wtx_between_response_fragments();
    if (result != 0) return result;
    bridge_test_case = 4U;
    result = test_response_capacity();
    if (result != 0) return result;
    bridge_test_case = 5U;
    result = test_crc_error();
    if (result != 0) return result;
    bridge_test_case = 6U;
    return test_protocol_errors();
}
