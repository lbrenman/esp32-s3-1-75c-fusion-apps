#pragma once
/* App-wide settings that are compiled in. Values the user can change on the device
 * (Wi-Fi, server URL, API key, dark mode, brightness, volume) live in settings_store.c. */

#define APP_NAME          "Fusion Apps"
#define APP_VERSION       "0.1.0"

/* Home-screen clock. POSIX time zone string (this one is US Eastern, with DST). */
#define APP_TZ            "EST5EDT,M3.2.0,M11.1.0"
#define APP_NTP_SERVER    "pool.ntp.org"
#define APP_CLOCK_24H     0          /* 1 = 16:40, 0 = 4:40 PM */

/* Display: LVGL draws the screen in bands of this many lines. Each band needs a DMA buffer of
 * 466 x lines x 2 bytes in internal RAM while it's sent (two at once with double buffering).
 * The BSP's default is 50 lines (45 KB each), too big to find next to the wake word and TLS. */
#define APP_LVGL_BUF_LINES 12        /* about 10 KB per band; must be even */

/* Screen transitions */
#define UI_ANIM_MS        220        /* slide time between screens; 0 = instant */

/* Swipe detection on the voice-chat screen (which also uses touch-and-hold to talk) */
#define UI_SWIPE_MIN_PX   60         /* travel that counts as a swipe */
#define UI_SWIPE_MAX_MS   800        /* a swipe must happen this soon after touching */

/* Voice chat: close the server connection after this long without a turn or a touch; 0 = never.
 * The connection also closes when you go back to Home. Tap the mic (or the Connection switch on
 * the Voice server screen) to reconnect. Not used while the wake word is on: the connection then
 * stays up as long as the Voice chat screen is open, so the device is ready when you say the word. */
#define VOICE_IDLE_DISCONNECT_MS (3 * 60 * 1000)

/* Voice chat wake word (ESP-SR WakeNet). While the Voice chat screen is open and the wake word is
 * on (Settings > Voice chat), say the word, then your request; the turn ends when you stop talking.
 * Hold to talk keeps working, and a quick tap listens hands-free.
 * The model is chosen in sdkconfig.defaults (CONFIG_SR_WN_WN9_JARVIS_TTS) and `idf.py flash` writes
 * it to the "model" partition. To use another built-in word, change the CONFIG_SR_WN_* line and
 * these two names, then `rm sdkconfig` and rebuild. */
#define WAKE_WORD_LABEL      "Jarvis"      /* shown on screen */
#define WAKE_WORD_MODEL      "jarvis"      /* part of the model's name: wn9_jarvis_tts */
#define DEFAULT_WAKE_WORD    1             /* wake word on at first boot */
#define WAKE_CHIME           1             /* short tone when the wake word is heard; 0 = silent */
#define WAKE_END_SILENCE_MS  800           /* this much silence after you speak ends the turn */
#define WAKE_NO_SPEECH_MS    5000          /* give up if you don't start talking within this */
#define WAKE_HOLD_TAP_MS     350           /* a touch shorter than this listens hands-free */
#define WAKE_DEBUG           1             /* log the voice level 4x a second while listening */
/* The wake word needs some internal RAM, which the display needs too. If less than this is left
 * after it starts, it turns itself off and Voice chat goes back to hold to talk (see the log). */
#define WAKE_MIN_FREE_INTERNAL_KB  20
#define WAKE_MIN_DMA_BLOCK_KB      12
/* Checked BEFORE the engine starts, so it doesn't take RAM the display is about to need: about
 * what the engine takes (15 KB measured), and how much bigger than WAKE_MIN_DMA_BLOCK_KB the
 * largest DMA block has to be. If there isn't that much, the wake word stays off for this visit. */
#define WAKE_START_COST_KB         15
#define WAKE_START_DMA_MARGIN_KB   8

/* Waiting beep: while a voice app waits for the server's reply ("Thinking..." / "Translating..."),
 * a short, quiet tone plays every THINK_BEEP_PERIOD_MS, so you know it's still working. It starts
 * THINK_BEEP_DELAY_MS after you stop talking and stops as soon as the reply audio arrives.
 * Voice chat, Voice AI and the Translator. 0 = off. */
