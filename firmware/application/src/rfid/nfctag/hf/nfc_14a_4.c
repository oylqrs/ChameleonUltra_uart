/**
 * @file nfc_14a_4.c
 * @brief ISO14443-4 T=CL emulation for ChameleonUltra
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <string.h>

#include "nfc_14a_4.h"
#include "nfc_14a.h"
#include "tag_emulation.h"
#include "tag_persistence.h"
#include "fds_util.h"
#include "app_util_platform.h"
#include "app_timer.h"
#include "nrf.h"
#include "nrf_log.h"

/* ISO14443-4 PCB fields. CRC-A is added/removed by the 14A transport. */
#define PCB_IBLOCK_BASE       0x02U
#define PCB_IBLOCK_MASK       0xE2U
#define PCB_RBLOCK_ACK_BASE   0xA2U
#define PCB_RBLOCK_NAK_BASE   0xB2U
#define PCB_RBLOCK_MASK       0xE6U
#define PCB_SBLOCK_DESELECT   0xC2U
#define PCB_SBLOCK_WTX        0xF2U
#define PCB_BLOCK_NUM         0x01U
#define PCB_NAD_FOLLOWING     0x04U
#define PCB_CID_FOLLOWING     0x08U
#define PCB_CHAIN             0x10U

#define ISO_DEP_MIN_FSD       16U
#define ISO_DEP_MAX_FSD       256U
#define ISO_DEP_FRAME_BYTES   ISO_DEP_MAX_FSD
#define WTX_VALUE             0x3BU

static nfc_tag_14a_4_information_t *m_tag_information;
static nfc_tag_14a_coll_res_reference_t m_shadow_coll_res;

/* Complete command and response buffers. */
static uint8_t m_capdu_buf[NFC_14A_4_MAX_CAPDU];
static uint16_t m_capdu_assembly_len;
static volatile uint16_t m_pending_apdu_len;
static volatile bool m_apdu_pending;
static volatile uint32_t m_capdu_generation;
static volatile bool m_capdu_deferred;

static uint8_t m_rapdu_buf[NFC_14A_4_MAX_RAPDU];
static volatile uint16_t m_rapdu_len;
static uint16_t m_rapdu_offset;
static volatile bool m_response_ready;
static volatile bool m_response_loading;
static volatile bool m_response_started;
static bool m_response_finished;
static volatile uint32_t m_response_generation;

/* ISO-DEP link state. RX and TX block numbers are independent. */
static uint8_t m_rx_block_num;
static uint8_t m_tx_block_num;
static bool m_rx_chain_active;
static bool m_tx_waiting_r_ack;
static volatile bool m_waiting_response;
static bool m_wtx_outstanding;
static volatile bool m_field_active;
static volatile bool m_iso_dep_active;
static uint16_t m_pcd_fsd = ISO_DEP_MAX_FSD;
static uint8_t m_rats_cid;
static bool m_use_cid;
static volatile uint32_t m_transport_generation;
APP_TIMER_DEF(m_wtx_timer);
static bool m_wtx_timer_initialized;
static volatile bool m_wtx_timer_armed;
static uint32_t m_wtx_interval_ticks;
static bool m_async_tx;

/* Last transmitted I-block is retained for R(NAK) and duplicates. */
static uint8_t m_last_i_block[ISO_DEP_FRAME_BYTES];
static uint16_t m_last_i_block_len;
static bool m_last_i_block_valid;
static bool m_last_i_block_chained;
static uint8_t m_tx_buf[ISO_DEP_FRAME_BYTES];

/* Existing debug command counters. */
static uint8_t m_dbg_iblocks_rx;
static uint8_t m_dbg_iblocks_tx;
static uint8_t m_dbg_last_rx_pcb;
static uint8_t m_dbg_last_match;

/* Static APDU response table (RAM copy populated from slot data). */
static nfc_tag_14a_4_static_response_t
    m_static_resp[NFC_14A_4_MAX_STATIC_RESPONSES];
static uint8_t m_static_resp_count;

typedef struct {
    uint8_t cmd[NFC_14A_4_MAX_STATIC_CMD_LEN];
    uint8_t cmd_len;
    uint8_t resp[NFC_14A_4_MAX_LARGE_RESP_LEN];
    uint16_t resp_len;
} nfc_tag_14a_4_large_response_t;

static nfc_tag_14a_4_large_response_t
    m_large_resp[NFC_14A_4_MAX_LARGE_RESPONSES];
static uint8_t m_large_resp_count;

