#ifndef DATAFRAME_H
#define DATAFRAME_H

#include <stdint.h>
#include <stdbool.h>

// Data frame process callback
typedef void (*data_frame_cbk_t)(uint16_t cmd, uint16_t status, uint16_t length, uint8_t *data);
typedef void (*data_frame_source_pump_t)(void);

typedef enum {
    DATA_FRAME_SOURCE_USB,
    DATA_FRAME_SOURCE_BLE,
    DATA_FRAME_SOURCE_UART,
    DATA_FRAME_SOURCE_COUNT,
} data_frame_source_t;

// TX buffer
typedef struct {
    uint8_t *const buffer;
    uint16_t length;
} data_frame_tx_t;

void data_frame_receive(uint8_t *data, uint16_t length);
void data_frame_receive_from(data_frame_source_t source, const uint8_t *data, uint16_t length);
void data_frame_set_source(data_frame_source_t source);
data_frame_source_t data_frame_get_source(void);
bool data_frame_source_ready(data_frame_source_t source);
void data_frame_reset_source(data_frame_source_t source);
void data_frame_register_source_pump(data_frame_source_t source, data_frame_source_pump_t pump);
void data_frame_process(void);
void on_data_frame_complete(data_frame_cbk_t callback);

data_frame_tx_t *data_frame_make(
    uint16_t cmd,
    uint16_t status,
    uint16_t length,
    uint8_t *data
);


#endif // DATAFRAME_H
