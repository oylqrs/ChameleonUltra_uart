#ifndef TEST_BRIDGE_RF_NRF_H
#define TEST_BRIDGE_RF_NRF_H

#include <stdint.h>
#include "../../nfc_iso_dep/stubs/nrf.h"

typedef struct {
    uint32_t DEVICEID[2];
} bridge_rf_ficr_t;

extern bridge_rf_ficr_t bridge_rf_ficr;
#define NRF_FICR (&bridge_rf_ficr)

#endif
