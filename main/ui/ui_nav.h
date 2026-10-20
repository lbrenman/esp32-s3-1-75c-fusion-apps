#pragma once
/* Screen navigation.
 *
 * Every screen is built by a function and deleted when you leave it, so screens always use
 * the current theme and hold no stale state. A small stack remembers how to rebuild the
 * screens underneath (Home > App > App settings > Editor ...).
 *
 * Apps are registered in apps/apps.c. on_enter / on_leave run when the app is opened from
 * Home and when the user goes back to Home. All functions here run in the LVGL task. */

#include <stdbool.h>
#include "lvgl.h"

typedef lv_obj_t *(*ui_build_fn)(void *arg);

typedef struct {
    const char *name;
    const char *icon;               /* ICON_* from ui_icons.h */
    uint32_t    color1, color2;     /* tile gradient */
    ui_build_fn build;              /* returns the app's main screen */
    void      (*on_enter)(void);
    void      (*on_leave)(void);
} ui_app_t;

void ui_nav_init(ui_build_fn home_build);
void ui_nav_open_app(const ui_app_t *app);
void ui_nav_home(lv_screen_load_anim_t anim);
void ui_nav_push(ui_build_fn build, void *arg, lv_screen_load_anim_t anim);
void ui_nav_replace(ui_build_fn build, void *arg, lv_screen_load_anim_t anim);
void ui_nav_pop(lv_screen_load_anim_t anim);
void ui_nav_pop_n(int n, lv_screen_load_anim_t anim);
void ui_nav_refresh(void);          /* rebuild the current screen (theme change) */
bool ui_nav_busy(void);             /* a transition is still running: ignore taps/swipes */
const ui_app_t *ui_nav_current_app(void);
