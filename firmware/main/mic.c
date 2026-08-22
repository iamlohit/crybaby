#include "mic.h"
#include "board.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "mic";
static i2s_chan_handle_t rx_chan;

esp_err_t mic_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_I2S_SCK,
            .ws   = MIC_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_I2S_DIN,
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));
    ESP_LOGI(TAG, "I2S mic ready (%d Hz)", MIC_SAMPLE_RATE);
    return ESP_OK;
}

/* Record up to `max_seconds` of audio into a PSRAM buffer of int16 samples.
 * INMP441 delivers 24-bit data in 32-bit slots; shift down to 16-bit.
 * If poll_cb is non-NULL, stops early (before max_seconds) once it returns false. */
int16_t *mic_record_until(int max_seconds, size_t *out_samples, mic_progress_cb cb,
                          mic_poll_cb poll_cb)
{
    const size_t n_samples = (size_t)MIC_SAMPLE_RATE * max_seconds;
    int16_t *pcm = heap_caps_malloc(n_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int32_t *chunk = heap_caps_malloc(4096 * sizeof(int32_t), MALLOC_CAP_DEFAULT);
    if (!pcm || !chunk) {
        free(pcm); free(chunk);
        return NULL;
    }
    size_t done = 0;
    while (done < n_samples) {
        if (poll_cb && !poll_cb()) break;
        size_t want = n_samples - done;
        if (want > 4096) want = 4096;
        size_t got_bytes = 0;
        if (i2s_channel_read(rx_chan, chunk, want * sizeof(int32_t),
                             &got_bytes, 1000) != ESP_OK) break;
        size_t got = got_bytes / sizeof(int32_t);
        for (size_t i = 0; i < got; i++)
            pcm[done + i] = (int16_t)(chunk[i] >> 14);  /* 24-bit MSB-aligned -> 16 */
        done += got;
        if (cb) {
            static size_t last_sec;
            size_t sec = done / MIC_SAMPLE_RATE;
            if (sec != last_sec) { last_sec = sec; cb((int)sec, max_seconds); }
        }
    }
    free(chunk);
    *out_samples = done;
    ESP_LOGI(TAG, "recorded %u samples", (unsigned)done);
    return pcm;
}

int16_t *mic_record(int seconds, size_t *out_samples, mic_progress_cb cb)
{
    return mic_record_until(seconds, out_samples, cb, NULL);
}