/* Transient bridge profile. It never aliases the persistent slot buffer. */
static nfc_tag_14a_coll_res_entity_t m_runtime_coll_res;
static nfc_tag_14a_4_runtime_callbacks_t m_runtime_callbacks;
static bool m_runtime_installed;
static nfc_tag_14a_handler_t m_saved_14a_handler;
static nfc_tag_14a_4_information_t *m_saved_tag_information;
static bool m_saved_reset_enable;
static uint32_t m_saved_wtx_interval_ticks;

static void nfc_tag_14a_4_state_handler(uint8_t *data, uint16_t sz_bits);
static void nfc_tag_14a_4_field_on_handler(void);
static void nfc_tag_14a_4_field_off_handler(void);
static void nfc_tag_14a_4_rats_handler(uint16_t fsd, uint8_t cid);
static void wtx_timer_handler(void *context);

static bool ensure_wtx_timer(void) {
    if (!m_wtx_timer_initialized) {
        if (app_timer_create(&m_wtx_timer, APP_TIMER_MODE_SINGLE_SHOT,
                             wtx_timer_handler) != NRF_SUCCESS) {
            return false;
        }
        m_wtx_timer_initialized = true;
    }
    return true;
}

static void cancel_wtx_timer(void) {
    bool was_armed = m_wtx_timer_armed;
    m_wtx_timer_armed = false;
    if (m_wtx_timer_initialized && was_armed) {
        (void)app_timer_stop(m_wtx_timer);
    }
}

static bool schedule_wtx_timer(void) {
    cancel_wtx_timer();
    if (!m_wtx_timer_initialized) {
        return false;
    }
    m_wtx_timer_armed = true;
    if (app_timer_start(m_wtx_timer, m_wtx_interval_ticks,
                        (void *)(uintptr_t)m_transport_generation) != NRF_SUCCESS) {
        m_wtx_timer_armed = false;
        return false;
    }
    return true;
}

static void configure_wtx_interval(const nfc_14a_ats_t *ats) {
    uint8_t fwi = 4U;
    if (ats != NULL && ats->length >= 2U && (ats->data[1] & 0x20U) != 0) {
        uint8_t tb_offset = 2U + ((ats->data[1] & 0x10U) != 0 ? 1U : 0U);
        if (tb_offset < ats->length && (ats->data[tb_offset] >> 4U) < 15U) {
            fwi = ats->data[tb_offset] >> 4U;
        }
    }
    /* Half of the granted FWT * WTXM leaves time to request another WTX. */
    uint32_t interval_ms = ((302U << fwi) * WTX_VALUE) / 2000U;
    m_wtx_interval_ticks = APP_TIMER_TICKS(interval_ms);
    if (m_wtx_interval_ticks < APP_TIMER_MIN_TIMEOUT_TICKS) {
        m_wtx_interval_ticks = APP_TIMER_MIN_TIMEOUT_TICKS;
    }
}

static bool is_iblock(uint8_t pcb) {
    return (pcb & PCB_IBLOCK_MASK) == PCB_IBLOCK_BASE;
}

static bool is_rblock(uint8_t pcb) {
    return (pcb & PCB_RBLOCK_MASK) == PCB_RBLOCK_ACK_BASE;
}

static bool is_sblock(uint8_t pcb) {
    return (pcb & 0xC2U) == 0xC2U;
}

static void install_14a_4_handler(void) {
    nfc_tag_14a_handler_t handler = {
        .cb_reset = nfc_tag_14a_4_reset_handler,
        .cb_state = nfc_tag_14a_4_state_handler,
        .get_coll_res = nfc_tag_14a_4_get_coll_res,
        .cb_field_on = nfc_tag_14a_4_field_on_handler,
        .cb_field_off = nfc_tag_14a_4_field_off_handler,
        .cb_rats = nfc_tag_14a_4_rats_handler,
    };
    nfc_tag_14a_set_handler(&handler);
}

static void reset_transport(void) {
    cancel_wtx_timer();
    m_transport_generation++;
    if (m_transport_generation == 0) {
        m_transport_generation = 1;
    }

    m_rx_block_num = 0;
    m_tx_block_num = 0;
    m_rx_chain_active = false;
    m_tx_waiting_r_ack = false;
    m_waiting_response = false;
    m_wtx_outstanding = false;
    m_use_cid = false;
    m_rats_cid = 0;
    m_pcd_fsd = ISO_DEP_MAX_FSD;

    m_capdu_assembly_len = 0;
    m_pending_apdu_len = 0;
    m_apdu_pending = false;
    m_capdu_deferred = false;
    m_rapdu_len = 0;
    m_rapdu_offset = 0;
    m_response_ready = false;
    m_response_started = false;
    m_response_finished = false;
    m_response_generation = 0;
    m_last_i_block_len = 0;
    m_last_i_block_valid = false;
    m_last_i_block_chained = false;
}

