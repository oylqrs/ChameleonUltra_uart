#include "iso14443_4_reader.h"

#include <stdbool.h>
#include <string.h>

#include "app_status.h"
#include "rc522.h"

#define ISO_DEP_FRAME_CAPACITY     64U
#define ISO_DEP_IO_TIMEOUT_MS      600U
#define ISO_DEP_MAX_WTX_EXCHANGES  64U

#define PCB_IBLOCK_BASE       0x02U
#define PCB_IBLOCK_MASK       0xE2U
#define PCB_RBLOCK_ACK_BASE   0xA2U
#define PCB_SBLOCK_WTX        0xF2U
#define PCB_BLOCK_NUMBER      0x01U
#define PCB_NAD_FOLLOWING     0x04U
#define PCB_CID_FOLLOWING     0x08U
#define PCB_CHAINING          0x10U

static bool frame_crc_is_valid(const uint8_t *frame, uint16_t length) {
    if (length < 3U) {
        return false;
    }

    uint8_t crc[2];
    crc_14a_calculate((uint8_t *)frame, length - 2U, crc);
    return frame[length - 2U] == crc[0] && frame[length - 1U] == crc[1];
}

static iso14443_4_reader_result_t transceive(
    uint8_t *tx,
    uint8_t tx_length,
    uint8_t *rx,
    uint16_t *rx_length,
    uint16_t timeout_ms) {
    uint16_t previous_timeout = pcd_14a_reader_timeout_get();
    uint16_t rx_bits = 0;

    write_register_single(ComIrqReg, 0x7FU);
    pcd_14a_reader_timeout_set(timeout_ms);
    uint8_t status = pcd_14a_reader_bytes_transfer(
        PCD_TRANSCEIVE, tx, tx_length, rx, &rx_bits,
        ISO_DEP_FRAME_CAPACITY * 8U);
    pcd_14a_reader_timeout_set(previous_timeout);

    if (status != STATUS_HF_TAG_OK || rx_bits < 24U ||
            (rx_bits & 7U) != 0U ||
            rx_bits > ISO_DEP_FRAME_CAPACITY * 8U) {
        return ISO14443_4_READER_IO_ERROR;
    }

    *rx_length = rx_bits / 8U;
    if (!frame_crc_is_valid(rx, *rx_length)) {
        return ISO14443_4_READER_CRC_ERROR;
    }
    return ISO14443_4_READER_OK;
}

static bool is_iblock(uint8_t pcb) {
    return (pcb & PCB_IBLOCK_MASK) == PCB_IBLOCK_BASE;
}

static bool is_wtx(const uint8_t *frame, uint16_t length) {
    return length == 4U && frame[0] == PCB_SBLOCK_WTX &&
           frame[1] > 0U && frame[1] <= 59U;
}

iso14443_4_reader_result_t iso14443_4_reader_exchange(
    const uint8_t *apdu,
    uint16_t apdu_length,
    uint8_t *response,
    uint16_t response_capacity,
    uint16_t *response_length,
    uint8_t *block_number) {
    if (apdu == NULL || apdu_length == 0U ||
            apdu_length > ISO14443_4_READER_MAX_APDU_LENGTH ||
            response == NULL || response_capacity == 0U ||
            response_length == NULL || block_number == NULL) {
        return ISO14443_4_READER_INVALID_ARGUMENT;
    }

    uint8_t tx[ISO_DEP_FRAME_CAPACITY];
    uint8_t rx[ISO_DEP_FRAME_CAPACITY];
    uint8_t request_block = *block_number & PCB_BLOCK_NUMBER;
    uint8_t expected_response_block = 0;
    uint16_t rx_length = 0;
    uint16_t assembled = 0;
    uint8_t wtx_count = 0;
    bool first_iblock = true;

    *response_length = 0;
    tx[0] = PCB_IBLOCK_BASE | request_block;
    memcpy(&tx[1], apdu, apdu_length);
    crc_14a_append(tx, apdu_length + 1U);

    iso14443_4_reader_result_t result = transceive(
        tx, (uint8_t)(apdu_length + 3U), rx, &rx_length,
        ISO_DEP_IO_TIMEOUT_MS);
    while (result == ISO14443_4_READER_OK) {
        if (is_wtx(rx, rx_length)) {
            if (++wtx_count > ISO_DEP_MAX_WTX_EXCHANGES) {
                return ISO14443_4_READER_PROTOCOL_ERROR;
            }
            memcpy(tx, rx, rx_length - 2U);
            crc_14a_append(tx, rx_length - 2U);
            uint16_t wtx_timeout_ms =
                (uint16_t)(ISO_DEP_IO_TIMEOUT_MS * rx[1]);
            result = transceive(tx, (uint8_t)rx_length, rx, &rx_length,
                                wtx_timeout_ms);
            continue;
        }

        uint8_t pcb = rx[0];
        if (!is_iblock(pcb) ||
                (pcb & (PCB_CID_FOLLOWING | PCB_NAD_FOLLOWING)) != 0U ||
                (!first_iblock &&
                 (pcb & PCB_BLOCK_NUMBER) != expected_response_block)) {
            return ISO14443_4_READER_PROTOCOL_ERROR;
        }

        uint16_t inf_length = rx_length - 3U;
        if (inf_length == 0U) {
            return ISO14443_4_READER_PROTOCOL_ERROR;
        }
        if (assembled > response_capacity ||
                inf_length > response_capacity - assembled) {
            return ISO14443_4_READER_RESPONSE_TOO_LARGE;
        }
        memcpy(&response[assembled], &rx[1], inf_length);
        assembled += inf_length;

        if (first_iblock) {
            *block_number = request_block ^ PCB_BLOCK_NUMBER;
            first_iblock = false;
        }
        if ((pcb & PCB_CHAINING) == 0U) {
            *response_length = assembled;
            return ISO14443_4_READER_OK;
        }

        expected_response_block = (pcb ^ PCB_BLOCK_NUMBER) &
                                  PCB_BLOCK_NUMBER;
        tx[0] = PCB_RBLOCK_ACK_BASE | expected_response_block;
        crc_14a_append(tx, 1U);
        result = transceive(tx, 3U, rx, &rx_length,
                            ISO_DEP_IO_TIMEOUT_MS);
    }

    return result;
}
