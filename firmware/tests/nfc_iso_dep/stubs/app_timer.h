#ifndef TEST_APP_TIMER_H
#define TEST_APP_TIMER_H
#include <stdbool.h>
#include <stdint.h>
typedef void (*app_timer_timeout_handler_t)(void *);
typedef struct {
    app_timer_timeout_handler_t handler;
    void *context;
    bool armed;
    uint32_t ticks;
} test_timer_t;
typedef test_timer_t *app_timer_id_t;
#define APP_TIMER_DEF(name) static test_timer_t name##_data; static const app_timer_id_t name = &name##_data
#define APP_TIMER_MODE_SINGLE_SHOT 0U
#define APP_TIMER_TICKS(ms) ((uint32_t)(ms))
#define APP_TIMER_MIN_TIMEOUT_TICKS 5U
uint32_t app_timer_create(app_timer_id_t const *, unsigned, app_timer_timeout_handler_t);
uint32_t app_timer_start(app_timer_id_t, uint32_t, void *);
uint32_t app_timer_stop(app_timer_id_t);
#endif
