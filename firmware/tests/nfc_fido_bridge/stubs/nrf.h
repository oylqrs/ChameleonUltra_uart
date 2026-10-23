#ifndef TEST_NRF_H
#define TEST_NRF_H

#include <stdint.h>

typedef struct {
    uint32_t DEVICEID[2];
} bridge_test_ficr_t;

extern bridge_test_ficr_t bridge_test_ficr;
#define NRF_FICR (&bridge_test_ficr)

#endif
