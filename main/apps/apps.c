#include "apps.h"

/* Order on the Home screen */
const ui_app_t *const g_apps[] = {
    &APP_VOICE,
    &APP_VOICE_AI,
    &APP_TRANSLATE,
    &APP_STOCKS,
    &APP_QUOTES,
    &APP_SETTINGS,
};
const int g_app_count = sizeof(g_apps) / sizeof(g_apps[0]);
