/*
 * voice_core.c — the voice-chat engine (push-to-talk ported from voice_chat/main.c, plus a wake word)
 *
 * While the app is open: hold to record 16 kHz mono, release to send ONE binary WebSocket frame
 * (a WAV file) to the voice server. With the wake word on, the engine also listens while the
 * Voice chat screen is showing: say "Jarvis" (WAKE_WORD_LABEL), then your request; ESP-SR's voice
 * activity detection ends the turn when you stop talking, and the same WAV goes to the server.
 * The server's JSON text frames (TRANSCRIBED_REQUEST / RESPONSE / ERROR) become on-screen text,
 * and its binary reply audio (MP3, WAV or raw 16-bit PCM, detected automatically) plays through
 * the speaker. MP3 is decoded on the device with minimp3 (CC0).
 *
 * The WebSocket is opened when the app opens and closed when it closes. Server URL and API key come
 * from the settings store (first boot: secrets.h), so they can be changed on the device.
 *
 * Translator mode (VOICE_MODE_TRANSLATE, set by the Translator app) uses the same recording,
 * sending and playback, with a different connection and different text:
 *   connect   <tr_url>/translate?sourceLanguage=en&targetLanguage=es&x-api-key=...
 *   send      the same WAV (16 kHz mono), one binary frame per turn; hold to talk only (no wake word)
 *   receive   {"type":"TRANSLATION","input":{"text":...},"output":{"text":...,"audio":{...}}}, then the
 *             spoken translation as one binary frame (only if output.audio was announced).
 *             The older TRANSCRIBED_REQUEST / RESPONSE events and ERROR work too.
 * Text keeps its accents (UTF-8 for the ui_font_latin_* fonts). See the protocol README of the
 * translator server for the full contract.
 *
 * Audio uses Waveshare's bsp_extra helper on top of the 1.75C BSP (the same calls as the working
 * 05_Spec_Analyzer example).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"     // xTaskCreatePinnedToCoreWithCaps

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "cJSON.h"

#include "bsp_board_extra.h"

/* ESP-SR: audio front end (AFE) with WakeNet (wake word) and VAD (voice activity detection) */
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "model_path.h"

#include "voice.h"
#include "translate_langs.h"
#include "text_ascii.h"
#include "net.h"
#include "settings_store.h"
#include "app_config.h"

/* minimp3: public-domain (CC0) MP3 decoder, implementation compiled in minimp3_impl.c */
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

#define TAG "voice"

/* The server's certificate chain, embedded from main/server_ca.pem (see README, "TLS certificate").
 * Used when the server URL points at the same host as WS_HOST in secrets.h; any other host is
 * checked against ESP-IDF's built-in CA bundle. */
extern const char server_ca_pem_start[] asm("_binary_server_ca_pem_start");

/* ---------------------------------------------------------------- settings */
#define REC_RATE            16000           // server expects 16 kHz
#define I2S_CHANNELS        2               // bsp_extra runs the codec in stereo
#define READ_FRAMES         512             // frames per I2S read (32 ms)
#define REC_MAX_SECONDS     10              // recording limit
#define REC_MIN_MS          300             // ignore taps shorter than this
#define WAV_HEADER_BYTES    44
#define RX_MAX_BYTES        (3 * 1024 * 1024)   // largest single server message we accept
#define PLAY_FRAMES         512
#define TURN_TIMEOUT_US     (45LL * 1000 * 1000)
#define TEXT_ONLY_IDLE_US   (4LL * 1000 * 1000)  // reply text but no audio: return to ready after this
#define HEARD_MAX_CHARS     200             // bytes, Voice chat (ASCII)
#define REPLY_MAX_CHARS     320             // what fits on the round screen
/* Translator: UTF-8, where a Hindi or Arabic letter takes 2-3 bytes */
#define TR_HEARD_MAX        360
#define TR_REPLY_MAX        600
#define HEARD_CAP()         (s_mode == VOICE_MODE_TRANSLATE ? TR_HEARD_MAX : HEARD_MAX_CHARS)
#define REPLY_CAP()         (s_mode == VOICE_MODE_TRANSLATE ? TR_REPLY_MAX : REPLY_MAX_CHARS)
#define REPLAY_MAX_CHUNKS   16              // Translator replay: binary frames kept from the last reply
#define NOTE_MAX_CHARS      96
#define RESEND_WAIT_MS      20000           // after a failed send: wait this long for the reconnect, then resend once
#define MAX_SAMPLES         ((size_t)REC_MAX_SECONDS * REC_RATE)

/* wake word engine (see "wake word" below) */
#define WAKE_PREROLL_MS     400             // audio kept from just before you start talking
#define WAKE_CHIME_IGNORE_MS 400            // the mic hears the chime: don't count it as speech
#define WAKE_MIN_SPEECH_MS  400             // less voice than this (the end of "Jarvis", a cough): keep waiting
#define WAKE_LEVEL_ABOVE_DB 12              // voice: this much louder than the room's noise floor...
#define WAKE_LEVEL_MIN_DB   (-50)           // ...and at least this loud (dBFS)
#define WAKE_REARM_MS       700             // after a reply: ignore wake words for this long (speaker echo)
#define MODEL_PARTITION     "model"

/* ------------------------------------------------------------------- state */
typedef struct {
    uint8_t *data;
    size_t   len;
} play_item_t;

enum { CMD_WAKE = 1, CMD_RESTART = 2 };
enum { REC_CMD_PTT = 1, REC_CMD_SEND = 2 };         /* rec_task: record while held / send what the wake engine captured */

static volatile voice_state_t s_state = VOICE_OFF;
static volatile voice_mode_t  s_mode = VOICE_MODE_CHAT;  /* which app is using the engine */
static volatile bool    s_active = false;           // the app is open
static volatile bool    s_want = true;              // connection switch (Voice server screen); on by default
#define LIVE()          (s_active && s_want)        // app open and connection wanted
static volatile bool    s_replaying = false;        // Translator: playing the last reply again
#define PLAY_OK()       (s_active && (s_want || s_replaying))   // replay works while disconnected too
static volatile bool    s_recording = false;
static volatile bool    s_resending = false;        // a failed send is waiting for the reconnect
static volatile bool    s_rec_cancel = false;       // set by a swipe: discard the recording
static volatile bool    s_ws_connected = false;
static volatile bool    s_got_text = false;
static volatile bool    s_cfg_error = false;        // no usable URL: don't retry until it changes
static volatile bool    s_restart_pending = false;  // Translator languages changed during a turn: reconnect after it
static volatile bool    s_tr_audio_wait = false;    // Translator: TRANSLATION announced audio that hasn't arrived
static volatile int64_t s_tr_audio_deadline_us = 0;
static bool             s_audio_ok = false;         // codec + tasks started
static volatile int64_t s_last_activity_us = 0;
static volatile int64_t s_turn_start_us = 0;
static volatile int64_t s_last_rx_us = 0;

/* wake word */
static srmodel_list_t  *s_models = NULL;            /* models in the "model" partition */
static char            *s_wn_name = NULL;           /* the wake word model, e.g. "wn9_jarvis_tts" */
static volatile bool    s_wake_enabled = false;     /* Settings switch */
static volatile bool    s_wake_nomem = false;       /* turned itself off: not enough internal RAM */
static volatile bool    s_fg = false;               /* the Voice chat screen is showing */
static volatile bool    s_wl_running = false;       /* the wake engine runs and owns the microphone */
static volatile bool    s_rec_busy = false;         /* rec_task is reading the microphone (hold to talk) */
static volatile bool    s_hold_req = false;         /* a touch asked the wake engine to start recording */
static volatile size_t  s_cap_n = 0;                /* samples the wake engine captured, for rec_task to send */
static TaskHandle_t     s_wake_task = NULL;

/* the recording, as a WAV file: header + up to REC_MAX_SECONDS of 16-bit mono (PSRAM) */
static uint8_t *s_wav = NULL;
static int16_t *s_pcm = NULL;

static TaskHandle_t  s_rec_task = NULL;
static QueueHandle_t s_play_q = NULL;
static QueueHandle_t s_ctl_q = NULL;
static SemaphoreHandle_t s_ws_mtx = NULL;           // guards s_ws between send and stop
static SemaphoreHandle_t s_txt_mtx = NULL;
static esp_websocket_client_handle_t s_ws = NULL;

static char s_url[768];
static char s_conn_src[SET_LANG_MAX + 1], s_conn_dst[SET_LANG_MAX + 1];   /* Translator: pair in the URL */

/* on-screen text (guarded by s_txt_mtx) */
static char s_heard[TR_HEARD_MAX + 4];
static char s_reply[TR_REPLY_MAX + 8];
static char s_note[NOTE_MAX_CHARS + 1];
static char s_txt_src[SET_LANG_MAX + 1], s_txt_dst[SET_LANG_MAX + 1];     /* Translator: pair of the text */
static volatile uint32_t s_text_ver = 1;

/* receive reassembly (filled from the websocket task) */
static uint8_t *s_rx_buf = NULL;
static size_t   s_rx_len = 0;
static size_t   s_rx_cap = 0;
static int      s_rx_op  = 0;

/* ---------------------------------------------------------- state + text */
static void set_state(voice_state_t st)
{
    if (!s_active && st != VOICE_OFF) return;     /* app closed: stay off */
    if (st == VOICE_READY) s_last_activity_us = esp_timer_get_time();
    s_state = st;
}

static void text_lock(void)   { xSemaphoreTake(s_txt_mtx, portMAX_DELAY); }
static void text_unlock(void) { s_text_ver++; xSemaphoreGive(s_txt_mtx); }

/* Translator replay: a copy of the last reply's audio frames (PSRAM), dropped with the text */
static play_item_t       s_rep[REPLAY_MAX_CHUNKS];
static int               s_rep_n = 0;
static size_t            s_rep_bytes = 0;
static SemaphoreHandle_t s_rep_mtx = NULL;

static void replay_clear(void)
{
    if (!s_rep_mtx) return;
    xSemaphoreTake(s_rep_mtx, portMAX_DELAY);
    for (int i = 0; i < s_rep_n; i++) heap_caps_free(s_rep[i].data);
    s_rep_n = 0;
    s_rep_bytes = 0;
    xSemaphoreGive(s_rep_mtx);
}

static void replay_add(const uint8_t *data, size_t len)
{
    if (!s_rep_mtx) return;
    xSemaphoreTake(s_rep_mtx, portMAX_DELAY);
    if (s_rep_n < REPLAY_MAX_CHUNKS && s_rep_bytes + len <= RX_MAX_BYTES) {
        uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (copy) {
            memcpy(copy, data, len);
            s_rep[s_rep_n].data = copy;
            s_rep[s_rep_n].len = len;
            s_rep_n++;
            s_rep_bytes += len;
        }
    }
    xSemaphoreGive(s_rep_mtx);
}

static void text_clear(void)
{
    text_lock();
    s_heard[0] = s_reply[0] = s_note[0] = '\0';
    s_got_text = false;
    text_unlock();
    replay_clear();                     /* a new turn: the old reply can't be replayed any more */
}