static bool response_is_ready(void) {
    return m_response_ready &&
           m_response_generation == m_transport_generation;
}

static void publish_deferred_capdu(void) {
    if (m_capdu_deferred && m_runtime_installed &&
        m_runtime_callbacks.on_capdu != NULL &&
        m_runtime_callbacks.on_capdu(m_capdu_buf, m_pending_apdu_len)) {
        m_capdu_deferred = false;
    }
}

/* ------------------------------------------------------------------ */
/* Static response table                                               */
/* ------------------------------------------------------------------ */

void nfc_tag_14a_4_add_static_response(const uint8_t *cmd, uint8_t cmd_len,
                                       const uint8_t *resp, uint16_t resp_len) {
    if (cmd == NULL || resp == NULL || cmd_len == 0) {
        return;
    }
    if (cmd_len > NFC_14A_4_MAX_STATIC_CMD_LEN) {
        cmd_len = NFC_14A_4_MAX_STATIC_CMD_LEN;
    }

    if (resp_len > NFC_14A_4_MAX_STATIC_RESP_LEN) {
        if (m_large_resp_count >= NFC_14A_4_MAX_LARGE_RESPONSES) {
            return;
        }
        if (resp_len > NFC_14A_4_MAX_LARGE_RESP_LEN) {
            resp_len = NFC_14A_4_MAX_LARGE_RESP_LEN;
        }
        nfc_tag_14a_4_large_response_t *entry =
            &m_large_resp[m_large_resp_count++];
        entry->cmd_len = cmd_len;
        entry->resp_len = resp_len;
        memcpy(entry->cmd, cmd, cmd_len);
        memcpy(entry->resp, resp, resp_len);
        return;
    }

    if (m_static_resp_count >= NFC_14A_4_MAX_STATIC_RESPONSES) {
        return;
    }
    nfc_tag_14a_4_static_response_t *entry =
        &m_static_resp[m_static_resp_count++];
    entry->cmd_len = cmd_len;
    entry->resp_len = (uint8_t)resp_len;
    memcpy(entry->cmd, cmd, cmd_len);
    memcpy(entry->resp, resp, resp_len);

    if (m_tag_information != NULL && !m_runtime_installed &&
        m_tag_information->static_resp_count <
            NFC_14A_4_MAX_STATIC_RESPONSES) {
        memcpy(&m_tag_information->static_resp[
                   m_tag_information->static_resp_count++],
               entry, sizeof(*entry));
    }
}

void nfc_tag_14a_4_clear_static_responses(void) {
    m_static_resp_count = 0;
    m_large_resp_count = 0;
    if (m_tag_information != NULL && !m_runtime_installed) {
        m_tag_information->static_resp_count = 0;
    }
}