#define THINK_BEEP              1
#define THINK_BEEP_DELAY_MS     1000      /* quiet for this long first (fast replies get no beep) */
#define THINK_BEEP_PERIOD_MS    1500      /* one beep this often */
#define THINK_BEEP_HZ           880       /* pitch */
#define THINK_BEEP_MS           60        /* length of one beep */
#define THINK_BEEP_LEVEL        1200      /* loudness, 0-32767 before the volume setting (the wake chime is 5000) */

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
#define STOCKS_TOOL_QUOTE    "GetStockQuote"           /* detailed quote when you tap a row */
#define STOCKS_SYMBOL_ARG    "symbol"      /* argument name of the add/remove tools */
#define DEFAULT_STOCKS_AUTO  1             /* auto refresh on at first boot */
#define MCP_DEBUG            0             /* 1 = log the first 300 bytes of every MCP reply (tag "mcp") */

/* Quotes app: a second stocks app, for the Stock API (plain REST over HTTPS, not MCP).
 *   GET <base>/quote?symbol=AAPL             -> {"Quote":{"Symbol":..,"Price":..,"DayHigh":..}}
 *   GET <base>/watchlist?symbols=INTC,TXN    -> {"Watchlist":[{"Symbol":..,"Price":..}, ...]}
 * with the API key in the x-api-key header. HTTP 204 means "Symbol(s) not found".
 * The base URL and API key are in secrets.h (first boot) and on the device (Quotes > Stock API).
 * Which symbols the watchlist shows is picked on the device and saved; it's not kept on the server. */
#define QT_PATH_QUOTE        "/quote"           /* added to the base URL */
#define QT_PATH_WATCHLIST    "/watchlist"
#define QT_KEY_HEADER        "x-api-key"        /* HTTP header that carries the API key */
#define QT_TIMEOUT_MS        15000
#define QT_REFRESH_MS        (60 * 1000)        /* the open Watchlist or Quote screen refreshes this often */
#define QT_RETRY_MS          (15 * 1000)        /* after a failed watchlist load */
#define QT_SEL_MAX           16                 /* symbols you can pick for the watchlist (one Picked page) */
#define DEFAULT_QT_SYMBOLS   "AAPL,INTC,TXN"    /* first boot; secrets.h STOCK_API_SYMBOLS overrides */
#define QT_DEBUG             0                  /* 1 = log the first 300 bytes of every reply (tag "quotes") */

/* Translator app: push-to-talk client for the Amplify Fusion voice translator (WebSocket).
 * The server base URL and API key are in secrets.h (first boot) and on the device (swipe up);
 * the app connects to <base>/translate?sourceLanguage=..&targetLanguage=..&x-api-key=..
 * The languages are picked on the device (tap the language bar) and saved. */
#define DEFAULT_TR_SOURCE      "en"          /* first boot; secrets.h TRANSLATE_SOURCE_LANG overrides */
#define DEFAULT_TR_TARGET      "es"
#define TR_PATH                "/translate"  /* added to the base URL */
#define TR_AUDIO_WAIT_MS       15000         /* TRANSLATION announced audio: give up waiting after this */

/* Voice AI app: a second voice assistant, for the Voice AI Chat API (REST, not WebSocket).
 * Each turn is one HTTPS POST of the recording as multipart/form-data (a "file" part, 16 kHz mono
 * WAV) to <base>/voice-ai-chat?format=mp3&conversationId=..., with the API key in the x-api-key
 * header. The JSON reply has transcript, reply and audioBase64 (the spoken reply).
 * The base URL and API key are in secrets.h (first boot) and on the device (swipe up). */
#define AI_CHAT_PATH         "/voice-ai-chat"   /* added to the base URL */
#define AI_CHAT_FORMAT       "mp3"              /* reply audio: "mp3" (smaller) or "wav" */
#define AI_CHAT_LANGUAGE     ""                 /* "" = the server detects it; or "en", "es"... (also the reply's language) */
#define AI_CHAT_VOICE        ""                 /* "" = the server's default voice; or e.g. "voice_478" */
#define AI_CHAT_TIMEOUT_MS   60000              /* one turn is three model calls: allow time */

/* Phone settings page (web_settings.c): a small web server on the watch. Open
 * http://WEB_HOSTNAME.local (or the IP shown in Settings > Phone) on a phone or computer on the same
 * Wi-Fi, enter the PIN shown in Settings > Phone, and change the same settings as on the watch
 * (everything except Wi-Fi). Plain HTTP on your own network; the PIN guards it. */
#define DEFAULT_WEB_SETTINGS   1             /* page on at first boot; Settings > Phone switches it */
#define WEB_HOSTNAME           "fusion-apps" /* -> http://fusion-apps.local (mDNS) */
#define WEB_PIN_MAX_TRIES      5             /* wrong PINs in a row before the page locks... */
#define WEB_PIN_LOCK_S         60            /* ...for this many seconds */
#define WEB_MAX_SESSIONS       4             /* phones/browsers signed in at once (until reboot or New PIN) */
