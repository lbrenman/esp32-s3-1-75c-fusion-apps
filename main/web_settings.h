#pragma once
/* Phone settings page: a small web server on the watch (port 80) plus an mDNS name, so a phone or
 * computer on the same Wi-Fi can open http://fusion-apps.local and change the same settings as the
 * watch's own screens (everything except Wi-Fi).
 *
 * - The page asks for the 6-digit PIN shown in Settings > Phone. A correct PIN gives the browser a
 *   session cookie, kept in RAM until a reboot, "New PIN" or switching the page off.
 * - WEB_PIN_MAX_TRIES wrong PINs in a row lock the page for WEB_PIN_LOCK_S seconds.
 * - API keys are write-only: the page only learns whether one is saved and its last 4 characters
 *   (as the watch shows them). Nothing secret is logged.
 * - Changes are applied in the LVGL task exactly like the watch's own screens apply them
 *   (save to NVS, reconnect the voice engine, reload Stocks, change the theme...).
 *
 * The server task's stack and its buffers are in PSRAM, to keep internal RAM for the display and
 * the wake word. */

#include <stdbool.h>
#include <stddef.h>

void web_settings_start(void);           /* once at boot, after net_start(): starts it if switched on */
void web_settings_set_enabled(bool on);  /* the Settings switch (saves the setting too) */
bool web_settings_running(void);
int  web_settings_sessions(void);        /* browsers signed in right now */
void web_settings_sign_out_all(void);    /* after a new PIN: every browser has to enter it again */
const char *web_settings_host(void);     /* "fusion-apps.local" */
