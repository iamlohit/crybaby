#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
esp_err_t mic_init(void);
typedef void (*mic_progress_cb)(int seconds_done, int total_seconds);
int16_t *mic_record(int seconds, size_t *out_samples, mic_progress_cb cb);

/* Returns true to keep recording, false to stop early (e.g. button released). */
typedef bool (*mic_poll_cb)(void);
/* Like mic_record, but stops as soon as poll_cb() returns false (checked every ~0.25 s). */
int16_t *mic_record_until(int max_seconds, size_t *out_samples, mic_progress_cb progress_cb,
                          mic_poll_cb poll_cb);