static void set_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_note(const char *fmt, ...)
{
    text_lock();
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_note, sizeof(s_note), fmt, ap);
    va_end(ap);
    text_unlock();
}

/* Drops a UTF-8 character cut off at the end of s (after a byte-limited copy). */
static void utf8_fix_tail(char *s)
{
    size_t n = strlen(s), i = n;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;    /* continuation bytes */
    if (i == 0) return;
    unsigned char lead = (unsigned char)s[i - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (n - (i - 1) < need) s[i - 1] = '\0';
}

static void set_heard(const char *text)
{
    text_lock();
    if (s_mode == VOICE_MODE_TRANSLATE) {
        strlcpy(s_heard, text, TR_HEARD_MAX + 1);
        utf8_fix_tail(s_heard);
    } else {
        snprintf(s_heard, HEARD_MAX_CHARS + 3, "\"%s\"", text);    /* as before: fits Voice chat's buffer */
    }
    text_unlock();
}

/* Translator: the translation replaces whatever is shown (TRANSLATION.output.text) */
static void set_reply(const char *text)
{
    text_lock();
    strlcpy(s_reply, text, TR_REPLY_MAX + 1);
    if (strlen(text) > TR_REPLY_MAX) {
        s_reply[TR_REPLY_MAX - 3] = '\0';
        utf8_fix_tail(s_reply);
        strcat(s_reply, "...");
    }
    s_note[0] = '\0';
    text_unlock();
}

static void set_text_langs(const char *src, const char *dst)
{
    text_lock();
    if (src && src[0]) strlcpy(s_txt_src, src, sizeof(s_txt_src));
    if (dst && dst[0]) strlcpy(s_txt_dst, dst, sizeof(s_txt_dst));
    text_unlock();
}

void voice_text_langs(char *src, size_t src_len, char *dst, size_t dst_len)
{
    xSemaphoreTake(s_txt_mtx, portMAX_DELAY);
    if (src) strlcpy(src, s_txt_src, src_len);
    if (dst) strlcpy(dst, s_txt_dst, dst_len);
    xSemaphoreGive(s_txt_mtx);
}

static void reply_append(const char *text)
{
    text_lock();
    size_t cur = strlen(s_reply);
    const size_t cap = REPLY_CAP();
    if (cur > 0 && cur < cap) {
        strncat(s_reply, "\n", cap - cur);
        cur++;
    }
    if (cur < cap) {
        strncat(s_reply, text, cap - cur);
    }
    if (strlen(text) + cur > cap) {
        s_reply[cap - 3] = '\0';
        utf8_fix_tail(s_reply);          /* Translator text is UTF-8 */
        strcat(s_reply, "...");
    }
    s_note[0] = '\0';
    text_unlock();
}

uint32_t voice_text_version(void) { return s_text_ver; }

void voice_text_get(char *heard, size_t heard_len, char *reply, size_t reply_len, char *note, size_t note_len)
{
    xSemaphoreTake(s_txt_mtx, portMAX_DELAY);
    if (heard) strlcpy(heard, s_heard, heard_len);
    if (reply) strlcpy(reply, s_reply, reply_len);
    if (note)  strlcpy(note, s_note, note_len);
    xSemaphoreGive(s_txt_mtx);
}

voice_state_t voice_state(void) { return s_state; }
bool          voice_connected(void) { return s_ws_connected; }
int64_t       voice_idle_us(void) { return esp_timer_get_time() - s_last_activity_us; }

const char *voice_state_text(voice_state_t st)
{
    switch (st) {
    case VOICE_OFF:        return "Off";
    case VOICE_WAIT_WIFI:  return "Waiting for Wi-Fi...";
    case VOICE_CONNECTING: return "Connecting...";
    case VOICE_READY:      return s_wl_running ? "Say \"" WAKE_WORD_LABEL "\"" : "Hold to talk";
    case VOICE_LISTENING:  return "Listening...";
    case VOICE_THINKING:   return s_mode == VOICE_MODE_TRANSLATE ? "Translating..." : "Thinking...";
    case VOICE_SPEAKING:   return "Speaking...";
    case VOICE_ERROR:      return "Not connected";
    case VOICE_DISCONNECTED: return "Disconnected";
    }
    return "";
}

/* -------------------------------------------------------- diagnostics
 * One-line connection health snapshots under the "vdiag" tag, so a serial log of a session
 * shows when and why the link dropped. Never logs the URL query (API key). */
#define DTAG "vdiag"

typedef struct {
    int      attempts, connects, drops;
    int64_t  attempt_us;          /* when the current connect attempt started */
    int64_t  connected_us;        /* when the link came up (0 = down) */
    int64_t  last_rx_us, last_tx_us, last_pong_us, last_ping_us;
    uint32_t rx_frames, tx_frames, pongs, pings;
    uint64_t rx_bytes, tx_bytes;
    int64_t  last_beat_us;
} vdiag_t;

static vdiag_t s_dg;

/* seconds since t, as text ("-" if never) */
static const char *ago(int64_t t, char *buf, size_t len)
{
    if (t <= 0) { snprintf(buf, len, "-"); return buf; }
    snprintf(buf, len, "%.1fs", (esp_timer_get_time() - t) / 1e6);
    return buf;
}

static const char *net_name(void)
{
    switch (net_state()) {
    case NET_UP:         return "up";
    case NET_CONNECTING: return "connecting";
    default:             return "not-set";
    }
}

static void diag(const char *why)
{
#if VOICE_DIAG
    char a[16], b[16], c[16], d[16], e[16];
    ESP_LOGI(DTAG, "%s | state=%s want=%d active=%d link=%d up=%s | last rx %s tx %s pong %s ping %s"
             " | rx %u msgs %u KB, tx %u msgs %u KB | wifi %s %d dBm | heap int %u KB (dma blk %u KB) psram %u KB",
             why, voice_state_text(s_state), s_want, s_active, s_ws_connected,
             ago(s_dg.connected_us, a, sizeof(a)),
             ago(s_dg.last_rx_us, b, sizeof(b)), ago(s_dg.last_tx_us, c, sizeof(c)),
             ago(s_dg.last_pong_us, d, sizeof(d)), ago(s_dg.last_ping_us, e, sizeof(e)),
             (unsigned)s_dg.rx_frames, (unsigned)(s_dg.rx_bytes / 1024),
             (unsigned)s_dg.tx_frames, (unsigned)(s_dg.tx_bytes / 1024),
             net_name(), net_rssi(),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
#endif
}

static const char *ws_err_type_name(int t)
{
    switch (t) {   /* numeric: older esp_websocket_client versions lack some enum names */
    case 0: return "none";
    case 1: return "TCP/TLS transport";
    case 2: return "pong timeout";
    case 3: return "handshake";
    case 4: return "server close";
    default: return "unknown";
    }
}

static void diag_error(const esp_websocket_event_data_t *d)
{
#if VOICE_DIAG
    const esp_websocket_error_codes_t *eh = &d->error_handle;
    int t = (int)eh->error_type;
    if (t < 1 || t > 4) {
        /* the library fills these in for connect/handshake/pong errors, not for a failed write */
        ESP_LOGW(DTAG, "ERROR: no details from the library (see the websocket_client line above)");
        diag("after ERROR");
        return;
    }
    ESP_LOGW(DTAG, "ERROR type=%d (%s) handshake_status=%d tls_err=0x%x (%s) tls_stack=-0x%04x cert_flags=0x%x sock_errno=%d (%s)",
             (int)eh->error_type, ws_err_type_name((int)eh->error_type), eh->esp_ws_handshake_status_code,
             (unsigned)eh->esp_tls_last_esp_err, esp_err_to_name(eh->esp_tls_last_esp_err),
             (unsigned)(-eh->esp_tls_stack_err), (unsigned)eh->esp_tls_cert_verify_flags,
             eh->esp_transport_sock_errno, eh->esp_transport_sock_errno ? strerror(eh->esp_transport_sock_errno) : "-");
    diag("after ERROR");
#endif
}

static void diag_heartbeat(void)
{
#if VOICE_DIAG && VOICE_DIAG_HEARTBEAT_S > 0
    int64_t now = esp_timer_get_time();
    if (!s_active || now - s_dg.last_beat_us < (int64_t)VOICE_DIAG_HEARTBEAT_S * 1000000) return;
    s_dg.last_beat_us = now;
    diag("heartbeat");
#endif
}

/* ------------------------------------------------------------- helpers */
static void put_le16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24; }
static uint16_t get_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void write_wav_header(uint8_t *h, uint32_t data_bytes, uint32_t rate, uint16_t channels, uint16_t bits)
{
    uint16_t block_align = channels * bits / 8;
    memcpy(h, "RIFF", 4);
    put_le32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);                 // fmt chunk size
    put_le16(h + 20, 1);                  // PCM
    put_le16(h + 22, channels);
    put_le32(h + 24, rate);
    put_le32(h + 28, rate * block_align); // byte rate
    put_le16(h + 32, block_align);
    put_le16(h + 34, bits);
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, data_bytes);
}

/* Finds fmt + data chunks. Returns false if not a usable PCM WAV. */
static bool parse_wav(const uint8_t *buf, size_t len, const uint8_t **pcm, size_t *pcm_len,
                      uint32_t *rate, uint16_t *channels, uint16_t *bits)
{
    if (len < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        return false;
    }
    bool have_fmt = false;
    size_t pos = 12;
    while (pos + 8 <= len) {
        const uint8_t *id = buf + pos;
        uint32_t size = get_le32(buf + pos + 4);
        size_t body = pos + 8;

        if (memcmp(id, "fmt ", 4) == 0 && body + 16 <= len) {
            uint16_t fmt = get_le16(buf + body);
            *channels = get_le16(buf + body + 2);
            *rate     = get_le32(buf + body + 4);
            *bits     = get_le16(buf + body + 14);
            if (fmt != 1 && fmt != 0xFFFE) {
                ESP_LOGW(TAG, "WAV format %u is not PCM", fmt);
                return false;
            }
            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            if (!have_fmt) return false;
            size_t avail = len - body;
            *pcm = buf + body;
            /* streaming WAVs often carry 0 or 0xFFFFFFFF as the data size */
            *pcm_len = (size == 0 || size == 0xFFFFFFFF || size > avail) ? avail : size;
            return true;
        }
        pos = body + size + (size & 1);
        if (size == 0xFFFFFFFF) break;
    }
    return false;
}

static void url_encode(const char *in, char *out, size_t out_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < out_len; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = '\0';
}

/* Copies the host part of "wss://host:port/path?query" into out. */
static void url_host(const char *url, char *out, size_t out_len)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t n = strcspn(p, ":/?#");
    if (n >= out_len) n = out_len - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

/* -------------------------------------------------------- server messages */

/* Text for the screen. Voice chat: ASCII look-alikes. Translator: Latin with accents, Cyrillic,
 * Arabic and Devanagari (pre-shaped); text mostly in a script the fonts can't draw (Chinese,
 * Japanese, Korean) becomes a short note naming the language instead of a row of question marks. */
