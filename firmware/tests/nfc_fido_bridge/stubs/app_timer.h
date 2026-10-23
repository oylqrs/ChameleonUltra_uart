#ifndef TEST_APP_TIMER_H
#define TEST_APP_TIMER_H

#include <stdint.h>

#define APP_TIMER_TICKS(ms) ((uint32_t)(ms))

uint32_t app_timer_cnt_get(void);
uint32_t app_timer_cnt_diff_compute(uint32_t now, uint32_t previous);

#endif
