#include "dataframe.h"
#include "netdata.h"

#define NRF_LOG_MODULE_NAME data_frame
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
NRF_LOG_MODULE_REGISTER();

static netdata_frame_raw_t m_netdata_frame_tx_buf;
static data_frame_tx_t m_frame_tx_buf_info = {
    .buffer = (uint8_t *) &m_netdata_frame_tx_buf,  // default buffer
};

typedef struct {
    netdata_frame_raw_t frame;
    uint16_t position;
    uint16_t cmd;
    uint16_t status;
    uint16_t data_len;
    volatile bool completed;
} data_frame_rx_state_t;

static data_frame_rx_state_t m_rx_states[DATA_FRAME_SOURCE_COUNT];
static data_frame_source_pump_t m_source_pumps[DATA_FRAME_SOURCE_COUNT];
static data_frame_cbk_t m_frame_process_cbk = NULL;
static volatile data_frame_source_t m_receive_source = DATA_FRAME_SOURCE_USB;
static data_frame_source_t m_frame_source = DATA_FRAME_SOURCE_USB;
static uint8_t m_next_process_source;

static bool source_is_valid(data_frame_source_t source) {
    return (uint32_t)source < (uint32_t)DATA_FRAME_SOURCE_COUNT;
}

void data_frame_set_source(data_frame_source_t source) {
    if (source_is_valid(source)) {
        m_receive_source = source;
    }
}

data_frame_source_t data_frame_get_source(void) {
    return m_frame_source;
}

static uint8_t compute_lrc(const uint8_t *buf, uint16_t bufsize) {
    uint8_t lrc = 0x00;
    for (uint16_t i = 0; i < bufsize; i++) {
        lrc += buf[i];
    }
    return 0x100 - lrc;
}

//
//  !!!!!!!!!!!!!!!!! NRF_LOG_HEXDUMP_INFO() printing long data can cause freezing and needs to be fixed. !!!!!!!!!!!!!!!!!
//  FIXME.
//

/**
 * @brief: create a packet, put the created data packet into the buffer, and wait for the post to set up a non busy state
 * @param cmd: instructionResponse
 * @param status:responseStatus
 * @param length: answerDataLength
 * @param data: answerData
 */
data_frame_tx_t *data_frame_make(uint16_t cmd, uint16_t status, uint16_t data_length, uint8_t *data) {
    if (data_length > 0 && data == NULL) {
        NRF_LOG_ERROR("data_frame_make error, null pointer.");
        return NULL;
    }
    if (data_length > NETDATA_MAX_DATA_LENGTH) {
        NRF_LOG_ERROR("data_frame_make error, too much data.");
        return NULL;
    }

    // NRF_LOG_INFO("TX Data frame: cmd = 0x%04x (%i), status = 0x%04x, length = %d%s", cmd, cmd, status, data_length, data_length > 0 ? ", data =" : "");
    // if (data_length > 0) {
    //     NRF_LOG_HEXDUMP_INFO(data, data_length);
    // }

    netdata_frame_postamble_t *tx_post = (netdata_frame_postamble_t *)((uint8_t *)&m_netdata_frame_tx_buf + sizeof(netdata_frame_preamble_t) + data_length);
    // sof
    m_netdata_frame_tx_buf.pre.sof = NETDATA_FRAME_SOF;
    // sof lrc
    m_netdata_frame_tx_buf.pre.lrc1 = compute_lrc((uint8_t *)&m_netdata_frame_tx_buf.pre, offsetof(netdata_frame_preamble_t, lrc1));
    // cmd
    m_netdata_frame_tx_buf.pre.cmd = U16HTONS(cmd);
    // status
    m_netdata_frame_tx_buf.pre.status = U16HTONS(status);
    // data_length
    m_netdata_frame_tx_buf.pre.len = U16HTONS(data_length);
    // head lrc
    m_netdata_frame_tx_buf.pre.lrc2 = compute_lrc((uint8_t *)&m_netdata_frame_tx_buf.pre, offsetof(netdata_frame_preamble_t, lrc2));
    // data
    if (data_length > 0) {
        memcpy(&m_netdata_frame_tx_buf.data, data, data_length);
    }
    // length out.
    m_frame_tx_buf_info.length = (sizeof(netdata_frame_preamble_t) + data_length + sizeof(netdata_frame_postamble_t));
    // data all lrc
    tx_post->lrc3 = compute_lrc((uint8_t *)&m_netdata_frame_tx_buf.data, data_length);
    return (&m_frame_tx_buf_info);
}

/**
 * @brief Data frame reset
 */
void data_frame_reset_source(data_frame_source_t source) {
    if (!source_is_valid(source)) {
        return;
    }

    data_frame_rx_state_t *state = &m_rx_states[source];
    state->position = 0;
    state->cmd = 0;
    state->status = 0;
    state->data_len = 0;
    state->completed = false;
}

