#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

esp_err_t speaker_init(void);
/* Play mono 16-bit PCM at SPK_SAMPLE_RATE; volume 0-100 (stock's pow(v/100,2) curve). */
void speaker_play(const int16_t *pcm, size_t n_samples, int volume);
/* Two-tone boot chime — proves the amp without needing a WAV asset. */
void speaker_play_chime(int volume);
