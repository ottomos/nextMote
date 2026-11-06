#pragma once
#include <stdint.h>

extern int64_t host_test_time_us;
static inline int64_t esp_timer_get_time(void) { return host_test_time_us; }
