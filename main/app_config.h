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

/* Voice chat: close the server connection after this long without a turn or a touch; 0 = never.
 * The connection also closes when you go back to Home. Tap the mic (or the Connection switch on
 * the Voice server screen) to reconnect. */
#define VOICE_IDLE_DISCONNECT_MS (3 * 60 * 1000)

/* Connection diagnostics in the serial log (tag "vdiag"): every connect/disconnect with timings,
 * decoded error details, close codes, send times, and a health line every VOICE_DIAG_HEARTBEAT_S
 * seconds while Voice chat is open. 0 = off. */
#define VOICE_DIAG               1
#define VOICE_DIAG_HEARTBEAT_S   30
/* Also turn on the WebSocket/TLS libraries' own debug logs (very chatty: every ping/pong).
 * Needs CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y: see README, "Diagnosing connection drops". */
#define VOICE_DIAG_VERBOSE       0

/* Defaults used until the user changes them in Settings */
#define DEFAULT_DARK_MODE   1
#define DEFAULT_BRIGHTNESS  80       /* % */
#define DEFAULT_VOLUME      75       /* % */

/* Stocks app: client for the FusionStockWatchListMCP server (Amplify Fusion MCP over HTTPS).
 * The server URL and API key are in secrets.h (first boot) and on the device (gear button). */
#define STOCKS_REFRESH_MS    (60 * 1000)   /* auto refresh while the app is open */
#define STOCKS_RETRY_MS      (15 * 1000)   /* after a failed refresh */
#define STOCKS_KEY_HEADER    "x-api-key"   /* HTTP header that carries the API key */
#define STOCKS_TOOL_QUOTES   "GetStockWatchList"
#define STOCKS_TOOL_LIST     "GetStockWatchListSymbols"
#define STOCKS_TOOL_ADD      "AddSymbolToStockWatchList"
#define STOCKS_TOOL_REMOVE   "RemoveSymbolFromStockWatchList"
#define STOCKS_SYMBOL_ARG    "symbol"      /* argument name of the add/remove tools */
#define DEFAULT_STOCKS_AUTO  1             /* auto refresh on at first boot */
#define MCP_DEBUG            0             /* 1 = log the first 300 bytes of every MCP reply (tag "mcp") */
