#pragma once
#include <stdbool.h>

typedef struct {
    const char *text;
    float prob;
} ui_result_item_t;

typedef struct {
    const char *top_text;
    float top_prob;
    ui_result_item_t items[5];
    int n;
} ui_result_t;

void ui_show_idle(int battery_pct, bool wifi_ok, bool charging);
void ui_show_recording(int total_seconds);
void ui_recording_tick(int seconds_done, int total_seconds);
void ui_show_analyzing(void);
void ui_show_result(const ui_result_t *r);
void ui_show_message(const char *title, const char *body, bool is_error);

/* Voicebot (push-to-talk) states */
void ui_show_listening(void);
void ui_show_thinking(void);
void ui_show_answer(const char *transcript, const char *reply);

void ui_show_volume(int pct);