static void display_text(const char *in, char *out, size_t out_len, const char *lang)
{
    if (s_mode != VOICE_MODE_TRANSLATE) {
        text_to_ascii(in, out, out_len);
        return;
    }
    size_t missing = text_to_intl(in, out, out_len);   /* WebSocket task only (static buffers) */
    if (missing == 0) return;
    size_t letters = 0;                       /* characters that aren't spaces or ASCII punctuation */
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if ((*p & 0xC0) == 0x80) continue;    /* continuation byte */
        if (*p >= 0x80 || isalnum(*p)) letters++;
    }
    if (missing * 2 >= letters) {
        const tr_lang_t *l = tr_lang_find(lang);
        snprintf(out, out_len, "(%s text - this screen can't show it)", l ? l->name : "The");
    }
}

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* Translator: {"type":"TRANSLATION","sourceLanguage":"en","targetLanguage":"es",
 *              "input":{"text":"..."},"output":{"text":"...","audio":{"mimeType":"audio/mpeg","bytes":N}}} */
static void handle_translation(cJSON *obj)
{
    static char t[1024];   /* WebSocket task only */
    const char *src = json_str(obj, "sourceLanguage");
    const char *dst = json_str(obj, "targetLanguage");
    set_text_langs(src, dst);
    char cur_src[SET_LANG_MAX + 1], cur_dst[SET_LANG_MAX + 1];
    voice_text_langs(cur_src, sizeof(cur_src), cur_dst, sizeof(cur_dst));

    cJSON *input = cJSON_GetObjectItem(obj, "input");
    cJSON *output = cJSON_GetObjectItem(obj, "output");
    const char *in_text = cJSON_IsObject(input) ? json_str(input, "text") : NULL;
    const char *out_text = cJSON_IsObject(output) ? json_str(output, "text") : NULL;
    cJSON *audio = cJSON_IsObject(output) ? cJSON_GetObjectItem(output, "audio") : NULL;
    bool has_audio = cJSON_IsObject(audio);

    if (in_text) {
        display_text(in_text, t, sizeof(t), cur_src);
        set_heard(t);
    }
    if (out_text) {
        display_text(out_text, t, sizeof(t), cur_dst);
        set_reply(t);
    }
    /* the texts go to the log as received (UTF-8); they're what was said, not secrets */
    ESP_LOGI(TAG, "Translation %s -> %s: \"%s\" -> \"%s\"%s", cur_src, cur_dst, in_text ? in_text : "",
             out_text ? out_text : "", has_audio ? " (audio follows)" : " (text only)");

    if (has_audio) {
        cJSON *bytes = cJSON_GetObjectItem(audio, "bytes");
        const char *mime = json_str(audio, "mimeType");
        ESP_LOGI(TAG, "Spoken translation announced: %s, %d bytes", mime ? mime : "?",
                 cJSON_IsNumber(bytes) ? bytes->valueint : -1);
        s_tr_audio_deadline_us = esp_timer_get_time() + (int64_t)TR_AUDIO_WAIT_MS * 1000;
        s_tr_audio_wait = true;
    } else {
        /* text only: the turn is done */
        s_tr_audio_wait = false;
        if (s_state == VOICE_THINKING) set_state(VOICE_READY);
    }
}

static void handle_event_obj(cJSON *obj)
{
    static char m[1024];   /* display-safe copy of "message" (called from the WebSocket task only) */
    cJSON *type = cJSON_GetObjectItem(obj, "type");
    cJSON *msg  = cJSON_GetObjectItem(obj, "message");
    bool translate = (s_mode == VOICE_MODE_TRANSLATE);
    char src[SET_LANG_MAX + 1], dst[SET_LANG_MAX + 1];
    voice_text_langs(src, sizeof(src), dst, sizeof(dst));
    const char *msg_lang = dst;
    if (cJSON_IsString(type) && strcmp(type->valuestring, "TRANSCRIBED_REQUEST") == 0) msg_lang = src;
    display_text(cJSON_IsString(msg) ? msg->valuestring : "", m, sizeof(m), msg_lang);

    if (!cJSON_IsString(type)) {
        /* object without a type: show it compactly */
        char *s = cJSON_PrintUnformatted(obj);
        if (s) { display_text(s, m, sizeof(m), dst); reply_append(m); cJSON_free(s); }
        s_got_text = true;
        return;
    }

    if (translate && strcmp(type->valuestring, "TRANSLATION") == 0) {
        handle_translation(obj);
    } else if (strcmp(type->valuestring, "TRANSCRIBED_REQUEST") == 0) {
        ESP_LOGI(TAG, "Heard: %s", m);
        set_heard(m);
    } else if (strcmp(type->valuestring, "RESPONSE") == 0) {
        ESP_LOGI(TAG, "Reply: %s", m);
        reply_append(m);
        s_got_text = true;
    } else if (translate && strcmp(type->valuestring, "ERROR") == 0) {
        /* {"type":"ERROR","code":"...","stage":"transcribe","message":"...","retryable":true} */
        const char *code = json_str(obj, "code");
        const char *stage = json_str(obj, "stage");
        ESP_LOGE(TAG, "Server error%s%s%s%s: %s", code ? " " : "", code ? code : "",
                 stage ? " at " : "", stage ? stage : "", m);
        set_note("%s", m[0] ? m : "Server error");
        s_tr_audio_wait = false;              /* e.g. SPEECH_FAILED: no audio is coming */
        if (s_state == VOICE_THINKING) set_state(VOICE_READY);
    } else if (strcmp(type->valuestring, "ERROR") == 0) {
        ESP_LOGE(TAG, "Server error: %s", m);
        char e[1100];
        snprintf(e, sizeof(e), "Server error: %s", m);
        reply_append(e);
        s_got_text = true;
    } else {
        ESP_LOGW(TAG, "Ignoring message type %s", type->valuestring);
    }
}

static void handle_text(const char *text)
{
    cJSON *root = cJSON_Parse(text);
    if (!root) {
        /* plain text reply */
        static char t[1024];
        ESP_LOGI(TAG, "Text: %s", text);
        char dst[SET_LANG_MAX + 1];
        voice_text_langs(NULL, 0, dst, sizeof(dst));
        display_text(text, t, sizeof(t), dst);
        reply_append(t);
        s_got_text = true;
        return;
    }
    if (cJSON_IsArray(root)) {
        cJSON *item;
        cJSON_ArrayForEach(item, root) {
            if (cJSON_IsObject(item)) handle_event_obj(item);
        }
    } else if (cJSON_IsObject(root)) {
        handle_event_obj(root);
    }
    cJSON_Delete(root);
}

static bool rx_reserve(size_t need)
{
    if (need > RX_MAX_BYTES) return false;
    if (need <= s_rx_cap) return true;
    size_t cap = s_rx_cap ? s_rx_cap : 64 * 1024;
    while (cap < need) cap *= 2;
    if (cap > RX_MAX_BYTES) cap = RX_MAX_BYTES;
    uint8_t *p = heap_caps_realloc(s_rx_buf, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) return false;
    s_rx_buf = p;
    s_rx_cap = cap;
    return true;
}

static void handle_ws_data(const esp_websocket_event_data_t *d)
{
    if (d->op_code == 0x08) {   // close frame: first two bytes are the close code
        int code = (d->data_len >= 2) ? (((uint8_t)d->data_ptr[0] << 8) | (uint8_t)d->data_ptr[1]) : 0;
        char raw[96] = "", reason[64];
        int rlen = d->data_len > 2 ? d->data_len - 2 : 0;
        if (rlen > (int)sizeof(raw) - 1) rlen = sizeof(raw) - 1;
        if (rlen > 0) memcpy(raw, d->data_ptr + 2, rlen);
        raw[rlen] = '\0';
        text_to_ascii(raw, reason, sizeof(reason));
        ESP_LOGW(TAG, "Server closed the connection, code %d", code);
#if VOICE_DIAG
        ESP_LOGW(DTAG, "CLOSE frame from server: code=%d reason=\"%s\"", code, reason);
        diag("after server CLOSE");
#endif
        if (code == 1006) set_note("Server refused the connection - check the API key");
        else if (code == 1009) set_note("Recording too large for the server");
        else if (code == 1011) set_note("Server error (1011)");
        else if (code != 1000 && reason[0]) set_note("Server closed the connection (%d: %s)", code, reason);
        return;
    }
    if (d->op_code == 0x0A) { s_dg.last_pong_us = esp_timer_get_time(); s_dg.pongs++; return; }  // pong
    if (d->op_code == 0x09) { s_dg.last_ping_us = esp_timer_get_time(); s_dg.pings++; return; }  // ping from server

    if (d->op_code == 0x01 || d->op_code == 0x02) {
        if (d->payload_offset == 0) {
            s_rx_len = 0;
            s_rx_op = d->op_code;
        }
    } else if (d->op_code != 0x00) {
        return;   // unknown opcode
    }

    if (d->data_len > 0) {
        if (!rx_reserve(s_rx_len + d->data_len + 1)) {
            ESP_LOGE(TAG, "Incoming message too large or out of memory; dropping it");
            s_rx_len = 0;
            return;
        }
        memcpy(s_rx_buf + s_rx_len, d->data_ptr, d->data_len);
        s_rx_len += d->data_len;
    }

    bool frame_complete = (d->payload_offset + d->data_len >= d->payload_len);
    if (!frame_complete) return;

    s_last_rx_us = esp_timer_get_time();
    s_dg.last_rx_us = s_last_rx_us;
    s_dg.rx_frames++;
    s_dg.rx_bytes += s_rx_len;
#if VOICE_DIAG
    ESP_LOGI(DTAG, "rx %s message, %u bytes", s_rx_op == 0x01 ? "text" : "binary", (unsigned)s_rx_len);
#endif

    if (s_rx_op == 0x01) {
        if (!rx_reserve(s_rx_len + 1)) { s_rx_len = 0; return; }   // also covers empty frames
        s_rx_buf[s_rx_len] = '\0';
        handle_text((const char *)s_rx_buf);
    } else if (s_rx_op == 0x02 && s_rx_len > 0 && LIVE()) {
        play_item_t item = {
            .data = heap_caps_malloc(s_rx_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
            .len = s_rx_len,
        };
        if (item.data) {
            memcpy(item.data, s_rx_buf, s_rx_len);
            ESP_LOGI(TAG, "Received %u bytes of reply audio", (unsigned)s_rx_len);
            s_tr_audio_wait = false;
            if (s_mode == VOICE_MODE_TRANSLATE) replay_add(s_rx_buf, s_rx_len);
            if (xQueueSend(s_play_q, &item, 0) != pdTRUE) {
                ESP_LOGW(TAG, "Playback queue full; dropping audio");
                heap_caps_free(item.data);
            }
        }
    }
    s_rx_len = 0;
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        s_ws_connected = true;
        s_dg.connects++;
        s_dg.connected_us = esp_timer_get_time();
        s_dg.last_pong_us = 0;
        s_dg.last_beat_us = s_dg.connected_us;
#if VOICE_DIAG
        ESP_LOGI(DTAG, "CONNECTED #%d (attempt #%d) in %lld ms", s_dg.connects, s_dg.attempts,
                 (long long)((s_dg.connected_us - s_dg.attempt_us) / 1000));
        diag("on connect");
#endif
        if (s_want && !s_resending &&
            (s_state == VOICE_CONNECTING || s_state == VOICE_ERROR || s_state == VOICE_WAIT_WIFI)) {
            set_note("%s", "");
            set_state(VOICE_READY);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected (will retry)");
        if (s_ws_connected) s_dg.drops++;
#if VOICE_DIAG
        {
            char buf[48];
            snprintf(buf, sizeof(buf), "DISCONNECTED (drop #%d)", s_dg.drops);
            diag(buf);
        }
#endif
        s_ws_connected = false;
        s_dg.connected_us = 0;
        s_dg.attempt_us = esp_timer_get_time();   /* the client retries after reconnect_timeout_ms */
        if (s_want && !s_resending && s_state != VOICE_SPEAKING) set_state(VOICE_CONNECTING);
        break;
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "WebSocket closed");
        diag("CLOSED");
        s_ws_connected = false;
        s_dg.connected_us = 0;
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error (check URL, API key, TLS)");
        diag_error(d);
        if (!s_ws_connected) set_note("Can't reach the server - check the URL");
        break;
    case WEBSOCKET_EVENT_DATA:
        handle_ws_data(d);
        break;
    default:
        break;
    }
}