static bool find_static_response(const uint8_t *apdu, uint16_t apdu_len,
                                 const uint8_t **resp_out,
                                 uint16_t *resp_len_out) {
    if (m_runtime_installed) {
        return false;
    }

    for (uint8_t i = 0; i < m_static_resp_count; i++) {
        nfc_tag_14a_4_static_response_t *entry = &m_static_resp[i];
        if (apdu_len >= entry->cmd_len &&
            memcmp(apdu, entry->cmd, entry->cmd_len) == 0) {
            *resp_out = entry->resp;
            *resp_len_out = entry->resp_len;
            return true;
        }
    }

    for (uint8_t i = 0; i < m_large_resp_count; i++) {
        nfc_tag_14a_4_large_response_t *entry = &m_large_resp[i];
        if (apdu_len >= entry->cmd_len &&
            memcmp(apdu, entry->cmd, entry->cmd_len) == 0) {
            *resp_out = entry->resp;
            *resp_len_out = entry->resp_len;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* ISO-DEP TX helpers                                                  */
/* ------------------------------------------------------------------ */

static uint8_t append_cid(uint8_t *buf, uint8_t pcb) {
    uint8_t offset = 0;
    buf[offset++] = pcb | (m_use_cid ? PCB_CID_FOLLOWING : 0U);
    if (m_use_cid) {
        buf[offset++] = m_rats_cid & 0x0FU;
    }
    return offset;
}

static void transmit_block(uint8_t *data, uint16_t length) {
    if (m_async_tx) {
        nfc_tag_14a_tx_bytes_now(data, length, true);
    } else {
        nfc_tag_14a_tx_bytes(data, length, true);
    }
}

static bool accept_cid(const uint8_t *data, uint16_t length,
                       uint16_t *offset) {
    bool has_cid = (data[0] & PCB_CID_FOLLOWING) != 0;
    *offset = 1;

    if (has_cid) {
        if (length <= *offset || (data[*offset] & 0xF0U) != 0 ||
            (data[*offset] & 0x0FU) != m_rats_cid) {
            return false;
        }
        m_use_cid = true;
        (*offset)++;
    } else if (m_use_cid) {
        return false;
    }
    return true;
}

static void send_rblock(bool nak) {
    uint8_t pcb = (nak ? PCB_RBLOCK_NAK_BASE : PCB_RBLOCK_ACK_BASE) |
                  (m_rx_block_num & PCB_BLOCK_NUM);
    uint8_t length = append_cid(m_tx_buf, pcb);
    transmit_block(m_tx_buf, length);
}

static void send_wtx(void) {
    uint8_t length = append_cid(m_tx_buf, PCB_SBLOCK_WTX);
    m_tx_buf[length++] = WTX_VALUE;
    m_wtx_outstanding = true;
    transmit_block(m_tx_buf, length);
}

static void retransmit_last_i_block(void) {
    if (m_last_i_block_valid && m_last_i_block_len > 0) {
        transmit_block(m_last_i_block, m_last_i_block_len);
    }
}

static bool prepare_response(const uint8_t *data, uint16_t length) {
    if (data == NULL || length == 0 || length > NFC_14A_4_MAX_RAPDU ||
        m_response_loading) {
        return false;
    }
    memcpy(m_rapdu_buf, data, length);
    m_rapdu_len = length;
    m_rapdu_offset = 0;
    m_response_generation = m_transport_generation;
    __DMB();
    m_response_ready = true;
    return true;
}

static bool send_next_response_block(void) {
    uint16_t prologue_len = 1U + (m_use_cid ? 1U : 0U);
    if (m_pcd_fsd < ISO_DEP_MIN_FSD ||
        m_pcd_fsd > ISO_DEP_MAX_FSD ||
        m_pcd_fsd <= prologue_len + NFC_TAG_14A_CRC_LENGTH ||
        m_rapdu_offset >= m_rapdu_len) {
        return false;
    }

    uint16_t max_inf = m_pcd_fsd - prologue_len -
                       NFC_TAG_14A_CRC_LENGTH;
    uint16_t remaining = m_rapdu_len - m_rapdu_offset;
    uint16_t fragment_len = remaining > max_inf ? max_inf : remaining;
    bool chained = fragment_len < remaining;

    uint8_t pcb = PCB_IBLOCK_BASE |
                  (m_tx_block_num & PCB_BLOCK_NUM) |
                  (chained ? PCB_CHAIN : 0U);
    uint16_t frame_len = append_cid(m_tx_buf, pcb);
    memcpy(&m_tx_buf[frame_len], &m_rapdu_buf[m_rapdu_offset],
           fragment_len);
    frame_len += fragment_len;

    memcpy(m_last_i_block, m_tx_buf, frame_len);
    m_last_i_block_len = frame_len;
    m_last_i_block_valid = true;
    m_last_i_block_chained = chained;

    NRF_LOG_INFO("14A-4 TX I-block: pcb=%02x offset=%u inf=%u chain=%u",
                 pcb, m_rapdu_offset, fragment_len, chained ? 1U : 0U);

    transmit_block(m_tx_buf, frame_len);
    m_dbg_iblocks_tx++;
    m_rapdu_offset += fragment_len;
    m_tx_block_num ^= 1U;
    m_tx_waiting_r_ack = chained;

    if (!chained) {
        m_response_finished = true;
        m_waiting_response = false;
        NRF_LOG_INFO("14A-4 RAPDU TX final: sent=%u total=%u",
                     m_rapdu_offset, m_rapdu_len);
    }
    return true;
}

static void start_response(void) {
    if (!response_is_ready() || !m_waiting_response) {
        return;
    }
    cancel_wtx_timer();
    m_response_ready = false;
    m_response_started = true;
    m_response_finished = false;
    m_wtx_outstanding = false;
    m_rapdu_offset = 0;
    if (!send_next_response_block()) {
        reset_transport();
    }
}

static void wtx_timer_handler(void *context) {
    uint8_t nested = 0;
    app_util_critical_region_enter(&nested);
    if (m_wtx_timer_armed &&
        (uint32_t)(uintptr_t)context == m_transport_generation &&
        m_field_active && m_waiting_response && !m_response_started &&
        !m_wtx_outstanding) {
        m_wtx_timer_armed = false;
        publish_deferred_capdu();
        m_async_tx = true;
        if (response_is_ready()) {
            start_response();
        } else {
            send_wtx();
        }
        m_async_tx = false;
    }
    app_util_critical_region_exit(nested);
}

/* ------------------------------------------------------------------ */
/* ISO-DEP RX handlers                                                 */
/* ------------------------------------------------------------------ */

static void handle_sblock(uint8_t *data, uint16_t length) {
    uint8_t pcb = data[0];
    uint16_t offset;
    if (!accept_cid(data, length, &offset)) {
        return;
    }

    if ((pcb & 0xF7U) == PCB_SBLOCK_DESELECT) {
        if (length != offset) {
            return;
        }
        uint8_t tx_len = append_cid(m_tx_buf, PCB_SBLOCK_DESELECT);
        nfc_tag_14a_4_field_off_handler();
        nfc_tag_14a_tx_bytes(m_tx_buf, tx_len, true);
        return;
    }

    if ((pcb & 0xF7U) == PCB_SBLOCK_WTX) {
        if (length != offset + 1U) {
            return;
        }
        if (!m_wtx_outstanding || data[offset] != WTX_VALUE) {
            return;
        }

        m_wtx_outstanding = false;
        publish_deferred_capdu();
        if (response_is_ready()) {
            start_response();
        } else if (m_waiting_response) {
            if (!schedule_wtx_timer()) {
                send_wtx();
            }
        }
    }
}

static void handle_rblock(uint8_t *data, uint16_t length) {
    uint16_t offset;
    if (!accept_cid(data, length, &offset) || length != offset) {
        return;
    }
    if (!m_last_i_block_valid) {
        return;
    }

    uint8_t pcb = data[0];
    uint8_t requested_block = pcb & PCB_BLOCK_NUM;
    bool nak = (pcb & PCB_CHAIN) != 0;

    NRF_LOG_INFO("14A-4 RX R-block: pcb=%02x block=%u nak=%u expected=%u",
                 pcb, requested_block, nak ? 1U : 0U,
                 m_tx_block_num & PCB_BLOCK_NUM);

    if (nak || requested_block != (m_tx_block_num & PCB_BLOCK_NUM)) {
        NRF_LOG_INFO("14A-4 retransmit I-block: pcb=%02x len=%u",
                     m_last_i_block[0], m_last_i_block_len);
        retransmit_last_i_block();
        return;
    }

    m_tx_waiting_r_ack = false;
    if (m_last_i_block_chained && m_rapdu_offset < m_rapdu_len) {
        (void)send_next_response_block();
    } else {
        m_last_i_block_valid = false;
    }
}

static void finish_capdu(void) {
    m_capdu_generation++;
    m_pending_apdu_len = m_capdu_assembly_len;
    __DMB();
    m_apdu_pending = true;
    m_waiting_response = true;
    m_response_ready = false;
    m_response_started = false;
    m_response_finished = false;
    m_rapdu_len = 0;
    m_rapdu_offset = 0;
    m_last_i_block_valid = false;
    m_tx_waiting_r_ack = false;

    if (m_runtime_installed && m_runtime_callbacks.on_capdu != NULL) {
        m_capdu_deferred = true;
        publish_deferred_capdu();
    }

    const uint8_t *static_resp = NULL;
    uint16_t static_len = 0;
    bool found = find_static_response(m_capdu_buf, m_capdu_assembly_len,
                                      &static_resp, &static_len);
    m_dbg_last_match = found ? 1U : 0U;
    if (found && prepare_response(static_resp, static_len)) {
        start_response();
    } else if (response_is_ready()) {
        start_response();
    } else {
        send_wtx();
    }
}

static void handle_iblock(uint8_t *data, uint16_t length) {
    uint8_t pcb = data[0];
    uint16_t offset;
    if (!accept_cid(data, length, &offset)) {
        send_rblock(true);
        return;
    }

    if ((pcb & PCB_NAD_FOLLOWING) != 0) {
        /* NAD is not used by NFC FIDO. Reject it rather than misroute it. */
        send_rblock(true);
        return;
    }
    if (offset >= length) {
        send_rblock(true);
        return;
    }

    uint8_t reader_block = pcb & PCB_BLOCK_NUM;
    bool chained = (pcb & PCB_CHAIN) != 0;
    m_dbg_iblocks_rx++;
    m_dbg_last_rx_pcb = pcb;

    if (m_tx_waiting_r_ack) {
        retransmit_last_i_block();
        return;
    }

    if (reader_block != (m_rx_block_num & PCB_BLOCK_NUM)) {
        if (m_last_i_block_valid) {
            retransmit_last_i_block();
        } else if (m_waiting_response) {
            send_wtx();
        } else {
            send_rblock(false);
        }
        return;
    }

    if (m_waiting_response && !m_response_finished) {
        send_wtx();
        return;
    }

    if (m_response_started || m_response_finished) {
        m_response_started = false;
        m_response_finished = false;
        m_rapdu_len = 0;
        m_rapdu_offset = 0;
        m_last_i_block_valid = false;
        m_tx_waiting_r_ack = false;
    }

    if (!m_rx_chain_active) {
        /* A previous static response may still have an unread debug APDU. */
        m_apdu_pending = false;
        m_pending_apdu_len = 0;
        m_capdu_generation++;
        m_capdu_assembly_len = 0;
    }

    uint16_t inf_len = length - offset;
    if (inf_len > NFC_14A_4_MAX_CAPDU - m_capdu_assembly_len) {
        send_rblock(true);
        return;
    }

    memcpy(&m_capdu_buf[m_capdu_assembly_len], &data[offset], inf_len);
    m_capdu_assembly_len += inf_len;
    m_rx_block_num ^= 1U;

    if (chained) {
        m_rx_chain_active = true;
        send_rblock(false);
        return;
    }

    m_rx_chain_active = false;
    finish_capdu();
}

/* Called from the NFCT IRQ. sz_bits remains bit-based for compatibility with
 * every other 14A handler; ISO-DEP frames must be byte-aligned and CRC-valid. */
static void nfc_tag_14a_4_state_handler(uint8_t *data, uint16_t sz_bits) {
    if (!m_field_active || !m_iso_dep_active || data == NULL || sz_bits < 24U ||
        (sz_bits & 7U) != 0) {
        return;
    }

    uint16_t frame_len = sz_bits / 8U;
    if (!nfc_tag_14a_checks_crc(data, frame_len)) {
        if (frame_len > NFC_TAG_14A_CRC_LENGTH && is_iblock(data[0])) {
            send_rblock(true);
        }
        return;
    }
    frame_len -= NFC_TAG_14A_CRC_LENGTH;
    if (frame_len == 0) {
        return;
    }

    uint8_t pcb = data[0];
    if (is_sblock(pcb)) {
        handle_sblock(data, frame_len);
    } else if (is_rblock(pcb)) {
        handle_rblock(data, frame_len);
    } else if (is_iblock(pcb)) {
        handle_iblock(data, frame_len);
    }
}

/* ------------------------------------------------------------------ */
/* Relay and lifecycle API                                             */
/* ------------------------------------------------------------------ */

bool nfc_tag_14a_4_get_pending_apdu(uint8_t *buf, uint16_t *length) {
    if (buf == NULL || length == NULL || !m_apdu_pending) {
        return false;
    }

    uint32_t generation = m_transport_generation;
    uint32_t capdu_generation = m_capdu_generation;
    uint16_t pending_len = m_pending_apdu_len;
    if (pending_len == 0 || pending_len > NFC_14A_4_MAX_CAPDU) {
        return false;
    }
    memcpy(buf, m_capdu_buf, pending_len);
    __DMB();
    uint8_t nested = 0;
    app_util_critical_region_enter(&nested);
    bool valid = m_apdu_pending && pending_len == m_pending_apdu_len &&
                 generation == m_transport_generation &&
                 capdu_generation == m_capdu_generation;
    if (valid) {
        m_apdu_pending = false;
    }
    app_util_critical_region_exit(nested);
    if (!valid) {
        return false;
    }
    *length = pending_len;
    return true;
}

bool nfc_tag_14a_4_submit_response(const uint8_t *data, uint16_t length) {
    if (data == NULL || length == 0 || length > NFC_14A_4_MAX_RAPDU) {
        return false;
    }

    uint8_t nested = 0;
    uint32_t generation = 0;
    app_util_critical_region_enter(&nested);
    bool accepted = !m_response_loading && m_field_active &&
                    m_waiting_response && !m_response_ready &&
                    !m_response_started && !m_capdu_deferred;
    if (accepted) {
        m_response_loading = true;
        generation = m_transport_generation;
    }
    app_util_critical_region_exit(nested);
    if (!accepted) {
        return false;
    }

    memcpy(m_rapdu_buf, data, length);

    app_util_critical_region_enter(&nested);
    accepted = generation == m_transport_generation && m_field_active &&
               m_waiting_response && !m_response_ready && !m_response_started;
    if (accepted) {
        m_rapdu_len = length;
        m_rapdu_offset = 0;
        m_response_generation = generation;
        m_apdu_pending = false;
        __DMB();
        m_response_ready = true;
        if (!m_wtx_outstanding) {
            m_async_tx = true;
            start_response();
            m_async_tx = false;
        }
    }
    m_response_loading = false;
    app_util_critical_region_exit(nested);
    return accepted;
}

void nfc_tag_14a_4_set_response(const uint8_t *data, uint16_t length) {
    (void)nfc_tag_14a_4_submit_response(data, length);
}

void nfc_tag_14a_4_reset_handler(void) {
    /* REQA/WUPA and a repeated RATS both start a new ISO-DEP activation.
     * End the old bridge session even when its last RAPDU completed cleanly;
     * otherwise the next RATS would inherit m_iso_dep_active and suppress the
     * FIELD_ON notification for the new session. */
    if (m_runtime_installed && m_field_active && m_iso_dep_active) {
        nfc_tag_14a_4_field_off_handler();
        nfc_tag_14a_4_field_on_handler();
    } else {
        reset_transport();
    }
}

void nfc_tag_14a_4_abort(void) {
    uint8_t nested = 0;
    app_util_critical_region_enter(&nested);
    nfc_tag_14a_abort_tx();
    m_field_active = false;
    m_iso_dep_active = false;
    reset_transport();
    app_util_critical_region_exit(nested);
}

static void nfc_tag_14a_4_field_on_handler(void) {
    reset_transport();
    m_field_active = true;
    m_iso_dep_active = false;
}

static void nfc_tag_14a_4_field_off_handler(void) {
    bool report_field_off = m_iso_dep_active;
    if (m_response_started &&
        (m_tx_waiting_r_ack || m_rapdu_offset < m_rapdu_len)) {
        NRF_LOG_WARNING("14A-4 field lost during RAPDU: sent=%u total=%u",
                        m_rapdu_offset, m_rapdu_len);
        NRF_LOG_WARNING("14A-4 field loss state: waiting_ack=%u last_pcb=%02x",
                        m_tx_waiting_r_ack ? 1U : 0U,
                        m_last_i_block_valid ? m_last_i_block[0] : 0U);
    }
    nfc_tag_14a_abort_tx();
    m_field_active = false;
    m_iso_dep_active = false;
    reset_transport();
    if (report_field_off && m_runtime_installed &&
        m_runtime_callbacks.on_field_off != NULL) {
        m_runtime_callbacks.on_field_off();
    }
}

static void nfc_tag_14a_4_rats_handler(uint16_t fsd, uint8_t cid) {
    if (!m_field_active) {
        nfc_tag_14a_4_field_on_handler();
    }
    if (fsd < ISO_DEP_MIN_FSD || fsd > ISO_DEP_MAX_FSD) {
        fsd = ISO_DEP_MAX_FSD;
    }
    m_pcd_fsd = fsd;
    m_rats_cid = cid & 0x0FU;
    m_use_cid = m_rats_cid != 0;
    NRF_LOG_INFO("14A-4 RATS: fsd=%u cid=%u use_cid=%u",
                 m_pcd_fsd, m_rats_cid, m_use_cid ? 1U : 0U);
    if (!m_iso_dep_active) {
        m_iso_dep_active = true;
        if (m_runtime_installed && m_runtime_callbacks.on_field_on != NULL) {
            m_runtime_callbacks.on_field_on();
        }
    }
}

bool nfc_tag_14a_4_runtime_install(
    const nfc_tag_14a_coll_res_entity_t *profile,
    const nfc_tag_14a_4_runtime_callbacks_t *callbacks) {
    if (profile == NULL || !is_valid_uid_size(profile->size) ||
        profile->ats.length < 2U ||
        profile->ats.data[0] != profile->ats.length ||
        (profile->sak[0] & 0x20U) == 0) {
        return false;
    }
    if (!ensure_wtx_timer()) {
        return false;
    }

    if (!m_runtime_installed) {
        nfc_tag_14a_get_handler(&m_saved_14a_handler);
        m_saved_tag_information = m_tag_information;
        m_saved_reset_enable = nfc_tag_14a_is_reset_enable();
        m_saved_wtx_interval_ticks = m_wtx_interval_ticks;
    }

    memcpy(&m_runtime_coll_res, profile, sizeof(*profile));
    configure_wtx_interval(&profile->ats);
    if (callbacks != NULL) {
        m_runtime_callbacks = *callbacks;
    } else {
        memset(&m_runtime_callbacks, 0, sizeof(m_runtime_callbacks));
    }

    m_runtime_installed = true;
    nfc_tag_14a_set_reset_enable(false);
    install_14a_4_handler();
    m_field_active = false;
    m_iso_dep_active = false;
    reset_transport();
    return true;
}

void nfc_tag_14a_4_runtime_uninstall(void) {
    if (!m_runtime_installed) {
        return;
    }

    nfc_tag_14a_4_abort();
    m_runtime_installed = false;
    memset(&m_runtime_callbacks, 0, sizeof(m_runtime_callbacks));
    m_tag_information = m_saved_tag_information;
    m_wtx_interval_ticks = m_saved_wtx_interval_ticks;
    nfc_tag_14a_set_handler(&m_saved_14a_handler);
    nfc_tag_14a_set_reset_enable(m_saved_reset_enable);
    memset(&m_saved_14a_handler, 0, sizeof(m_saved_14a_handler));
    m_saved_tag_information = NULL;
}

bool nfc_tag_14a_4_runtime_is_installed(void) {
    return m_runtime_installed;
}

void nfc_tag_14a_4_get_debug_counters(uint8_t *rx, uint8_t *tx,
                                      uint8_t *last_pcb,
                                      uint8_t *last_match) {
    if (rx != NULL) {
        *rx = m_dbg_iblocks_rx;
    }
    if (tx != NULL) {
        *tx = m_dbg_iblocks_tx;
    }
    if (last_pcb != NULL) {
        *last_pcb = m_dbg_last_rx_pcb;
    }
    if (last_match != NULL) {
        *last_match = m_dbg_last_match;
    }
}

/* ------------------------------------------------------------------ */
/* Collision data and persistent slot callbacks                        */
/* ------------------------------------------------------------------ */

nfc_tag_14a_coll_res_reference_t *nfc_tag_14a_4_get_coll_res(void) {
    if (m_tag_information == NULL && !m_runtime_installed) {
        return NULL;
    }
    nfc_tag_14a_coll_res_entity_t *coll_res = m_runtime_installed ?
        &m_runtime_coll_res : &m_tag_information->res_coll;
    m_shadow_coll_res.sak = coll_res->sak;
    m_shadow_coll_res.atqa = coll_res->atqa;
    m_shadow_coll_res.uid = coll_res->uid;
    m_shadow_coll_res.size = &coll_res->size;
    m_shadow_coll_res.ats = &coll_res->ats;
    return &m_shadow_coll_res;
}

int nfc_tag_14a_4_data_loadcb(tag_specific_type_t type,
                              tag_data_buffer_t *buffer) {
    (void)type;
    int info_size = sizeof(nfc_tag_14a_4_information_t);
    if (buffer == NULL || buffer->buffer == NULL ||
        buffer->length < info_size) {
        NRF_LOG_ERROR("14A-4 load buffer too small");
        return info_size;
    }

    m_tag_information = (nfc_tag_14a_4_information_t *)buffer->buffer;
    if (!ensure_wtx_timer()) {
        NRF_LOG_ERROR("14A-4 WTX timer unavailable");
    }
    configure_wtx_interval(&m_tag_information->res_coll.ats);
    m_static_resp_count = m_tag_information->static_resp_count;
    if (m_static_resp_count > NFC_14A_4_MAX_STATIC_RESPONSES) {
        m_static_resp_count = NFC_14A_4_MAX_STATIC_RESPONSES;
    }
    memcpy(m_static_resp, m_tag_information->static_resp,
           m_static_resp_count * sizeof(nfc_tag_14a_4_static_response_t));

    install_14a_4_handler();
    reset_transport();
    return info_size;
}

int nfc_tag_14a_4_data_savecb(tag_specific_type_t type,
                              tag_data_buffer_t *buffer) {
    (void)type;
    (void)buffer;
    return sizeof(nfc_tag_14a_4_information_t);
}

bool nfc_tag_14a_4_data_factory(uint8_t slot,
                                tag_specific_type_t tag_type) {
    if (tag_type != TAG_TYPE_HF14A_4) {
        return false;
    }

    nfc_tag_14a_4_information_t info;
    memset(&info, 0, sizeof(info));

    info.res_coll.size = NFC_TAG_14A_UID_DOUBLE_SIZE;
    info.res_coll.atqa[0] = 0x04;
    info.res_coll.atqa[1] = 0x00;
    info.res_coll.sak[0] = 0x20;
    info.res_coll.uid[0] = 0x04;
    info.res_coll.uid[1] = 0x01;
    info.res_coll.uid[2] = 0x02;
    info.res_coll.uid[3] = 0x03;
    info.res_coll.uid[4] = 0x04;
    info.res_coll.uid[5] = 0x05;
    info.res_coll.uid[6] = 0x06;

    static const uint8_t default_ats[] = {
        0x10, 0x78, 0x80, 0x70, 0x02, 0x00, 0x31, 0xC1,
        0x64, 0x09, 0x97, 0x61, 0x26, 0x00, 0x90, 0x00,
    };
    info.res_coll.ats.length = sizeof(default_ats);
    memcpy(info.res_coll.ats.data, default_ats, sizeof(default_ats));

    fds_slot_record_map_t map_info;
    get_fds_map_by_slot_sense_type_for_dump(slot, TAG_SENSE_HF,
                                             &map_info);
    return fds_write_sync(map_info.id, map_info.key, sizeof(info), &info);
}
