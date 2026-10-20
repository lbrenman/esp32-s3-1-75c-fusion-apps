#pragma once
/* App-wide settings that are compiled in. Values the user can change on the device
 * (Wi-Fi, server URL, API key, dark mode, brightness, volume) live in settings_store.c. */

#define APP_NAME          "Fusion Apps"
#define APP_VERSION       "0.1.0"

/* Home-screen clock. POSIX time zone string (this one is US Eastern, with DST). */
#define APP_TZ            "EST5EDT,M3.2.0,M11.1.0"
#define APP_NTP_SERVER    "pool.ntp.org"
#define APP_CLOCK_24H     0          /* 1 = 16:40, 0 = 4:40 PM */

/* Screen transitions */
#define UI_ANIM_MS        220        /* slide time between screens; 0 = instant */

/* Swipe detection on the voice-chat screen (which also uses touch-and-hold to talk) */
#define UI_SWIPE_MIN_PX   60         /* travel that counts as a swipe */
#define UI_SWIPE_MAX_MS   800        /* a swipe must happen this soon after touching */

/* Voice chat: go back to the home screen after this long idle; 0 = never */
#define VOICE_AUTO_HOME_MS (3 * 60 * 1000)

/* Defaults used until the user changes them in Settings */
#define DEFAULT_DARK_MODE   1
#define DEFAULT_BRIGHTNESS  80       /* % */
#define DEFAULT_VOLUME      75       /* % */
