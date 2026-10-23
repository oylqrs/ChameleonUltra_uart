#ifndef TEST_BRIDGE_RF_APP_UTIL_PLATFORM_H
#define TEST_BRIDGE_RF_APP_UTIL_PLATFORM_H

#include "../../nfc_iso_dep/stubs/app_util_platform.h"

#define CRITICAL_REGION_ENTER() \
    { uint8_t test_nested = 0; app_util_critical_region_enter(&test_nested);
#define CRITICAL_REGION_EXIT() \
    app_util_critical_region_exit(test_nested); }

#endif