/* Translator: the endpoint for a base URL, as the web app builds it:
 *   wss://host:4443/translator             -> wss://host:4443/translator/translate
 *   wss://host:4443/translator/            -> wss://host:4443/translator/translate
 *   wss://host:4443/translator/translate   -> unchanged (never doubled)
 *   wss://host:4443/translator/speech      -> wss://host:4443/translator/translate
 * A query string on the base URL is kept. */
void voice_tr_endpoint(const char *base, char *out, size_t out_len)
{
    char path[SET_URL_MAX + 1], query[SET_URL_MAX + 1] = "";
    strlcpy(path, base, sizeof(path));
    char *q = strchr(path, '?');
    if (q) { strlcpy(query, q, sizeof(query)); *q = '\0'; }
    size_t n = strlen(path);
    while (n > 0 && path[n - 1] == '/') path[--n] = '\0';
    const char *speech = "/speech";
    size_t sl = strlen(speech), tl = strlen(TR_PATH);
    if (n >= sl && strcmp(path + n - sl, speech) == 0) path[n - sl] = '\0';
    n = strlen(path);
    bool has = n >= tl && strcmp(path + n - tl, TR_PATH) == 0;
    snprintf(out, out_len, "%s%s%s", path, has ? "" : TR_PATH, query);
}

/* ----------------------------------------------------- WebSocket lifecycle
 * Runs only in the control task, so starting/stopping never races and never
 * blocks the LVGL task. */
static void ws_start(void)
{
    app_settings_t cfg;
    settings_get(&cfg);

    bool translate = (s_mode == VOICE_MODE_TRANSLATE);
    const char *base = translate ? cfg.tr_url : cfg.ws_url;
    if (strncmp(base, "wss://", 6) != 0 && strncmp(base, "ws://", 5) != 0) {
        ESP_LOGE(TAG, "Server URL must start with wss:// (%s > swipe up > Server URL)",
                 translate ? "Translator" : "Voice chat");
        set_note("Set the server URL: swipe up");
        s_cfg_error = true;
        set_state(VOICE_ERROR);
        return;
    }

    char key_enc[3 * SET_KEY_MAX + 1];
    char endpoint[SET_URL_MAX + 32];
    if (translate) {
        if (!tr_pair_valid(cfg.tr_src, cfg.tr_dst)) {
            ESP_LOGE(TAG, "Translator languages %s -> %s aren't a usable pair", cfg.tr_src, cfg.tr_dst);
            set_note("Pick the languages: tap the language bar");
            s_cfg_error = true;
            set_state(VOICE_ERROR);
            return;
        }
        url_encode(cfg.tr_api_key, key_enc, sizeof(key_enc));
        voice_tr_endpoint(cfg.tr_url, endpoint, sizeof(endpoint));
        strlcpy(s_conn_src, cfg.tr_src, sizeof(s_conn_src));
        strlcpy(s_conn_dst, cfg.tr_dst, sizeof(s_conn_dst));
        snprintf(s_url, sizeof(s_url), "%s%ssourceLanguage=%s&targetLanguage=%s&x-api-key=%s", endpoint,
                 strchr(endpoint, '?') ? "&" : "?", s_conn_src, s_conn_dst, key_enc);
        /* never log the key */
        ESP_LOGI(TAG, "Connecting to %s (%s -> %s)", endpoint, s_conn_src, s_conn_dst);
    } else {
        url_encode(cfg.ws_api_key, key_enc, sizeof(key_enc));
        const char *conv = cfg.conversation_id;    /* saved in settings; same for every turn and reconnect */
        strlcpy(endpoint, cfg.ws_url, sizeof(endpoint));
        const char *sep = strchr(cfg.ws_url, '?') ? "&" : "?";
        snprintf(s_url, sizeof(s_url), "%s%sx-api-key=%s&conversationId=%s", cfg.ws_url, sep, key_enc, conv);
        /* never log the key */
        ESP_LOGI(TAG, "Connecting to %s (conversationId=%s)", cfg.ws_url, conv);
    }

    char host[128];
    url_host(endpoint, host, sizeof(host));

    esp_websocket_client_config_t wc = {
        .uri = s_url,
        .buffer_size = 4096,             // small: these buffers come from internal RAM
        .task_stack = 8192,
        .network_timeout_ms = 15000,
        .reconnect_timeout_ms = 5000,
        .ping_interval_sec = 20,
    };
    if (strcasecmp(host, settings_build_host()) == 0) {
        wc.cert_pem = server_ca_pem_start;           /* the chain fetched at build time */
    } else {
        wc.crt_bundle_attach = esp_crt_bundle_attach; /* any other server: built-in CA bundle */
        ESP_LOGI(TAG, "Host %s differs from WS_HOST; using the built-in CA bundle", host);
    }

    esp_websocket_client_handle_t ws = esp_websocket_client_init(&wc);
    if (!ws) {
        set_note("Could not start the connection");
        set_state(VOICE_ERROR);
        s_cfg_error = true;
        return;
    }
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);

    xSemaphoreTake(s_ws_mtx, portMAX_DELAY);
    s_ws = ws;
    xSemaphoreGive(s_ws_mtx);

    set_state(VOICE_CONNECTING);
    s_dg.attempts++;
    s_dg.attempt_us = esp_timer_get_time();
#if VOICE_DIAG
    ESP_LOGI(DTAG, "connect attempt #%d to %s (ping every %d s, network timeout %d ms, reconnect after %d ms)",
             s_dg.attempts, host, (int)wc.ping_interval_sec, wc.network_timeout_ms, wc.reconnect_timeout_ms);
#endif
    net_set_power_save(false);                /* full Wi-Fi speed while connected */
    esp_websocket_client_start(ws);
}

static void ws_stop(void)
{
    xSemaphoreTake(s_ws_mtx, portMAX_DELAY);
    esp_websocket_client_handle_t ws = s_ws;
    s_ws = NULL;
    xSemaphoreGive(s_ws_mtx);
    if (!ws) return;

    ESP_LOGI(TAG, "Closing the WebSocket");
    diag("closing (app/switch/idle)");
    if (esp_websocket_client_is_connected(ws)) {
        esp_websocket_client_close(ws, pdMS_TO_TICKS(2000));
    }
    esp_websocket_client_destroy(ws);
    s_ws_connected = false;
    s_rx_len = 0;
    net_set_power_save(true);                 /* back to saving battery */
}

/* turn supervision: recover from replies that never come, or text-only replies */
static void supervise(void)
{
    if (s_state != VOICE_THINKING || s_resending) return;   /* the resend manages its own timing */
    int64_t now = esp_timer_get_time();
    if (s_tr_audio_wait) {
        /* Translator: the text is on screen and the spoken translation was announced */
        if (now > s_tr_audio_deadline_us && uxQueueMessagesWaiting(s_play_q) == 0) {
            ESP_LOGW(TAG, "The announced spoken translation didn't arrive within %d s", TR_AUDIO_WAIT_MS / 1000);
            s_tr_audio_wait = false;
            set_note("The spoken translation didn't arrive");
            set_state(VOICE_READY);
        }
        return;
    }
    if (s_got_text && uxQueueMessagesWaiting(s_play_q) == 0 && now - s_last_rx_us > TEXT_ONLY_IDLE_US) {
        set_state(VOICE_READY);
    } else if (now - s_turn_start_us > TURN_TIMEOUT_US) {
        ESP_LOGW(TAG, "No reply within %lld s", TURN_TIMEOUT_US / 1000000);
        set_note("No reply from the server");
        set_state(VOICE_READY);
    }
}

static void ctl_task(void *arg)
{
    for (;;) {
        uint32_t cmd = 0;
        xQueueReceive(s_ctl_q, &cmd, pdMS_TO_TICKS(250));

        if (cmd == CMD_RESTART) {
            ws_stop();
            s_cfg_error = false;
        } else if (s_restart_pending && s_active && s_want && !s_recording && !s_resending &&
                   s_state != VOICE_LISTENING && s_state != VOICE_THINKING && s_state != VOICE_SPEAKING &&
                   uxQueueMessagesWaiting(s_play_q) == 0) {
            /* Translator languages changed during a turn: reconnect now that it's over */
            s_restart_pending = false;
            ESP_LOGI(TAG, "Reconnecting with the new languages");
            ws_stop();
            s_cfg_error = false;
            set_state(net_is_up() ? VOICE_CONNECTING : VOICE_WAIT_WIFI);
        }
        if (!s_active) {
            ws_stop();
            continue;
        }
        if (!s_want) {                       /* switched off, or idle timeout */
            if (s_ws) {
                ws_stop();
                ESP_LOGI(TAG, "Disconnected");
            }
            if (s_state != VOICE_DISCONNECTED && s_state != VOICE_ERROR && !s_replaying) set_state(VOICE_DISCONNECTED);
            continue;
        }
        if (!s_ws && !s_cfg_error) {
            if (!net_is_up()) {
                if (s_state != VOICE_WAIT_WIFI) set_state(VOICE_WAIT_WIFI);
                continue;
            }
            ws_start();
        }
        supervise();
        diag_heartbeat();

#if VOICE_IDLE_DISCONNECT_MS > 0
        /* close an idle connection, on any screen; tap the mic (or the switch) to reconnect.
         * Not with the wake word on: then the connection has to be ready when you say it. */
        if (s_state == VOICE_READY && !(s_mode == VOICE_MODE_CHAT && voice_wake_status() == WAKE_ON) &&
            voice_idle_us() > (int64_t)VOICE_IDLE_DISCONNECT_MS * 1000) {
            ESP_LOGI(TAG, "Idle for %d min: disconnecting", VOICE_IDLE_DISCONNECT_MS / 60000);
            s_want = false;
            set_note("Disconnected after %d min idle", VOICE_IDLE_DISCONNECT_MS / 60000);
            ws_stop();
            set_state(VOICE_DISCONNECTED);
        }
#endif
    }
}

