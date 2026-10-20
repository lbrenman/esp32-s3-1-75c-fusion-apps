#pragma once
/* Day/night color palettes. Screens read ui_pal when they are built; switching the theme
 * rebuilds the current screen (ui_nav_refresh), so nothing needs to restyle itself. */

#include <stdbool.h>
#include "lvgl.h"

typedef struct {
    lv_color_t bg;          /* screen background (true black at night: AMOLED pixels off) */
    lv_color_t card;        /* list rows, panels */
    lv_color_t card_press;
    lv_color_t text;
    lv_color_t text_dim;
    lv_color_t line;
    lv_color_t accent;
    lv_color_t ok, warn, err;
} ui_pal_t;

extern ui_pal_t ui_pal;

void ui_theme_init(lv_display_t *disp, bool dark);
void ui_theme_set_dark(bool dark);    /* call ui_nav_refresh() afterwards */
bool ui_theme_is_dark(void);

/* Per-app accent gradients (icon tiles, voice-chat orb) */
#define UI_C_VOICE_1     0x7C5CFF
#define UI_C_VOICE_2     0x2F8CFF
#define UI_C_SETTINGS_1  0x8E9AAF
#define UI_C_SETTINGS_2  0x4A5468

/* Row icon colors (iOS-style colored squares) */
#define UI_C_BLUE    0x2F8CFF
#define UI_C_GREEN   0x30C26A
#define UI_C_ORANGE  0xFF9F0A
#define UI_C_PURPLE  0x8E5CFF
#define UI_C_RED     0xFF453A
#define UI_C_GRAY    0x8E8E93
#define UI_C_TEAL    0x14B8C8
#define UI_C_INDIGO  0x5E5CE6
