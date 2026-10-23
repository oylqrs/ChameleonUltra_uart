#ifndef TEST_SYSSLEEP_H
#define TEST_SYSSLEEP_H

#include <stdint.h>

#define SLEEP_DELAY_MS_BUTTON_CLICK 4000u

void sleep_timer_stop(void);
void sleep_timer_start(uint32_t milliseconds);

#endif
