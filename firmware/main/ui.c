/* crybaby UI — HIG-derived design for 240x240 LVGL, button-driven.
 * System: pure-black bg; SF-dark-palette accents; Montserrat scale 14/16/20/28/48;
 * sentence case; one primary action per screen; instant state feedback. */
#include "ui.h"
#include "esp_lvgl_port.h"
#include <stdio.h>

#define COL_BG      lv_color_black()
#define COL_TEXT    lv_color_white()
#define COL_MUTED   lv_color_hex(0x8E8E93)  /* systemGray, 5.4:1 on black */
#define COL_RED     lv_color_hex(0xFF453A)
#define COL_BLUE    lv_color_hex(0x0A84FF)
#define COL_GREEN   lv_color_hex(0x30D158)
#define COL_YELLOW  lv_color_hex(0xFFD60A)
#define COL_CARD    lv_color_hex(0x1C1C1E)  /* secondarySystemBackground dark */

static lv_obj_t *scr(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, COL_BG, 0);
    lv_obj_set_style_text_color(s, COL_TEXT, 0);
    return s;
}

static lv_obj_t *label(lv_obj_t *p, const char *txt, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

static void load(lv_obj_t *s)
{
    lv_screen_load_anim(s, LV_SCR_LOAD_ANIM_FADE_IN, 120, 0, true);
}

void ui_show_idle(int battery_pct, bool wifi_ok, bool charging)
{
    lvgl_port_lock(0);
    lv_obj_t *s = scr();

    /* status bar: caption size, muted */
    char buf[32];
    snprintf(buf, sizeof buf, "%s  %s%d%%", wifi_ok ? LV_SYMBOL_WIFI : "",
             charging ? LV_SYMBOL_CHARGE " " : "", battery_pct);
    lv_obj_t *st = label(s, buf, &lv_font_montserrat_14, COL_MUTED);
    lv_obj_align(st, LV_ALIGN_TOP_RIGHT, -12, 10);

    /* primary: audio glyph inside a circle, the single focal point */
    lv_obj_t *circle = lv_obj_create(s);
    lv_obj_set_size(circle, 96, 96);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, COL_CARD, 0);
    lv_obj_set_style_border_width(circle, 0, 0);
    lv_obj_align(circle, LV_ALIGN_CENTER, 0, -24);
    lv_obj_t *mic = label(circle, LV_SYMBOL_AUDIO, &lv_font_montserrat_28, COL_BLUE);
    lv_obj_center(mic);

    lv_obj_t *t = label(s, "Cry analyzer", &lv_font_montserrat_20, COL_TEXT);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 44);
    lv_obj_t *hint = label(s, "Press the side button\nwhen baby cries", &lv_font_montserrat_14, COL_MUTED);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -18);

    load(s);
    lvgl_port_unlock();
}

static lv_obj_t *rec_count_label, *rec_arc;

void ui_show_recording(int total_seconds)
{
    lvgl_port_lock(0);
    lv_obj_t *s = scr();

    lv_obj_t *dot = lv_obj_create(s);
    lv_obj_set_size(dot, 14, 14);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, COL_RED, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, -44, 16);
    lv_obj_t *t = label(s, "Listening", &lv_font_montserrat_16, COL_RED);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 12, 12);

    rec_arc = lv_arc_create(s);
    lv_obj_set_size(rec_arc, 150, 150);
    lv_arc_set_rotation(rec_arc, 270);
    lv_arc_set_bg_angles(rec_arc, 0, 360);
    lv_arc_set_range(rec_arc, 0, total_seconds);
    lv_arc_set_value(rec_arc, 0);
    lv_obj_remove_style(rec_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(rec_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(rec_arc, COL_CARD, LV_PART_MAIN);
    lv_obj_set_style_arc_color(rec_arc, COL_RED, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(rec_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(rec_arc, 8, LV_PART_INDICATOR);
    lv_obj_align(rec_arc, LV_ALIGN_CENTER, 0, 8);

    rec_count_label = label(s, "", &lv_font_montserrat_48, COL_TEXT);
    lv_obj_align(rec_count_label, LV_ALIGN_CENTER, 0, 8);
    load(s);
    lvgl_port_unlock();
}

void ui_recording_tick(int seconds_done, int total_seconds)
{
    lvgl_port_lock(0);
    char buf[8];
    snprintf(buf, sizeof buf, "%d", total_seconds - seconds_done);
    lv_label_set_text(rec_count_label, buf);
    lv_arc_set_value(rec_arc, seconds_done);
    lvgl_port_unlock();
}

void ui_show_analyzing(void)
{
    lvgl_port_lock(0);
    lv_obj_t *s = scr();
    lv_obj_t *sp = lv_spinner_create(s);
    lv_obj_set_size(sp, 72, 72);
    lv_obj_set_style_arc_color(sp, COL_CARD, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp, COL_BLUE, LV_PART_INDICATOR);
    lv_obj_align(sp, LV_ALIGN_CENTER, 0, -24);
    lv_obj_t *t = label(s, "Analyzing", &lv_font_montserrat_20, COL_TEXT);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 40);
    load(s);
    lvgl_port_unlock();
}

void ui_show_result(const ui_result_t *r)
{
    lvgl_port_lock(0);
    lv_obj_t *s = scr();

    lv_color_t accent = r->top_prob >= 0.6f ? COL_GREEN : COL_YELLOW;
    lv_obj_t *title = label(s, r->top_text, &lv_font_montserrat_28, accent);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(title, 216);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    /* ranked confidence bars, card-styled */
    int y = 92;
    for (int i = 0; i < r->n && i < 3; i++, y += 42) {
        char buf[48];
        snprintf(buf, sizeof buf, "%s  %d%%", r->items[i].text, (int)(r->items[i].prob * 100));
        lv_obj_t *l = label(s, buf, &lv_font_montserrat_14, i == 0 ? COL_TEXT : COL_MUTED);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 16, y);
        lv_obj_t *bar = lv_bar_create(s);
        lv_obj_set_size(bar, 208, 6);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, (int)(r->items[i].prob * 100), LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar, COL_CARD, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, i == 0 ? accent : COL_MUTED, LV_PART_INDICATOR);
        lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 16, y + 20);
    }

    lv_obj_t *hint = label(s, "Press to listen again", &lv_font_montserrat_14, COL_MUTED);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
    load(s);
    lvgl_port_unlock();
}

void ui_show_message(const char *title, const char *body, bool is_error)
{
    lvgl_port_lock(0);
    lv_obj_t *s = scr();
    lv_obj_t *t = label(s, title, &lv_font_montserrat_20, is_error ? COL_RED : COL_TEXT);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -30);
    lv_obj_t *b = label(s, body, &lv_font_montserrat_14, COL_MUTED);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(b, 208);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, 16);
    load(s);
    lvgl_port_unlock();
}
