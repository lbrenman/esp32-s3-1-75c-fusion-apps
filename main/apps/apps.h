#pragma once
/* App registry. To add an app: write apps/app_<name>.c with a build function (and optional
 * on_enter / on_leave), declare its ui_app_t here, and list it in g_apps in apps.c. */

#include "ui_nav.h"

extern const ui_app_t APP_VOICE;
extern const ui_app_t APP_TRANSLATE;
extern const ui_app_t APP_STOCKS;
extern const ui_app_t APP_SETTINGS;

extern const ui_app_t *const g_apps[];
extern const int g_app_count;

lv_obj_t *home_build(void *arg);

/* Settings app: its main list is the screen showing right now (not a screen opened from it). */
bool app_settings_showing(void);
/* Rebuild the current screen (after a theme change, or settings changed from the phone page),
 * keeping the Settings list's scroll position when it's the one showing. */
void app_settings_refresh(void);