void voice_set_mode(voice_mode_t mode)
{
    if (s_active || mode == s_mode) return;   /* only between apps */
    s_mode = mode;
    ESP_LOGI(TAG, "Mode: %s", mode == VOICE_MODE_TRANSLATE ? "Translator" : "Voice chat");
}

voice_mode_t voice_mode(void) { return s_mode; }
bool         voice_audio_pending(void) { return s_tr_audio_wait && s_state == VOICE_THINKING; }

bool voice_can_replay(void)
{
    voice_state_t st = s_state;
    return s_rep_n > 0 && !s_recording && !s_replaying &&
           (st == VOICE_READY || st == VOICE_DISCONNECTED || st == VOICE_CONNECTING ||
            st == VOICE_WAIT_WIFI || st == VOICE_ERROR);
}

bool voice_replay(void)
{
    if (!s_active || !voice_can_replay()) return false;
    xSemaphoreTake(s_rep_mtx, portMAX_DELAY);
    int queued = 0;
    s_replaying = true;
    for (int i = 0; i < s_rep_n; i++) {
        play_item_t item = {
            .data = heap_caps_malloc(s_rep[i].len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
            .len = s_rep[i].len,
        };
        if (!item.data) break;
        memcpy(item.data, s_rep[i].data, item.len);
        if (xQueueSend(s_play_q, &item, 0) != pdTRUE) { heap_caps_free(item.data); break; }
        queued++;
    }
    xSemaphoreGive(s_rep_mtx);
    if (queued == 0) {
        s_replaying = false;
        return false;
    }
    ESP_LOGI(TAG, "Replaying the last translation (%d part%s)", queued, queued == 1 ? "" : "s");
    s_last_activity_us = esp_timer_get_time();
    set_state(VOICE_SPEAKING);
    return true;
}

void voice_reconnect_when_idle(void)
{
    if (!s_active || !s_want) return;         /* not connected: the next connect uses the new pair */
    bool busy = s_recording || s_resending || s_state == VOICE_LISTENING ||
                s_state == VOICE_THINKING || s_state == VOICE_SPEAKING;
    if (busy) {
        ESP_LOGI(TAG, "Languages changed: reconnecting after this turn");
        s_restart_pending = true;
        return;
    }
    s_restart_pending = false;
    voice_reconnect();
}

void voice_set_active(bool active)
{
    if (active == s_active) return;
    s_restart_pending = false;
    s_tr_audio_wait = false;
    if (active) {
        text_clear();
        s_cfg_error = false;
        s_wake_nomem = false;                /* try the wake word again (memory may be free now) */
        s_want = true;                       /* connection switch is on by default */
        s_active = true;
        s_last_activity_us = esp_timer_get_time();
        if (!s_audio_ok) {
            set_note("Audio init failed - see the serial log");
            set_state(VOICE_ERROR);
            return;
        }
        set_state(net_is_up() ? VOICE_CONNECTING : VOICE_WAIT_WIFI);
    } else {
        s_active = false;
        s_replaying = false;
        voice_ptt_cancel();
        replay_clear();                      /* give the audio copy's memory back */
        s_state = VOICE_OFF;
    }
    uint32_t cmd = CMD_WAKE;
    xQueueSend(s_ctl_q, &cmd, 0);
    if (s_wake_task) xTaskNotifyGive(s_wake_task);
}

void voice_reconnect(void)
{
    uint32_t cmd = CMD_RESTART;
    s_want = true;
    s_last_activity_us = esp_timer_get_time();
    if (s_active) set_state(VOICE_CONNECTING);
    xQueueSend(s_ctl_q, &cmd, 0);
}

void voice_new_conversation(void)
{
    char id[SET_CONV_LEN + 1];
    settings_new_conversation(id, sizeof(id));
    text_clear();
    /* the conversation ID is part of the connection URL: reconnect if connected or connecting */
    if (s_active && s_want) voice_reconnect();
}

void voice_set_connected(bool on)
{
    if (on == s_want) return;
    ESP_LOGI(TAG, "Connection switched %s", on ? "on" : "off");
    voice_ptt_cancel();
    s_want = on;
    s_cfg_error = false;
    s_last_activity_us = esp_timer_get_time();
    set_note("%s", "");
    if (s_active) set_state(on ? (net_is_up() ? VOICE_CONNECTING : VOICE_WAIT_WIFI) : VOICE_DISCONNECTED);
    uint32_t cmd = CMD_WAKE;
    xQueueSend(s_ctl_q, &cmd, 0);
    if (s_wake_task) xTaskNotifyGive(s_wake_task);
}

bool voice_wants_connection(void) { return s_want; }

void voice_set_volume(int percent)
{
    if (!s_audio_ok) return;
    int set = 0;
    bsp_extra_codec_volume_set(percent, &set);
}

/* ------------------------------------------------------------ push-to-talk */
void voice_ptt_press(void)
{
    s_last_activity_us = esp_timer_get_time();
    if (s_active && !s_want) {               /* disconnected: a tap reconnects */
        voice_set_connected(true);
        return;
    }
    if (s_resending || s_state != VOICE_READY || !s_ws_connected || s_recording) return;
    s_rec_cancel = false;
    s_recording = true;
    if (s_wl_running) {
        /* the wake engine owns the microphone: it records from its own audio stream */
        s_hold_req = true;
    } else if (s_rec_task) {
        s_rec_busy = true;
        xTaskNotify(s_rec_task, REC_CMD_PTT, eSetValueWithOverwrite);
    } else {
        s_recording = false;
    }
}

void voice_ptt_release(void)
{
    s_recording = false;
}

void voice_ptt_cancel(void)
{
    if (s_recording) {
        s_rec_cancel = true;
        s_recording = false;
    }
}

/* ------------------------------------------------------------ recording */

/* Sends one WAV as one binary frame. True only if every byte went out: the library returns 0
 * (not -1) when the link stalls and the write times out. */
static bool send_wav(const uint8_t *wav, size_t total)
{
    int sent = -1;
    int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(s_ws_mtx, portMAX_DELAY);
    if (s_ws) sent = esp_websocket_client_send_bin(s_ws, (const char *)wav, (int)total, pdMS_TO_TICKS(15000));
    xSemaphoreGive(s_ws_mtx);
    int64_t send_ms = (esp_timer_get_time() - t0) / 1000;
    bool ok = (sent == (int)total);
    if (ok) {
        s_dg.last_tx_us = esp_timer_get_time();
        s_dg.tx_frames++;
        s_dg.tx_bytes += sent;
    }
#if VOICE_DIAG
    ESP_LOGI(DTAG, "tx WAV %u bytes: result %d in %lld ms%s", (unsigned)total, sent, (long long)send_ms,
             ok ? "" : " (FAILED)");
    if (!ok) diag("after failed send");
#else
    (void)send_ms;
#endif
    return ok;
}

/* Sends the first n samples of s_wav as one turn, resending once over a new connection if the
 * link broke mid-send. Runs in rec_task only (the send can block for up to ~35 s). */
static void send_turn(size_t n)
{
    uint32_t ms = (uint32_t)(n * 1000 / REC_RATE);
    if (!s_ws_connected) {
        set_state(VOICE_CONNECTING);
        set_note("Not connected to the server");
        return;
    }

    write_wav_header(s_wav, (uint32_t)(n * sizeof(int16_t)), REC_RATE, 1, 16);
    size_t total = WAV_HEADER_BYTES + n * sizeof(int16_t);

    s_turn_start_us = esp_timer_get_time();
    s_last_rx_us = s_turn_start_us;
    set_state(VOICE_THINKING);

    int conn_before = s_dg.connects;
    bool ok = send_wav(s_wav, total);

    if (!ok && LIVE()) {
        /* The link broke mid-send (the library reconnects by itself). Wait for a NEW connection,
         * not the stale flag of the broken one, then resend the same recording once. */
        ESP_LOGW(TAG, "Send failed; waiting up to %d s for the reconnect to resend", RESEND_WAIT_MS / 1000);
        s_resending = true;
        set_note("Connection lost - resending...");
        set_state(VOICE_THINKING);
        int64_t deadline = esp_timer_get_time() + (int64_t)RESEND_WAIT_MS * 1000;
        while (LIVE() && esp_timer_get_time() < deadline &&
               !(s_ws_connected && s_dg.connects > conn_before)) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (LIVE() && s_ws_connected && s_dg.connects > conn_before) {
            ESP_LOGI(TAG, "Reconnected: resending the recording");
            s_turn_start_us = esp_timer_get_time();
            s_last_rx_us = s_turn_start_us;
            ok = send_wav(s_wav, total);
            if (ok) set_note("%s", "");
        }
        s_resending = false;
    }

    if (ok) {
        ESP_LOGI(TAG, "Sent %u bytes (%u ms of audio)", (unsigned)total, (unsigned)ms);
    } else {
        ESP_LOGE(TAG, "Send failed");
        set_state(s_ws_connected ? VOICE_READY : VOICE_CONNECTING);
        set_note("Send failed - try again");
    }
}

/* Hold to talk without the wake word: reads the microphone while the finger is down.
 * Returns the number of samples to send, or 0 if the recording was discarded. */
static size_t record_held(int16_t *io, size_t io_bytes)
{
    set_state(VOICE_LISTENING);
    text_clear();
    s_tr_audio_wait = false;
    set_text_langs(s_conn_src, s_conn_dst);      /* Translator: the pair this turn goes out with */

    size_t got = 0;
    bsp_extra_i2s_read(io, io_bytes, &got, 200);   // drop stale buffered audio

    size_t n = 0;
    while (s_recording && n < MAX_SAMPLES) {
        got = 0;
        if (bsp_extra_i2s_read(io, io_bytes, &got, 1000) != ESP_OK || got == 0) {
            continue;
        }
        size_t frames = got / (I2S_CHANNELS * sizeof(int16_t));
        for (size_t i = 0; i < frames && n < MAX_SAMPLES; i++) {
            int32_t mixed = ((int32_t)io[2 * i] + (int32_t)io[2 * i + 1]) / 2;
            s_pcm[n++] = (int16_t)mixed;
        }
    }
    s_recording = false;
    s_rec_busy = false;
    s_last_activity_us = esp_timer_get_time();

    uint32_t ms = (uint32_t)(n * 1000 / REC_RATE);
    if (s_rec_cancel || !LIVE()) {     // the touch was a swipe, or the app closed / disconnected
        s_rec_cancel = false;
        ESP_LOGI(TAG, "Discarded %u ms of audio", (unsigned)ms);
        set_state(VOICE_READY);
        return 0;
    }
    ESP_LOGI(TAG, "Recorded %u ms", (unsigned)ms);

    if (ms < REC_MIN_MS) {
        set_state(VOICE_READY);
        set_note("Hold while you talk");
        return 0;
    }
    return n;
}

static void rec_task(void *arg)
{
    const size_t io_bytes = READ_FRAMES * I2S_CHANNELS * sizeof(int16_t);
    int16_t *io = heap_caps_malloc(io_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!io) {
        ESP_LOGE(TAG, "Out of memory for recording buffers");
        vTaskDelete(NULL);
    }

    for (;;) {
        uint32_t cmd = 0;
        xTaskNotifyWait(0, UINT32_MAX, &cmd, portMAX_DELAY);

        size_t n;
        if (cmd == REC_CMD_SEND) {
            n = s_cap_n;                       /* the wake engine filled s_pcm */
        } else if (cmd == REC_CMD_PTT) {
            n = record_held(io, io_bytes);
        } else {
            continue;
        }
        if (n > 0 && LIVE()) {
            send_turn(n);
        } else if (cmd == REC_CMD_SEND && s_state == VOICE_THINKING) {
            set_state(VOICE_READY);
        }
    }
}

/* ------------------------------------------------------------ wake word
 * One task runs ESP-SR's audio front end (AFE) with WakeNet (the wake word) and VAD (voice
 * activity detection), and while it runs it owns the microphone. It reads 32 ms of audio at a
 * time, feeds the AFE and acts on what comes out:
 *
 *   READY + wake word   chime, LISTENING; record until the VAD hears WAKE_END_SILENCE_MS of
 *                       silence after you talk (or WAKE_NO_SPEECH_MS of nothing): send
 *   touch and hold      LISTENING; record until release: send
 *   quick tap           LISTENING; record until you stop talking, as after the wake word
 *
 * The audio sent is the same as with hold to talk: both mics mixed to mono, no processing (no
 * AEC, noise suppression or AGC in the AFE), so the server hears what it always heard.
 * WakeNet runs only while the engine is READY, so a reply playing through the speaker can't wake
 * it. The AFE is created when listening should start (Voice chat screen open, connection on, the
 * setting on) and destroyed when it should stop, which gives its memory back. */

typedef enum { CAP_IDLE, CAP_HOLD, CAP_WAIT_SPEECH, CAP_SPEECH } cap_t;

static const esp_afe_sr_iface_t *s_afe = NULL;
static esp_afe_sr_data_t        *s_afe_data = NULL;
static int16_t *s_feed = NULL;          /* one feed chunk, stereo as read, mixed to mono in place */
static int      s_feed_chunk = 0;       /* samples per channel per feed */
static int      s_fetch_chunk = 0;      /* samples per fetch result */
static int16_t *s_chime = NULL;         /* stereo 16 kHz */
static size_t   s_chime_bytes = 0;

wake_status_t voice_wake_status(void)
{
    if (!s_wake_enabled) return WAKE_OFF;
    if (!s_wn_name)      return WAKE_NO_MODEL;
    if (s_wake_nomem)    return WAKE_NO_MEMORY;
    return WAKE_ON;
}

bool voice_wake_listening(void) { return s_wl_running; }

void voice_set_wake_word(bool on)
{
    s_wake_enabled = on;
    s_wake_nomem = false;
    ESP_LOGI(TAG, "Wake word %s", on ? "on" : "off");
    if (s_wake_task) xTaskNotifyGive(s_wake_task);
}

void voice_set_foreground(bool on)
{
    s_fg = on;
    if (s_wake_task) xTaskNotifyGive(s_wake_task);
}

static bool wake_should_run(void)
{
    return s_mode == VOICE_MODE_CHAT && s_wn_name && s_wake_enabled && !s_wake_nomem && s_audio_ok &&
           s_active && s_fg && s_want;
}

static bool ready_for_turn(void)
{
    return s_state == VOICE_READY && s_ws_connected && !s_resending;
}

static void make_chime(void)
{
#if WAKE_CHIME
    /* two short rising notes (E6, A6) with soft edges: "I'm listening" */
    static const float freqs[2] = { 1318.5f, 1760.0f };
    const int note = REC_RATE * 70 / 1000, gap = REC_RATE * 20 / 1000, ramp = REC_RATE * 8 / 1000;
    const int frames = 2 * note + gap;
    s_chime_bytes = (size_t)frames * 2 * sizeof(int16_t);
    s_chime = heap_caps_calloc(1, s_chime_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_chime) return;
    for (int k = 0; k < 2; k++) {
        int off = k * (note + gap);
        for (int i = 0; i < note; i++) {
            float env = 1.0f;
            if (i < ramp) env = (float)i / ramp;
            else if (i > note - ramp) env = (float)(note - i) / ramp;
            int16_t v = (int16_t)(5000.0f * env * sinf(2.0f * (float)M_PI * freqs[k] * i / REC_RATE));
            s_chime[2 * (off + i)] = v;
            s_chime[2 * (off + i) + 1] = v;
        }
    }
#endif
}

static void free_internal(size_t *free_kb, size_t *dma_kb)
{
    *free_kb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    *dma_kb  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024;
}

static void wake_stop(void)
{
    if (s_afe_data) {
        s_afe->destroy(s_afe_data);
        s_afe_data = NULL;
    }
    if (s_feed) {
        heap_caps_free(s_feed);
        s_feed = NULL;
    }
    s_wl_running = false;
    if (s_hold_req) {                          /* a touch was waiting for this engine */
        s_hold_req = false;
        s_recording = false;
    }
    size_t f, d;
    free_internal(&f, &d);
    ESP_LOGI(TAG, "Wake word stopped (free internal %u KB, largest DMA block %u KB)", (unsigned)f, (unsigned)d);
}

static bool wake_start(void)
{
    size_t free0, dma0;
    free_internal(&free0, &dma0);

    afe_config_t *cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!cfg) {
        ESP_LOGE(TAG, "afe_config_init failed");
        s_wake_nomem = true;
        return false;
    }
    cfg->aec_init = false;                 /* no playback reference channel */
    cfg->se_init = false;
    cfg->ns_init = false;
    cfg->agc_init = false;                 /* send the audio as recorded */
    cfg->vad_init = true;
    cfg->vad_mode = VAD_MODE_3;
    cfg->vad_min_speech_ms = 128;
    cfg->vad_min_noise_ms = 300;          /* short hangover: the end of a turn is timed in wake_task */
    cfg->wakenet_init = true;
    cfg->wakenet_model_name = s_wn_name;
    cfg->wakenet_model_name_2 = NULL;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_perferred_core = 1;
    cfg->afe_perferred_priority = 5;

    s_afe = esp_afe_handle_from_config(cfg);
    s_afe_data = s_afe ? s_afe->create_from_config(cfg) : NULL;
    afe_config_free(cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "Could not start the wake word engine (out of memory?)");
        s_wake_nomem = true;
        return false;
    }

    s_feed_chunk = s_afe->get_feed_chunksize(s_afe_data);
    s_fetch_chunk = s_afe->get_fetch_chunksize(s_afe_data);
    esp_log_level_set("AFE", ESP_LOG_ERROR);   /* its per-frame warnings would flood the log */
    int nch = s_afe->get_feed_channel_num(s_afe_data);
    size_t bytes = (size_t)s_feed_chunk * I2S_CHANNELS * sizeof(int16_t);
    s_feed = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_feed) s_feed = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    size_t free1, dma1;
    free_internal(&free1, &dma1);
    ESP_LOGI(TAG, "Wake word \"%s\" (%s) listening: %d samples per feed, %d per fetch, %d ch; it took %d KB internal RAM, "
             "%u KB left (largest DMA block %u KB)", WAKE_WORD_LABEL, s_wn_name, s_feed_chunk, s_fetch_chunk, nch,
             (int)free0 - (int)free1, (unsigned)free1, (unsigned)dma1);

    if (!s_feed || nch != 1 || s_fetch_chunk <= 0 || free1 < WAKE_MIN_FREE_INTERNAL_KB || dma1 < WAKE_MIN_DMA_BLOCK_KB) {
        ESP_LOGE(TAG, "Wake word off: %s (needs %d KB internal free and a %d KB DMA block; "
                 "see WAKE_MIN_* in app_config.h)", !s_feed ? "no feed buffer" : nch != 1 ? "unexpected channel count" :
                 "not enough internal RAM left", WAKE_MIN_FREE_INTERNAL_KB, WAKE_MIN_DMA_BLOCK_KB);
        wake_stop();
        s_wake_nomem = true;
        set_note("Wake word off: low memory");
        return false;
    }
    return true;
}

