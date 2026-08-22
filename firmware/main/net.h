#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
esp_err_t net_init(void);
bool net_wait_connected(int timeout_ms);
char *net_analyze(const int16_t *pcm, size_t n_samples, uint32_t sample_rate);

typedef struct {
    char transcript[128];
    char reply[256];
} net_talk_meta_t;

/* POST PCM as WAV to the voicebot; on success *out_wav is a malloc'd WAV response
 * (caller frees) and meta is filled from the X-Transcript/X-Reply-Text headers. */
bool net_talk(const int16_t *pcm, size_t n_samples, uint32_t sample_rate,
              uint8_t **out_wav, size_t *out_len, net_talk_meta_t *meta);
