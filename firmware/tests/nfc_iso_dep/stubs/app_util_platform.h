#ifndef TEST_APP_UTIL_PLATFORM_H
#define TEST_APP_UTIL_PLATFORM_H
#include <stdint.h>
void app_util_critical_region_enter(uint8_t *nested);
void app_util_critical_region_exit(uint8_t nested);
#endif
