#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
esp_err_t net_init(void);
bool net_wait_connected(int timeout_ms);
char *net_analyze(const int16_t *pcm, size_t n_samples, uint32_t sample_rate);