/* Level of a block of samples in dBFS (0 = full scale, about -90 = digital silence) */
static float level_db(const int16_t *x, int n)
{
    if (n <= 0) return -96.0f;
    double sum = 0;
    for (int i = 0; i < n; i++) sum += (double)x[i] * x[i];
    double rms = sqrt(sum / n);
    return rms < 1.0 ? -96.0f : (float)(20.0 * log10(rms / 32768.0));
}

/* The engine's main loop. Owns s_afe / s_afe_data; nothing else touches them. */
static void wake_task(void *arg)
{
    cap_t   cap = CAP_IDLE;
    size_t  n = 0;
    int64_t start_us = 0, ignore_until_us = 0, armed_us = 0;
    bool    saw_silence = false, wn_on = true;
    int     pending = 0;                         /* samples fed but not fetched yet */
    float   floor_db = -60.0f;                   /* the room's noise level, learned while idle */
    int64_t voice_us = 0, last_voice_us = 0, dbg_us = 0;   /* voiced time in this turn, last voiced frame */

    for (;;) {
        if (!wake_should_run()) {
            if (s_afe_data) {
                if (cap != CAP_IDLE) {
                    ESP_LOGI(TAG, "Discarded the recording (left Voice chat)");
                    cap = CAP_IDLE;
                    s_recording = false;
                    if (s_state == VOICE_LISTENING) set_state(VOICE_READY);
                }
                wake_stop();
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(300));
            continue;
        }
        if (!s_afe_data) {
            /* Start once the screen has slid in and the server connection is up: the slide and the
             * TLS handshake need internal RAM too, and together with the AFE's startup that was
             * enough to make the display drop frames. Also wait for hold to talk to finish. */
            if (s_rec_busy || !s_ws_connected || s_state != VOICE_READY) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
                continue;
            }
            s_wl_running = true;                 /* touches go to this engine from now on */
            if (!wake_start()) {
                s_wl_running = false;
                if (s_hold_req) {
                    s_hold_req = false;
                    s_recording = false;
                }
                continue;
            }
            cap = CAP_IDLE;
            pending = 0;
            wn_on = true;                        /* WakeNet starts enabled */
            armed_us = esp_timer_get_time() + (int64_t)WAKE_REARM_MS * 1000;
        }

        /* 1. read one chunk (both mics), mix to mono in place, feed the AFE */
        size_t want = (size_t)s_feed_chunk * I2S_CHANNELS * sizeof(int16_t), got = 0;
        if (bsp_extra_i2s_read(s_feed, want, &got, 500) != ESP_OK || got < want) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        for (int i = 0; i < s_feed_chunk; i++) {
            s_feed[i] = (int16_t)(((int32_t)s_feed[2 * i] + (int32_t)s_feed[2 * i + 1]) / 2);
        }
        s_afe->feed(s_afe_data, s_feed);
        pending += s_feed_chunk;
        if (pending > 8 * s_fetch_chunk) pending = 8 * s_fetch_chunk;

        /* 2. handle what the AFE has ready (only fetch what was fed: an empty fetch logs a warning) */
        while (pending >= s_fetch_chunk) {
            afe_fetch_result_t *res = s_afe->fetch_with_delay(s_afe_data, pdMS_TO_TICKS(20));
            if (!res || res->ret_value != ESP_OK) break;   /* not ready yet: next time */
            pending -= s_fetch_chunk;
            int64_t now = esp_timer_get_time();

            /* Is this frame voice? ESP-SR's VAD, or clearly louder than the room (a backup that
             * doesn't depend on the VAD's tuning). The noise floor follows quiet frames while idle. */
            int    frame_n = res->data_size / (int)sizeof(int16_t);
            float  db = level_db(res->data, frame_n);
            if (cap == CAP_IDLE && ready_for_turn()) {   /* not while a reply plays */
                floor_db += (db < floor_db ? 0.2f : 0.02f) * (db - floor_db);
                if (floor_db < -75.0f) floor_db = -75.0f;
            }
            float  thr = floor_db + WAKE_LEVEL_ABOVE_DB;
            if (thr < WAKE_LEVEL_MIN_DB) thr = WAKE_LEVEL_MIN_DB;
            bool   voice = (res->vad_state == VAD_SPEECH) || db > thr;
#if WAKE_DEBUG
            if (cap != CAP_IDLE && now - dbg_us > 250 * 1000) {
                dbg_us = now;
                ESP_LOGI(TAG, "listen: vad=%d level=%.0f dB (voice above %.0f) -> %s", (int)res->vad_state, db, thr,
                         voice ? "voice" : "quiet");
            }
#else
            (void)dbg_us;
#endif

            /* WakeNet only while waiting for a turn (never during a reply) */
            bool want_wn = (cap == CAP_IDLE) && ready_for_turn();
            if (want_wn != wn_on) {
                if (want_wn) {
                    s_afe->enable_wakenet(s_afe_data);
                    armed_us = now + (int64_t)WAKE_REARM_MS * 1000;
                } else {
                    s_afe->disable_wakenet(s_afe_data);
                }
                wn_on = want_wn;
            }

            /* a swipe, the connection switch or the app closing throws the recording away */
            if (cap != CAP_IDLE && (s_rec_cancel || !LIVE())) {
                s_rec_cancel = false;
                s_recording = false;
                ESP_LOGI(TAG, "Discarded %u ms of audio", (unsigned)(n * 1000 / REC_RATE));
                cap = CAP_IDLE;
                if (s_state == VOICE_LISTENING) set_state(VOICE_READY);
                continue;
            }

            if (cap == CAP_IDLE) {
                if (s_hold_req) {
                    s_hold_req = false;
                    if (ready_for_turn() && s_recording) {
                        cap = CAP_HOLD;
                        n = 0;
                        start_us = now;
                        text_clear();
                        set_state(VOICE_LISTENING);
                    } else {
                        s_recording = false;
                    }
                } else if (wn_on && res->wakeup_state == WAKENET_DETECTED && now >= armed_us && ready_for_turn()) {
                    ESP_LOGI(TAG, "Wake word heard (room noise %.0f dBFS)", floor_db);
                    cap = CAP_WAIT_SPEECH;
                    n = 0;
                    start_us = now;
                    saw_silence = false;
                    voice_us = 0;
                    text_clear();
                    set_state(VOICE_LISTENING);
                    s_last_activity_us = now;
                    s_afe->disable_wakenet(s_afe_data);
                    wn_on = false;
                    if (s_chime) {
                        size_t w = 0;
                        bsp_extra_i2s_write(s_chime, s_chime_bytes, &w, 500);
                    }
                    ignore_until_us = esp_timer_get_time() + (int64_t)WAKE_CHIME_IGNORE_MS * 1000;
                    continue;        /* this chunk is the wake word itself */
                }
                if (cap == CAP_IDLE) continue;
            }

            /* append this chunk to the recording */
            int samples = res->data_size / (int)sizeof(int16_t);
            if (samples > 0 && n < MAX_SAMPLES) {
                size_t take = (size_t)samples;
                if (take > MAX_SAMPLES - n) take = MAX_SAMPLES - n;
                memcpy(s_pcm + n, res->data, take * sizeof(int16_t));
                n += take;
            }

            bool finish = false;
            switch (cap) {
            case CAP_HOLD:
                if (!s_recording) {                       /* released */
                    if (now - start_us < (int64_t)WAKE_HOLD_TAP_MS * 1000) {
                        /* a quick tap: listen hands-free, as after the wake word */
                        cap = CAP_WAIT_SPEECH;
                        start_us = now;
                        saw_silence = true;
                        voice_us = 0;
                        ignore_until_us = 0;
                    } else {
                        finish = true;
                    }
                } else if (n >= MAX_SAMPLES) {
                    s_recording = false;
                    finish = true;
                }
                break;

            case CAP_WAIT_SPEECH:
                if (now < ignore_until_us) break;         /* the mic is hearing the chime */
                if (!voice) {
                    saw_silence = true;
                } else {
                    /* you started talking. After a pause, keep just a little audio from before it */
                    size_t keep = (size_t)WAKE_PREROLL_MS * REC_RATE / 1000;
                    if (saw_silence && n > keep) {
                        memmove(s_pcm, s_pcm + (n - keep), keep * sizeof(int16_t));
                        n = keep;
                    }
                    cap = CAP_SPEECH;
                    last_voice_us = now;
                    break;
                }
                if (now - start_us > (int64_t)WAKE_NO_SPEECH_MS * 1000 || n >= MAX_SAMPLES) {
                    ESP_LOGI(TAG, "No request after the wake word");
                    cap = CAP_IDLE;
                    set_note("Didn't catch that");
                    set_state(VOICE_READY);
                }
                break;

            case CAP_SPEECH:
                if (voice) {
                    voice_us += (int64_t)frame_n * 1000000 / REC_RATE;
                    last_voice_us = now;
                } else if (now - last_voice_us > (int64_t)WAKE_END_SILENCE_MS * 1000) {
                    if (voice_us < (int64_t)WAKE_MIN_SPEECH_MS * 1000) {
                        /* only a blip (the tail of the wake word, a click): keep waiting */
                        cap = CAP_WAIT_SPEECH;
                        saw_silence = true;
                        voice_us = 0;
                        break;
                    }
                    finish = true;                        /* you stopped talking */
                }
                if (n >= MAX_SAMPLES) finish = true;
                break;

            default:
                break;
            }

            if (!finish) continue;

            /* hand the recording to rec_task, which sends it (sending can block for seconds) */
            cap = CAP_IDLE;
            s_recording = false;
            s_last_activity_us = now;
            uint32_t ms = (uint32_t)(n * 1000 / REC_RATE);
            ESP_LOGI(TAG, "Recorded %u ms (voice %u ms)", (unsigned)ms, (unsigned)(voice_us / 1000));
            if (ms < REC_MIN_MS) {
                set_state(VOICE_READY);
                continue;
            }
            s_cap_n = n;
            s_turn_start_us = now;               /* the turn timer starts now, not when the send does */
            s_last_rx_us = now;
            set_state(VOICE_THINKING);
            xTaskNotify(s_rec_task, REC_CMD_SEND, eSetValueWithOverwrite);
        }
    }
}

