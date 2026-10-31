/* Emotive Wall-E-style eyes for the 240x240 face. Two rounded rects on a
 * transparent container. Expression comes from per-eye "pose" (height,
 * vertical skew, tilt around the centre pivot), blinks animate height only
 * so a tilt survives the blink, and gaze saccades animate the container's
 * translate style. All animation runs inside the LVGL task via one lv_timer
 * per instance; screens are replaced with auto-delete, so each instance
 * frees itself on its container's LV_EVENT_DELETE. */
#include "eyes.h"
#include <stdlib.h>

#define EYE_COLOR      lv_color_hex(0x64D2FF) /* systemTeal dark — friendly, not alarm-red */
#define EYE_GAP        26
#define BLINK_MIN_H    6
#define TICK_MS        80

typedef struct {
    lv_obj_t *cont;
    lv_obj_t *left, *right;
    eyes_mood_t mood;
    int eye_w, eye_h;            /* fully-open geometry */
    lv_timer_t *timer;
    uint32_t next_blink, next_gaze;  /* lv_tick deadlines */
    int gaze_x, gaze_y;          /* current translate offset */
    int think_side;              /* thinking mood alternates upper corners */
} eyes_t;

static uint32_t rand_between(uint32_t lo, uint32_t hi)
{
    return lo + lv_rand(0, hi - lo);
}

/* Per-eye expression: open height, vertical skew, tilt (0.1° units,
 * positive = clockwise). Wall-E emotes almost entirely with these three. */
static void eye_pose(lv_obj_t *eye, int h, int dy, int angle_x10)
{
    lv_obj_set_height(eye, h);
    lv_obj_set_style_translate_y(eye, dy, 0);
    lv_obj_set_style_transform_rotation(eye, angle_x10, 0);
}

/* --- blink: v runs 100 -> closed -> 100, height tracks it --- */

static void blink_exec(void *var, int32_t v)
{
    eyes_t *e = var;
    int h = e->eye_h * v / 100;
    if (h < BLINK_MIN_H) h = BLINK_MIN_H;
    lv_obj_set_height(e->left, h);
    lv_obj_set_height(e->right, h);
}

static void blink_reopen(lv_anim_t *closed)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, closed->var);
    lv_anim_set_exec_cb(&a, blink_exec);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_duration(&a, 120);
    lv_anim_set_delay(&a, 40);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void blink_start(eyes_t *e)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, e);
    lv_anim_set_exec_cb(&a, blink_exec);
    lv_anim_set_values(&a, 100, 0);
    lv_anim_set_duration(&a, 90);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, blink_reopen);
    lv_anim_start(&a);
}

/* --- gaze: saccade the whole pair to a new offset --- */

static void gaze_x_exec(void *var, int32_t v)
{
    eyes_t *e = var;
    e->gaze_x = v;
    lv_obj_set_style_translate_x(e->cont, v, 0);
}

static void gaze_y_exec(void *var, int32_t v)
{
    eyes_t *e = var;
    e->gaze_y = v;
    lv_obj_set_style_translate_y(e->cont, v, 0);
}

static void gaze_to(eyes_t *e, int x, int y)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, e);
    lv_anim_set_duration(&a, 150);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, gaze_x_exec);
    lv_anim_set_values(&a, e->gaze_x, x);
    lv_anim_start(&a);
    lv_anim_set_exec_cb(&a, gaze_y_exec);
    lv_anim_set_values(&a, e->gaze_y, y);
    lv_anim_start(&a);
}

static void pick_gaze(eyes_t *e)
{
    switch (e->mood) {
    case EYES_MOOD_LISTENING:
        /* rapt attention: wide, level, locked on the speaker — only tiny
         * shifts so it reads as "looking at you", never distracted */
        eye_pose(e->left, e->eye_h, 0, 0);
        eye_pose(e->right, e->eye_h, 0, 0);
        gaze_to(e, (int)lv_rand(0, 10) - 5, (int)lv_rand(0, 6) - 4);
        break;

    case EYES_MOOD_THINKING:
        /* Wall-E "hmm": flick between upper corners, the eye on the gaze
         * side opens wider (raised brow) while the other squints, both
         * tilting into the glance */
        e->think_side = -e->think_side;
        gaze_to(e, e->think_side * (int)rand_between(12, 18), -(int)rand_between(8, 12));
        if (e->think_side > 0) {
            eye_pose(e->left, e->eye_h - 6, 3, 60);
            eye_pose(e->right, e->eye_h + 10, -4, 60);
        } else {
            eye_pose(e->left, e->eye_h + 10, -4, -60);
            eye_pose(e->right, e->eye_h - 6, 3, -60);
        }
        break;

    case EYES_MOOD_SAD:
        /* outer corners drooped, eyes cast down, barely moving */
        eye_pose(e->left, e->eye_h, 2, -80);
        eye_pose(e->right, e->eye_h, 2, 80);
        gaze_to(e, (int)lv_rand(0, 6) - 3, (int)rand_between(8, 12));
        break;

    case EYES_MOOD_HAPPY:
        /* smile-squint: outer corners up (sad inverted), light lively bounce */
        eye_pose(e->left, e->eye_h, -2, 60);
        eye_pose(e->right, e->eye_h, -2, -60);
        gaze_to(e, (int)lv_rand(0, 6) - 3, (int)lv_rand(0, 4) - 4);
        break;

    case EYES_MOOD_IDLE:
    default:
        if (lv_rand(0, 9) < 4) {
            /* often return to a level, centred rest pose */
            eye_pose(e->left, e->eye_h, 0, 0);
            eye_pose(e->right, e->eye_h, 0, 0);
            gaze_to(e, 0, 0);
        } else if (lv_rand(0, 5) == 0) {
            /* rare surprise pop: both eyes snap wide */
            eye_pose(e->left, e->eye_h + 12, -3, 0);
            eye_pose(e->right, e->eye_h + 12, -3, 0);
            gaze_to(e, 0, -4);
        } else {
            int x = (int)lv_rand(0, 28) - 14;
            gaze_to(e, x, (int)lv_rand(0, 16) - 8);
            if (x < -8 || x > 8) {
                /* curious side-glance: leading eye rises and widens, the
                 * trailing eye drops and narrows, both lean into the turn */
                lv_obj_t *lead = x < 0 ? e->left : e->right;
                lv_obj_t *trail = x < 0 ? e->right : e->left;
                int tilt = x < 0 ? -40 : 40;
                eye_pose(lead, e->eye_h + 8, -4, tilt);
                eye_pose(trail, e->eye_h - 8, 4, tilt);
            }
        }
        break;
    }
}

