#include "net.h"
#include "secrets.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mbedtls/base64.h"
#include <string.h>

static const char *TAG = "net";
static EventGroupHandle_t s_evt;
#define WIFI_CONNECTED_BIT BIT0

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_evt, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_evt, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "got IP");
    }
}

esp_err_t net_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    s_evt = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL);

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASSWORD, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

bool net_wait_connected(int timeout_ms)
{
    return (xEventGroupWaitBits(s_evt, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                pdMS_TO_TICKS(timeout_ms)) & WIFI_CONNECTED_BIT) != 0;
}

/* Build a WAV header for 16-bit mono PCM. */
static void wav_header(uint8_t h[44], uint32_t sample_rate, uint32_t n_samples)
{
    uint32_t data_len = n_samples * 2, byte_rate = sample_rate * 2;
    memcpy(h, "RIFF", 4); *(uint32_t *)(h + 4) = 36 + data_len;
    memcpy(h + 8, "WAVEfmt ", 8); *(uint32_t *)(h + 16) = 16;
    *(uint16_t *)(h + 20) = 1;  *(uint16_t *)(h + 22) = 1;
    *(uint32_t *)(h + 24) = sample_rate; *(uint32_t *)(h + 28) = byte_rate;
    *(uint16_t *)(h + 32) = 2;  *(uint16_t *)(h + 34) = 16;
    memcpy(h + 36, "data", 4); *(uint32_t *)(h + 40) = data_len;
}

/* POST PCM as WAV to the classifier; caller owns response buffer (malloc'd). */
char *net_analyze(const int16_t *pcm, size_t n_samples, uint32_t sample_rate)
{
    uint8_t hdr[44];
    wav_header(hdr, sample_rate, n_samples);

    esp_http_client_config_t cfg = {
        .url = ANALYZE_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", "audio/wav");
    size_t total = sizeof(hdr) + n_samples * 2;
    char *resp = NULL;

    if (esp_http_client_open(c, total) == ESP_OK) {
        esp_http_client_write(c, (const char *)hdr, sizeof(hdr));
        esp_http_client_write(c, (const char *)pcm, n_samples * 2);
        int64_t clen = esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        int cap = (clen > 0 && clen < 16384) ? (int)clen + 1 : 4096;
        resp = malloc(cap);
        if (resp) {
            int r = esp_http_client_read_response(c, resp, cap - 1);
            resp[r > 0 ? r : 0] = '\0';
        }
        ESP_LOGI(TAG, "analyze HTTP %d: %s", status, resp ? resp : "(null)");
        if (status != 200) { free(resp); resp = NULL; }
    } else {
        ESP_LOGE(TAG, "connect to %s failed", ANALYZE_URL);
    }
    esp_http_client_cleanup(c);
    return resp;
}

/* Header value is base64 (server-side, keeps it ASCII-safe for non-ASCII replies);
 * decode into a fixed caller buffer, empty string if missing or it doesn't fit. */
static void get_b64_header(esp_http_client_handle_t c, const char *key, char *out, size_t out_cap)
{
    out[0] = '\0';
    char *val = NULL;
    if (esp_http_client_get_header(c, key, &val) != ESP_OK || !val) return;
    size_t olen = 0;
    if (mbedtls_base64_decode((unsigned char *)out, out_cap - 1, &olen,
                              (const unsigned char *)val, strlen(val)) == 0) {
        out[olen] = '\0';
    }
}

/* POST PCM as WAV to the voicebot; caller owns *out_wav (malloc'd) on success. */
bool net_talk(const int16_t *pcm, size_t n_samples, uint32_t sample_rate,
              uint8_t **out_wav, size_t *out_len, net_talk_meta_t *meta)
{
    *out_wav = NULL;
    *out_len = 0;
    memset(meta, 0, sizeof(*meta));

    uint8_t hdr[44];
    wav_header(hdr, sample_rate, n_samples);

    esp_http_client_config_t cfg = {
        .url = TALK_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", "audio/wav");
    size_t total = sizeof(hdr) + n_samples * 2;
    bool ok = false;

    if (esp_http_client_open(c, total) == ESP_OK) {
        esp_http_client_write(c, (const char *)hdr, sizeof(hdr));
        esp_http_client_write(c, (const char *)pcm, n_samples * 2);
        int64_t clen = esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        get_b64_header(c, "X-Transcript", meta->transcript, sizeof(meta->transcript));
        get_b64_header(c, "X-Reply-Text", meta->reply, sizeof(meta->reply));
        if (status == 200 && clen > 0 && clen < 4 * 1024 * 1024) {
            /* A spoken reply is hundreds of KB (~600 KB seen live) — far more than
             * internal DRAM has free alongside Wi-Fi and LVGL, so this must come
             * from PSRAM, same as mic_record()'s capture buffer. */
            uint8_t *buf = heap_caps_malloc((size_t)clen, MALLOC_CAP_SPIRAM);
            if (!buf) {
                ESP_LOGE(TAG, "no room for %d-byte reply", (int)clen);
            } else {
                int r = esp_http_client_read_response(c, (char *)buf, (int)clen);
                if (r > 0) {
                    *out_wav = buf;
                    *out_len = (size_t)r;
                    ok = true;
                } else {
                    ESP_LOGE(TAG, "reply body read failed (%d)", r);
                    free(buf);
                }
            }
        }
        ESP_LOGI(TAG, "talk HTTP %d, wav=%d bytes, transcript=\"%s\" reply=\"%s\"",
                 status, (int)*out_len, meta->transcript, meta->reply);
    } else {
        ESP_LOGE(TAG, "connect to %s failed", TALK_URL);
    }
    esp_http_client_cleanup(c);
    return ok;
}
