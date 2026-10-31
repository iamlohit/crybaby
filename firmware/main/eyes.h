#pragma once
#include "lvgl.h"

/* Emotive Wall-E-style robot eyes: a self-animating pair of rounded
 * rectangles with blinks, gaze saccades, and per-eye tilt/skew poses,
 * parameterised per mood. */
typedef enum {
    EYES_MOOD_IDLE,      /* calm: slow blinks, lazy wander, curious side-glances */
    EYES_MOOD_LISTENING, /* attentive: wide open, locked on the viewer */
    EYES_MOOD_THINKING,  /* pondering: squinted, brow-raise flicks between upper corners */
    EYES_MOOD_SAD,       /* apologetic: outer corners drooped, gaze cast down */
    EYES_MOOD_HAPPY,     /* delighted: squinted smile, outer corners up, lively bounce */
} eyes_mood_t;

/* Create the eye pair on `parent` (a screen), centred at (0, y_ofs) from the
 * screen centre. Caller must hold the LVGL port lock. The animation timer
 * cleans itself up when the parent screen is deleted. */
void eyes_create(lv_obj_t *parent, eyes_mood_t mood, int y_ofs);