/* ------------------------------------------------------------- playback */
typedef enum { AUD_WAV, AUD_MP3, AUD_OGG, AUD_RAW } audio_kind_t;

static audio_kind_t classify_audio(const uint8_t *d, size_t len)
{
    if (len >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WAVE", 4) == 0) return AUD_WAV;
    if (len >= 3 && memcmp(d, "ID3", 3) == 0) return AUD_MP3;
    if (len >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0) return AUD_MP3;   // MPEG frame sync
    if (len >= 4 && memcmp(d, "OggS", 4) == 0) return AUD_OGG;
    return AUD_RAW;
}

static const char *kind_name(audio_kind_t k)
{
    switch (k) {
    case AUD_WAV: return "WAV";
    case AUD_MP3: return "MP3";
    case AUD_OGG: return "OGG (Opus/Vorbis)";
    default:      return "raw/unknown";
    }
}

static inline int16_t mono_at(const uint8_t *pcm, size_t i, int channels, size_t frame_bytes)
{
    int16_t l, r;
    memcpy(&l, pcm + i * frame_bytes, 2);
    if (channels < 2) return l;
    memcpy(&r, pcm + i * frame_bytes + 2, 2);
    return (int16_t)(((int32_t)l + r) / 2);
}

/* All playback goes out at REC_RATE (16 kHz), the rate the codec is set to at boot.
 * Replies at other rates are converted here with linear interpolation instead of re-clocking the
 * codec. Re-clocking tears down and rebuilds the I2S driver, which briefly takes internal DMA
 * memory from the display ("Draw bitmap failed"). For speech the difference is inaudible. */
typedef struct {
    double  step;      // input samples per output sample
    double  pos;       // read position in [prev, x0, x1, ...]
    int16_t prev;      // last input sample of the previous chunk
    size_t  out_n;     // stereo frames waiting in the output buffer
} resampler_t;

static resampler_t s_rs;

static void resampler_reset(void)
{
    s_rs.step = 1.0;
    s_rs.pos = 1.0;
    s_rs.prev = 0;
    s_rs.out_n = 0;
}

static void out_flush(int16_t *out)
{
    if (s_rs.out_n == 0) return;
    size_t written = 0;
    bsp_extra_i2s_write(out, s_rs.out_n * 2 * sizeof(int16_t), &written, 1000);
    s_rs.out_n = 0;
}

/* Plays interleaved 16-bit samples (1 or 2 channels) at in_rate through the 16 kHz codec. */
static void play_pcm16(const uint8_t *pcm, size_t frames, int channels, uint32_t in_rate, int16_t *out)
{
    if (frames == 0 || channels <= 0 || in_rate == 0) return;
    s_rs.step = (double)in_rate / (double)REC_RATE;

    const size_t frame_bytes = (size_t)channels * 2;
    /* mono sample i of this chunk (stereo is averaged); index 0 is the previous chunk's last */
    #define SAMPLE_AT(i) ((i) == 0 ? s_rs.prev : mono_at(pcm, (i) - 1, channels, frame_bytes))

    while (s_rs.pos + 1.0 <= (double)frames) {
        if (!PLAY_OK()) return;                        /* app closed or disconnected: stop talking */
        size_t i = (size_t)s_rs.pos;
        double f = s_rs.pos - (double)i;
        int32_t a = SAMPLE_AT(i);
        int32_t b2 = SAMPLE_AT(i + 1);
        int16_t y = (int16_t)(a + (int32_t)((b2 - a) * f));

        out[2 * s_rs.out_n] = y;
        out[2 * s_rs.out_n + 1] = y;
        if (++s_rs.out_n == PLAY_FRAMES) out_flush(out);
        s_rs.pos += s_rs.step;
    }
    s_rs.pos -= (double)frames;
    s_rs.prev = mono_at(pcm, frames - 1, channels, frame_bytes);
    #undef SAMPLE_AT
}

/* MP3 decoder state lives across queue items, so a stream split over several
 * WebSocket messages decodes as one. Reset at the end of each reply.
 * Decoder state and frame buffer live in PSRAM: internal RAM is reserved for the display's
 * DMA transfers (running out of it shows up as "Draw bitmap failed: ESP_ERR_NO_MEM"). */
static mp3dec_t *s_mp3 = NULL;
static bool     s_mp3_active = false;
static uint8_t *s_mp3_carry = NULL;          // incomplete frame left from the previous message
static size_t   s_mp3_carry_len = 0;
static int16_t *s_mp3_pcm = NULL;
#define MP3_CARRY_MAX 8192

static void play_task(void *arg)
{
    int16_t *out = heap_caps_malloc(PLAY_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_mp3_carry = heap_caps_malloc(MP3_CARRY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_mp3 = heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_mp3_pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out || !s_mp3_carry || !s_mp3 || !s_mp3_pcm) {
        ESP_LOGE(TAG, "Out of memory for playback buffers");
        vTaskDelete(NULL);
    }
    resampler_reset();
    uint32_t fmt_rate = REC_RATE;            // format of the current raw/WAV reply stream
    uint16_t fmt_ch = 1, fmt_bits = 16;

    for (;;) {
        play_item_t item;
        xQueueReceive(s_play_q, &item, portMAX_DELAY);

        if (!PLAY_OK()) {                    /* app closed or disconnected: drop the rest of the reply */
            s_replaying = false;
            heap_caps_free(item.data);
            s_mp3_active = false;
            s_mp3_carry_len = 0;
            resampler_reset();
            continue;
        }

        audio_kind_t kind = classify_audio(item.data, item.len);
        if (kind == AUD_RAW && s_mp3_active) kind = AUD_MP3;   // continuation of an MP3 stream

        const uint8_t *b = item.data;
        ESP_LOGI(TAG, "Reply audio: %s, %u bytes, first bytes %02X %02X %02X %02X %02X %02X %02X %02X",
                 kind_name(kind), (unsigned)item.len,
                 item.len > 0 ? b[0] : 0, item.len > 1 ? b[1] : 0, item.len > 2 ? b[2] : 0, item.len > 3 ? b[3] : 0,
                 item.len > 4 ? b[4] : 0, item.len > 5 ? b[5] : 0, item.len > 6 ? b[6] : 0, item.len > 7 ? b[7] : 0);

        if (kind == AUD_OGG) {
            ESP_LOGW(TAG, "OGG/Opus reply audio isn't supported yet; ask the server for MP3 or WAV");
            heap_caps_free(item.data);
            continue;
        }

        set_state(VOICE_SPEAKING);

        if (kind == AUD_MP3) {
            /* prepend any partial frame left over from the previous message */
            const uint8_t *in = item.data;
            size_t in_len = item.len;
            uint8_t *joined = NULL;
            if (s_mp3_active && s_mp3_carry_len > 0) {
                joined = heap_caps_malloc(s_mp3_carry_len + item.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (joined) {
                    memcpy(joined, s_mp3_carry, s_mp3_carry_len);
                    memcpy(joined + s_mp3_carry_len, item.data, item.len);
                    in = joined;
                    in_len = s_mp3_carry_len + item.len;
                }
            }
            s_mp3_carry_len = 0;
            if (!s_mp3_active) {
                mp3dec_init(s_mp3);
                s_mp3_active = true;
            }

            /* If more of this reply is still arriving, keep the tail for later: minimp3 would
             * skip a frame that is cut off at the end of the buffer. */
            vTaskDelay(pdMS_TO_TICKS(150));
            bool final_chunk = (uxQueueMessagesWaiting(s_play_q) == 0);

            size_t off = 0;
            int frames_decoded = 0;
            while (off < in_len && PLAY_OK()) {
                if (!final_chunk && in_len - off < 4096) break;   // wait for the next message
                mp3dec_frame_info_t info;
                int samples = mp3dec_decode_frame(s_mp3, in + off, (int)(in_len - off), s_mp3_pcm, &info);
                if (info.frame_bytes == 0) break;            // need more data (or nothing usable)
                off += info.frame_bytes;
                if (samples <= 0) continue;                  // skipped ID3 tag / junk
                if (frames_decoded++ == 0) {
                    ESP_LOGI(TAG, "MP3: %d Hz, %d ch, %d kbps", info.hz, info.channels, info.bitrate_kbps);
                }
                play_pcm16((const uint8_t *)s_mp3_pcm, (size_t)samples, info.channels, (uint32_t)info.hz, out);
            }
            size_t left = in_len - off;
            if (left > 0 && left <= MP3_CARRY_MAX && s_mp3_carry) {
                memcpy(s_mp3_carry, in + off, left);
                s_mp3_carry_len = left;
            }
            if (joined) heap_caps_free(joined);
        } else {
            const uint8_t *pcm = item.data;
            size_t pcm_len = item.len;
            if (kind == AUD_WAV) {
                uint32_t r; uint16_t c, bits;
                if (!parse_wav(item.data, item.len, &pcm, &pcm_len, &r, &c, &bits)) {
                    ESP_LOGW(TAG, "Unreadable WAV header; skipping");
                    heap_caps_free(item.data);
                    continue;
                }
                fmt_rate = r; fmt_ch = c; fmt_bits = bits;
                ESP_LOGI(TAG, "WAV: %u Hz, %u ch, %u bit", (unsigned)r, c, bits);
            }
            /* AUD_RAW: raw PCM continuing the last known format */
            if (fmt_bits != 16 || fmt_ch == 0) {
                ESP_LOGW(TAG, "Unsupported audio (%u ch, %u bit); only 16-bit PCM plays", fmt_ch, fmt_bits);
                heap_caps_free(item.data);
                continue;
            }
            play_pcm16(pcm, pcm_len / ((size_t)fmt_ch * 2), fmt_ch, fmt_rate, out);
        }

        heap_caps_free(item.data);

        vTaskDelay(pdMS_TO_TICKS(150));
        if (uxQueueMessagesWaiting(s_play_q) == 0) {
            /* end of the reply: flush, add a little silence, reset decoder + resampler */
            out_flush(out);
            memset(out, 0, PLAY_FRAMES * 2 * sizeof(int16_t));
            size_t written = 0;
            bsp_extra_i2s_write(out, PLAY_FRAMES * 2 * sizeof(int16_t), &written, 1000);
            resampler_reset();

            s_mp3_active = false;
            s_mp3_carry_len = 0;
            fmt_rate = REC_RATE; fmt_ch = 1; fmt_bits = 16;
            s_replaying = false;
            s_last_activity_us = esp_timer_get_time();
            if (!s_want) set_state(VOICE_DISCONNECTED);          /* a replay while disconnected */
            else set_state(s_ws_connected ? VOICE_READY : VOICE_CONNECTING);
        }
    }
}

/* ----------------------------------------------------------------- init */
esp_err_t voice_init(void)
{
    s_txt_mtx = xSemaphoreCreateMutex();
    s_ws_mtx = xSemaphoreCreateMutex();
    s_rep_mtx = xSemaphoreCreateMutex();
#if VOICE_DIAG_VERBOSE
    esp_log_level_set("websocket_client", ESP_LOG_DEBUG);
    esp_log_level_set("transport_ws", ESP_LOG_DEBUG);
    esp_log_level_set("transport_base", ESP_LOG_DEBUG);
    esp_log_level_set("esp-tls", ESP_LOG_DEBUG);
#endif
    s_ctl_q = xQueueCreate(8, sizeof(uint32_t));
    s_play_q = xQueueCreate(16, sizeof(play_item_t));

    if (bsp_extra_codec_init() != ESP_OK) {
        ESP_LOGE(TAG, "Audio codec init failed");
        return ESP_FAIL;
    }

    s_wav = heap_caps_malloc(WAV_HEADER_BYTES + MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_wav) {
        ESP_LOGE(TAG, "Out of memory for the recording buffer");
        return ESP_FAIL;
    }
    s_pcm = (int16_t *)(s_wav + WAV_HEADER_BYTES);

    /* Wake word model: idf.py flash writes the models chosen in sdkconfig to the "model" partition */
    app_settings_t cfg;
    settings_get(&cfg);
    s_wake_enabled = cfg.wake_word;
    s_models = esp_srmodel_init(MODEL_PARTITION);
    if (s_models) s_wn_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, WAKE_WORD_MODEL);
    if (s_wn_name) {
        ESP_LOGI(TAG, "Wake word model %s (\"%s\"), %s", s_wn_name, WAKE_WORD_LABEL, s_wake_enabled ? "on" : "off in Settings");
    } else {
        ESP_LOGW(TAG, "No \"%s\" wake word model in the \"%s\" partition: hold to talk only. "
                 "Check CONFIG_SR_WN_* in sdkconfig and run idf.py flash (not just app-flash)",
                 WAKE_WORD_MODEL, MODEL_PARTITION);
    }
    make_chime();

    /* Task stacks go in PSRAM where they can: internal RAM is what the display (and the wake word)
     * run short of. These tasks never write to flash, which is what a PSRAM stack can't do. */
    if (xTaskCreatePinnedToCoreWithCaps(rec_task, "rec", 6 * 1024, NULL, 6, &s_rec_task, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        xTaskCreatePinnedToCore(rec_task, "rec", 6 * 1024, NULL, 6, &s_rec_task, 1);
    }
    /* the MP3 decoder needs ~16 KB of stack scratch: put this task's stack in PSRAM */
    if (xTaskCreatePinnedToCoreWithCaps(play_task, "play", 32 * 1024, NULL, 6, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the playback task");
        return ESP_FAIL;
    }
    if (xTaskCreatePinnedToCoreWithCaps(ctl_task, "voice_ctl", 6 * 1024, NULL, 5, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        xTaskCreatePinnedToCore(ctl_task, "voice_ctl", 6 * 1024, NULL, 5, NULL, 0);
    }
    /* the wake word engine; its stack is in PSRAM to save internal RAM for the display */
    if (s_wn_name &&
        xTaskCreatePinnedToCoreWithCaps(wake_task, "wake", 16 * 1024, NULL, 5, &s_wake_task, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the wake word task: hold to talk only");
        s_wn_name = NULL;
    }
    s_audio_ok = true;
    return ESP_OK;
}
