#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
esp_err_t mic_init(void);
typedef void (*mic_progress_cb)(int seconds_done, int total_seconds);
int16_t *mic_record(int seconds, size_t *out_samples, mic_progress_cb cb);