static void schedule_blink(eyes_t *e, uint32_t now)
{
    uint32_t lo, hi;
    switch (e->mood) {
    case EYES_MOOD_LISTENING: lo = 3500; hi = 7000; break;
    case EYES_MOOD_THINKING:  lo = 4000; hi = 8000; break;
    case EYES_MOOD_SAD:       lo = 5000; hi = 9000; break;
    case EYES_MOOD_HAPPY:     lo = 2000; hi = 4500; break;
    default:                  lo = 2400; hi = 5600; break;
    }
    /* occasional quick double-blink when idle */
    if (e->mood == EYES_MOOD_IDLE && lv_rand(0, 4) == 0)
        e->next_blink = now + 400;
    else
        e->next_blink = now + rand_between(lo, hi);
}

static void schedule_gaze(eyes_t *e, uint32_t now)
{
    uint32_t lo, hi;
    switch (e->mood) {
    case EYES_MOOD_LISTENING: lo = 1800; hi = 3500; break;
    case EYES_MOOD_THINKING:  lo = 1200; hi = 2600; break;
    case EYES_MOOD_SAD:       lo = 3000; hi = 6000; break;
    case EYES_MOOD_HAPPY:     lo = 900;  hi = 1800; break;
    default:                  lo = 2800; hi = 6000; break;
    }
    e->next_gaze = now + rand_between(lo, hi);
}

static void eyes_tick(lv_timer_t *t)
{
    eyes_t *e = lv_timer_get_user_data(t);
    uint32_t now = lv_tick_get();
    if ((int32_t)(now - e->next_blink) >= 0) {
        blink_start(e);
        schedule_blink(e, now);
    }
    if ((int32_t)(now - e->next_gaze) >= 0) {
        pick_gaze(e);
        schedule_gaze(e, now);
    }
}

static void eyes_on_delete(lv_event_t *ev)
{
    eyes_t *e = lv_event_get_user_data(ev);
    lv_anim_delete(e, NULL);
    lv_timer_delete(e->timer);
    lv_free(e);
}

static lv_obj_t *make_eye(eyes_t *e, int x_ofs, int radius)
{
    lv_obj_t *eye = lv_obj_create(e->cont);
    lv_obj_set_size(eye, e->eye_w, e->eye_h);
    lv_obj_set_style_radius(eye, radius, 0);
    lv_obj_set_style_bg_color(eye, EYE_COLOR, 0);
    lv_obj_set_style_border_width(eye, 0, 0);
    lv_obj_set_style_pad_all(eye, 0, 0);
    lv_obj_set_style_transform_pivot_x(eye, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(eye, lv_pct(50), 0);
    lv_obj_remove_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(eye, LV_ALIGN_CENTER, x_ofs, 0);
    return eye;
}

void eyes_create(lv_obj_t *parent, eyes_mood_t mood, int y_ofs)
{
    eyes_t *e = lv_malloc_zeroed(sizeof(eyes_t));
    e->mood = mood;
    e->think_side = 1;
    int radius;
    switch (mood) {
    case EYES_MOOD_LISTENING: e->eye_w = 62; e->eye_h = 78; radius = 24; break;
    case EYES_MOOD_THINKING:  e->eye_w = 58; e->eye_h = 30; radius = 12; break;
    case EYES_MOOD_SAD:       e->eye_w = 46; e->eye_h = 34; radius = 14; break;
    case EYES_MOOD_HAPPY:     e->eye_w = 46; e->eye_h = 30; radius = 15; break;
    default:                  e->eye_w = 58; e->eye_h = 64; radius = 20; break;
    }

    e->cont = lv_obj_create(parent);
    /* sized with headroom: poses grow an eye by up to 12px and tilt it */
    lv_obj_set_size(e->cont, e->eye_w * 2 + EYE_GAP + 24, e->eye_h + 28);
    lv_obj_set_style_bg_opa(e->cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(e->cont, 0, 0);
    lv_obj_set_style_pad_all(e->cont, 0, 0);
    lv_obj_remove_flag(e->cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(e->cont, LV_ALIGN_CENTER, 0, y_ofs);

    int half = (e->eye_w + EYE_GAP) / 2;
    e->left = make_eye(e, -half, radius);
    e->right = make_eye(e, half, radius);

    uint32_t now = lv_tick_get();
    schedule_blink(e, now);
    schedule_gaze(e, now);
    pick_gaze(e);            /* land in the mood's pose immediately */
    e->timer = lv_timer_create(eyes_tick, TICK_MS, e);
    lv_obj_add_event_cb(e->cont, eyes_on_delete, LV_EVENT_DELETE, e);

    /* wake-up blink so the face reads as alive immediately */
    blink_start(e);
}
