#pragma once
#include <stdint.h>

/* Turns on the touch rails (LDO19M tsp_io 1.8 V, LDO27M tsp_avdd 3.0 V)
 * through ACPM. Returns 0 when both writes were acknowledged. */
int s2mps19_touch_rails_on(void (*kv)(const char *, uint64_t), void (*out)(const char *));
