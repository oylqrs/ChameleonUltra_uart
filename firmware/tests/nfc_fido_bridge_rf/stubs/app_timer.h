#ifndef TEST_BRIDGE_RF_APP_TIMER_H
#define TEST_BRIDGE_RF_APP_TIMER_H

#include "../../nfc_iso_dep/stubs/app_timer.h"

uint32_t app_timer_cnt_get(void);
uint32_t app_timer_cnt_diff_compute(uint32_t now, uint32_t previous);

#endif
