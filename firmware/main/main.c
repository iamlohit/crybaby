#include "board.h"
#include "display.h"
#include "mic.h"
#include "net.h"
#include "ui.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "main";
static adc_oneshot_unit_handle_t adc;

static int battery_percent(void)
{
    /* stock power_manager calibration: +80 raw offset, rolling 3-sample average,
       linear interpolation between 20%-step breakpoints */
    static const struct { int adc, level; } levels[] = {
        {2030, 0}, {2134, 20}, {2252, 40}, {2370, 60}, {2488, 80}, {2606, 100}};
    static int hist[3], nhist = 0, pos = 0;
    int raw = 0;
    if (adc_oneshot_read(adc, BAT_ADC_CHANNEL, &raw) != ESP_OK) return -1;
    hist[pos] = raw + 80;
    pos = (pos + 1) % 3;
    if (nhist < 3) nhist++;
    int avg = 0;
    for (int i = 0; i < nhist; i++) avg += hist[i];
    avg /= nhist;
    if (avg < levels[0].adc) return 0;
    for (int i = 0; i < 5; i++)
        if (avg < levels[i + 1].adc)
            return levels[i].level + (avg - levels[i].adc) * 20 / (levels[i + 1].adc - levels[i].adc);
    return 100;
}

static bool charger_attached(void)
{
    return gpio_get_level(BAT_CHARGING_GPIO) == 1;
}

static bool button_pressed(void)
{
    if (gpio_get_level(BTN_MAIN) != 0) return false;
    vTaskDelay(pdMS_TO_TICKS(30));            /* debounce */
    if (gpio_get_level(BTN_MAIN) != 0) return false;
    while (gpio_get_level(BTN_MAIN) == 0) vTaskDelay(pdMS_TO_TICKS(10));
    return true;
}

/* strdup'd texts leak per-cycle; bounded by UI usage — copy into static bufs */
static char text_bufs[5][48];

static bool parse_and_show(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    bool shown = false;
    cJSON *is_cry = cJSON_GetObjectItem(root, "is_cry");
    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (cJSON_IsFalse(is_cry)) {
        ui_show_message("No cry heard", "I couldn't detect a baby cry.\nHold closer and try again.", false);
        shown = true;
    } else if (cJSON_IsArray(results)) {
        ui_result_t r = { 0 };
        int n = cJSON_GetArraySize(results);
        for (int i = 0; i < n && r.n < 5; i++) {
            cJSON *it = cJSON_GetArrayItem(results, i);
            cJSON *text = cJSON_GetObjectItem(it, "text");
            cJSON *prob = cJSON_GetObjectItem(it, "prob");
            if (!cJSON_IsString(text) || !cJSON_IsNumber(prob)) continue;
            strlcpy(text_bufs[r.n], text->valuestring, sizeof text_bufs[0]);
            r.items[r.n].text = text_bufs[r.n];
            r.items[r.n].prob = (float)prob->valuedouble;
            r.n++;
        }
        if (r.n > 0) {
            r.top_text = r.items[0].text;
            r.top_prob = r.items[0].prob;
            ui_show_result(&r);
            shown = true;
        }
    }
    cJSON_Delete(root);
    return shown;
}

static void analyze_cycle(void)
{
    ui_show_recording(RECORD_SECONDS);
    size_t total = 0;
    int16_t *buf = mic_record(RECORD_SECONDS, &total, ui_recording_tick);
    if (!buf || total < MIC_SAMPLE_RATE) {
        free(buf);
        ui_show_message("Mic error", "Couldn't record audio.", true);
        vTaskDelay(pdMS_TO_TICKS(2500));
        return;
    }
    ui_show_analyzing();
    char *resp = net_analyze(buf, total, MIC_SAMPLE_RATE);
    free(buf);
    if (!resp) {
        ui_show_message("No connection", "Couldn't reach the analyzer.\nCheck Wi-Fi and server.", true);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return;
    }
    if (!parse_and_show(resp)) {
        ui_show_message("Hmm", "Unexpected reply from server.", true);
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
    free(resp);
}

void app_main(void)
{
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BTN_MAIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);
    gpio_config_t chg = {
        .pin_bit_mask = 1ULL << BAT_CHARGING_GPIO,
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&chg);
    adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&adc_cfg, &adc);
    adc_oneshot_chan_cfg_t ch = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    adc_oneshot_config_channel(adc, BAT_ADC_CHANNEL, &ch);

    display_init();
    ui_show_message("Starting", "Connecting to Wi-Fi…", false);
    ESP_ERROR_CHECK(net_init());
    ESP_ERROR_CHECK(mic_init());
    bool wifi = net_wait_connected(15000);
    if (!wifi) ESP_LOGW(TAG, "wifi not connected yet, continuing");

    ui_show_idle(battery_percent(), wifi, charger_attached());
    bool showing_result = false;
    bool last_chg = charger_attached();
    int idle_refresh = 0;
    while (true) {
        if (button_pressed()) {
            analyze_cycle();
            showing_result = true;
            idle_refresh = 0;
        } else if (showing_result && ++idle_refresh > 3000) {
            /* return to idle ~30 s after a result */
            ui_show_idle(battery_percent(), net_wait_connected(0), charger_attached());
            showing_result = false;
        } else if (!showing_result && charger_attached() != last_chg) {
            /* cable plugged/unplugged: reflect immediately */
            last_chg = charger_attached();
            ui_show_idle(battery_percent(), net_wait_connected(0), last_chg);
            idle_refresh = 0;
        } else if (!showing_result && ++idle_refresh > 6000) {
            ui_show_idle(battery_percent(), net_wait_connected(0), charger_attached());
            idle_refresh = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