bool data_frame_source_ready(data_frame_source_t source) {
    return source_is_valid(source) && m_rx_states[source].completed;
}

void data_frame_register_source_pump(data_frame_source_t source, data_frame_source_pump_t pump) {
    if (source_is_valid(source)) {
        m_source_pumps[source] = pump;
    }
}

static void reset_after_error(data_frame_source_t source, uint8_t byte) {
    data_frame_reset_source(source);
    if (byte == NETDATA_FRAME_SOF) {
        m_rx_states[source].frame.pre.sof = byte;
        m_rx_states[source].position = 1;
    }
}

/**
 * @brief Package receiving, which is used to receive the sent from the data packet and perform splicing processing
 * @param data: Receive byte array
 * @param length:The length of the receiving byte array
 */
void data_frame_receive_from(data_frame_source_t source, const uint8_t *data, uint16_t length) {
    if (!source_is_valid(source) || data == NULL || length == 0) {
        return;
    }

    data_frame_rx_state_t *state = &m_rx_states[source];
    if (state->completed) {
        NRF_LOG_WARNING("Data frame source %d wait process.", source);
        return;
    }

    for (uint16_t i = 0; i < length && !state->completed; i++) {
        uint8_t byte = data[i];

        if (state->position == 0 && byte != NETDATA_FRAME_SOF) {
            continue;
        }
        if (state->position >= sizeof(state->frame)) {
            NRF_LOG_ERROR("Data frame source %d overflow.", source);
            reset_after_error(source, byte);
            continue;
        }

        ((uint8_t *)&state->frame)[state->position++] = byte;

        if (state->position == offsetof(netdata_frame_preamble_t, cmd)) {
            if (state->frame.pre.lrc1 != compute_lrc((const uint8_t *)&state->frame.pre,
                                                     offsetof(netdata_frame_preamble_t, lrc1))) {
                NRF_LOG_ERROR("Data frame source %d SOF LRC error.", source);
                reset_after_error(source, byte);
            }
            continue;
        }

        if (state->position == sizeof(netdata_frame_preamble_t)) {
            if (state->frame.pre.lrc2 != compute_lrc((const uint8_t *)&state->frame.pre,
                                                     offsetof(netdata_frame_preamble_t, lrc2))) {
                NRF_LOG_ERROR("Data frame source %d header LRC error.", source);
                reset_after_error(source, byte);
                continue;
            }

            state->cmd = U16NTOHS(state->frame.pre.cmd);
            state->status = U16NTOHS(state->frame.pre.status);
            state->data_len = U16NTOHS(state->frame.pre.len);
            if (state->data_len > NETDATA_MAX_DATA_LENGTH) {
                NRF_LOG_ERROR("Data frame source %d data length too large.", source);
                reset_after_error(source, byte);
            }
            continue;
        }

        uint32_t frame_length = sizeof(netdata_frame_preamble_t) +
                                state->data_len + sizeof(netdata_frame_postamble_t);
        if (state->position == frame_length) {
            netdata_frame_postamble_t *post = (netdata_frame_postamble_t *)
                ((uint8_t *)&state->frame + sizeof(netdata_frame_preamble_t) + state->data_len);
            if (post->lrc3 != compute_lrc(state->frame.data, state->data_len)) {
                NRF_LOG_ERROR("Data frame source %d data LRC error.", source);
                reset_after_error(source, byte);
                continue;
            }
            state->completed = true;
        }
    }
}

void data_frame_receive(uint8_t *data, uint16_t length) {
    data_frame_receive_from(m_receive_source, data, length);
}

/**
 * @brief After the data packet processing, when the received data forms a complete frame,
 *         This function will be distributed processing tasks through this function, which will be adjusted to notify the data processing of the data
 * If the data processing is time -consuming operation, you need to put this function in the main loop to call
 */
void data_frame_process(void) {
    for (uint8_t source = 0; source < (uint8_t)DATA_FRAME_SOURCE_COUNT; source++) {
        if (m_source_pumps[source] != NULL) {
            m_source_pumps[source]();
        }
    }

    for (uint8_t i = 0; i < (uint8_t)DATA_FRAME_SOURCE_COUNT; i++) {
        data_frame_source_t source = (data_frame_source_t)
            ((m_next_process_source + i) % DATA_FRAME_SOURCE_COUNT);
        data_frame_rx_state_t *state = &m_rx_states[source];
        if (!state->completed) {
            continue;
        }

        m_frame_source = source;
        if (m_frame_process_cbk != NULL) {
            uint8_t *data = state->data_len > 0 ? state->frame.data : NULL;
            m_frame_process_cbk(state->cmd, state->status, state->data_len, data);
        }
        data_frame_reset_source(source);
        m_next_process_source = (uint8_t)((source + 1) % DATA_FRAME_SOURCE_COUNT);
        break;
    }
}

/**
 * @brief Package processing registration registration
 */
void on_data_frame_complete(data_frame_cbk_t callback) {
    m_frame_process_cbk = callback;
}
