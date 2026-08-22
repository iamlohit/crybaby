#include "board.h"
#include "display.h"
#include "mic.h"
#include "net.h"
#include "speaker.h"
#include "ui.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "main";
static adc_oneshot_unit_handle_t adc;

/* Diagnostic for the boot-chime reboot loop: no serial console works over this
 * board's CH340 link from the Mac (RX path broken, see docs/FLASHING.md), so
 * surface the reset cause on-screen instead. */
static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "power-on";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_INT_WDT:  return "interrupt wdt";
        case ESP_RST_TASK_WDT: return "task wdt";
        case ESP_RST_WDT:      return "other wdt";
        case ESP_RST_SW:       return "sw reset";
        default:               return "other";
    }
}

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

/* Speaker volume, persisted so it survives a power cycle. NVS is already
 * initialised by net_init(), so load only after that has run. */
#define VOLUME_STEP 10
static int g_volume = 80;

static void volume_load(void)
{
    nvs_handle_t h;
    if (nvs_open("crybaby", NVS_READONLY, &h) != ESP_OK) return;
    int32_t v = 0;
    if (nvs_get_i32(h, "volume", &v) == ESP_OK && v >= 0 && v <= 100) g_volume = (int)v;
    nvs_close(h);
}

static void volume_adjust(int delta)
{
    int v = g_volume + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (v != g_volume) {
        g_volume = v;
        nvs_handle_t h;
        if (nvs_open("crybaby", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_i32(h, "volume", g_volume);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    ui_show_volume(g_volume);   /* show feedback even at the rails */
}

static bool talk_button_down(void)
{
    return gpio_get_level(BTN_UP) == 0;
}

/* The left top button is both "quieter" and the modifier for "louder": tap it
 * alone to go down, or hold it and tap the right button to go up. Volume-down
 * therefore fires on *release*, once we know no chord happened — otherwise
 * starting a chord would always knock the volume down first.
 * Returns true if the left button was involved, so the caller skips talk/analyze. */
static bool volume_chord(void)
{
    if (gpio_get_level(BTN_DOWN) != 0) return false;
    vTaskDelay(pdMS_TO_TICKS(30));            /* debounce */
    if (gpio_get_level(BTN_DOWN) != 0) return false;

    bool chorded = false;
    while (gpio_get_level(BTN_DOWN) == 0) {
        if (gpio_get_level(BTN_UP) == 0) {
            vTaskDelay(pdMS_TO_TICKS(30));
            if (gpio_get_level(BTN_UP) == 0) {
                volume_adjust(+VOLUME_STEP);
                chorded = true;
                while (gpio_get_level(BTN_UP) == 0) vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!chorded) volume_adjust(-VOLUME_STEP);
    return true;
}

/* Push-to-talk: report a press as soon as it debounces (unlike button_pressed(),
 * this does NOT wait for release — release is the "stop recording" signal instead). */
static bool talk_button_pressed(void)
{
    if (!talk_button_down()) return false;
    vTaskDelay(pdMS_TO_TICKS(30));
    return talk_button_down();
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

/* strdup'd texts leak per-cycle; bounded by UI usage — copy into a static buf */
static char talk_transcript_buf[128];
static char talk_reply_buf[256];

static void talk_cycle(void)
{
    ui_show_listening();
    size_t total = 0;
    int16_t *buf = mic_record_until(TALK_MAX_SECONDS, &total, NULL, talk_button_down);
    if (!buf || total < MIC_SAMPLE_RATE / 4) {
        free(buf);
        ui_show_message("Didn't catch that", "Hold the right button and talk,\nthen release.", true);
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }
    ui_show_thinking();
    uint8_t *wav = NULL;
    size_t wav_len = 0;
    net_talk_meta_t meta = { 0 };
    bool ok = net_talk(buf, total, MIC_SAMPLE_RATE, &wav, &wav_len, &meta);
    free(buf);
    if (!ok || !wav) {
        free(wav);
        ui_show_message("No connection", "Couldn't reach the voice service.\nCheck Wi-Fi and server.", true);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return;
    }
    strlcpy(talk_transcript_buf, meta.transcript, sizeof talk_transcript_buf);
    strlcpy(talk_reply_buf, meta.reply, sizeof talk_reply_buf);
    ui_show_answer(talk_transcript_buf, talk_reply_buf);
    if (wav_len > 44) speaker_play((const int16_t *)(wav + 44), (wav_len - 44) / 2, g_volume);
    free(wav);
}

void app_main(void)
{
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BTN_MAIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);
    gpio_config_t btn_vol = {
        .pin_bit_mask = (1ULL << BTN_UP) | (1ULL << BTN_DOWN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn_vol);
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
    char boot_msg[48];
    snprintf(boot_msg, sizeof boot_msg, "Last reset: %s", reset_reason_str());
    ui_show_message("Starting", boot_msg, false);
    vTaskDelay(pdMS_TO_TICKS(1500));
    ui_show_message("Starting", "Connecting to Wi-Fi…", false);
    ESP_ERROR_CHECK(net_init());
    ESP_ERROR_CHECK(mic_init());
    bool wifi = net_wait_connected(15000);
    if (!wifi) ESP_LOGW(TAG, "wifi not connected yet, continuing");

    /* Speaker powers up only after Wi-Fi association settles — avoids stacking the
     * amp's inrush current on top of a Wi-Fi TX burst (suspected brownout cause). */
    ESP_ERROR_CHECK(speaker_init());
    volume_load();
    speaker_play_chime(g_volume);

    ui_show_idle(battery_percent(), wifi, charger_attached());
    bool showing_result = false;
    bool last_chg = charger_attached();
    int idle_refresh = 0;
    while (true) {
        if (button_pressed()) {
            analyze_cycle();
            showing_result = true;
            idle_refresh = 0;
        } else if (volume_chord()) {
            vTaskDelay(pdMS_TO_TICKS(700));   /* let the volume readout land */
            ui_show_idle(battery_percent(), net_wait_connected(0), charger_attached());
            showing_result = false;
            idle_refresh = 0;
        } else if (talk_button_pressed()) {
            talk_cycle();
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
