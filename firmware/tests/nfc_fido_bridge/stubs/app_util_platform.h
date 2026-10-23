#ifndef TEST_APP_UTIL_PLATFORM_H
#define TEST_APP_UTIL_PLATFORM_H

#include <stdint.h>

uint8_t bridge_test_critical_enter(void);
void bridge_test_critical_exit(uint8_t nested);

/* Match the lexical scope of the SDK macros with SOFTDEVICE_PRESENT. */
#define CRITICAL_REGION_ENTER() { uint8_t test_nested = bridge_test_critical_enter();
#define CRITICAL_REGION_EXIT() bridge_test_critical_exit(test_nested); }

#endif
