#include "speaker.h"
#include "board.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdlib.h>

static const char *TAG = "speaker";
static i2s_chan_handle_t tx_chan;

esp_err_t speaker_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    /* Without this, the DMA ring just keeps circulating the last-written buffers
     * forever once idle — the tail of the chime looping endlessly (stock's
     * no_audio_codec.cc sets this same flag for the same reason). */
    chan_cfg.auto_clear_after_cb = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_I2S_BCLK,
            .ws   = SPK_I2S_LRCK,
            .dout = SPK_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
    ESP_LOGI(TAG, "I2S speaker ready (%d Hz)", SPK_SAMPLE_RATE);
    return ESP_OK;
}

/* Same volume curve as stock no_audio_codec.cc: pow(vol/100, 2) * 65536,
 * applied to 16-bit samples widened into 32-bit I2S words. */
void speaker_play(const int16_t *pcm, size_t n_samples, int volume)
{
    int32_t vol_factor = (int32_t)(pow((double)volume / 100.0, 2) * 65536);
    int32_t *chunk = heap_caps_malloc(1024 * sizeof(int32_t), MALLOC_CAP_DEFAULT);
    if (!chunk) return;

    size_t done = 0;
    while (done < n_samples) {
        size_t want = n_samples - done;
        if (want > 1024) want = 1024;
        for (size_t i = 0; i < want; i++) {
            int64_t v = (int64_t)pcm[done + i] * vol_factor;
            if (v > INT32_MAX) v = INT32_MAX;
            if (v < INT32_MIN) v = INT32_MIN;
            chunk[i] = (int32_t)v;
        }
        size_t written = 0;
        if (i2s_channel_write(tx_chan, chunk, want * sizeof(int32_t), &written,
                              pdMS_TO_TICKS(1000)) != ESP_OK) break;
        done += want;
    }
    free(chunk);
    ESP_LOGI(TAG, "played %u samples", (unsigned)done);
}

void speaker_play_chime(int volume)
{
    const int rate = SPK_SAMPLE_RATE;
    const float tones[2] = { 880.0f, 1320.0f };  /* A5 then E6, ~xylophone "ready" ping */
    const int tone_ms = 120;
    size_t n = (size_t)(rate * tone_ms / 1000);
    int16_t *buf = heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_DEFAULT);
    if (!buf) return;
    for (int t = 0; t < 2; t++) {
        for (size_t i = 0; i < n; i++) {
            float env = 1.0f - (float)i / n;  /* linear decay, avoids a click at cutoff */
            buf[i] = (int16_t)(sinf(2.0f * (float)M_PI * tones[t] * i / rate) * 12000 * env);
        }
        speaker_play(buf, n, volume);
    }
    free(buf);
}
