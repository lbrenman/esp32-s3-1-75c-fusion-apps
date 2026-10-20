/*
 * voice_core.c — push-to-talk engine for the voice-chat app (ported from voice_chat/main.c)
 *
 * While the app is open: hold to record 16 kHz mono, release to send ONE binary WebSocket frame
 * (a WAV file) to the voice server. The server's JSON text frames (TRANSCRIBED_REQUEST / RESPONSE /
 * ERROR) become on-screen text, and its binary reply audio (MP3, WAV or raw 16-bit PCM, detected
 * automatically) plays through the speaker. MP3 is decoded on the device with minimp3 (CC0).
 *
 * The WebSocket is opened when the app opens and closed when it closes. Server URL and API key come
 * from the settings store (first boot: secrets.h), so they can be changed on the device.
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

#include "voice.h"
#include "net.h"
#include "settings_store.h"

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
#define HEARD_MAX_CHARS     200
#define REPLY_MAX_CHARS     320             // what fits on the round screen
#define NOTE_MAX_CHARS      96

/* ------------------------------------------------------------------- state */
typedef struct {
    uint8_t *data;
    size_t   len;
} play_item_t;

enum { CMD_WAKE = 1, CMD_RESTART = 2 };

static volatile voice_state_t s_state = VOICE_OFF;
static volatile bool    s_active = false;           // the app is open
static volatile bool    s_recording = false;
static volatile bool    s_rec_cancel = false;       // set by a swipe: discard the recording
static volatile bool    s_ws_connected = false;
static volatile bool    s_got_text = false;
static volatile bool    s_cfg_error = false;        // no usable URL: don't retry until it changes
static bool             s_audio_ok = false;         // codec + tasks started
static volatile int64_t s_last_activity_us = 0;
static volatile int64_t s_turn_start_us = 0;
static volatile int64_t s_last_rx_us = 0;

static TaskHandle_t  s_rec_task = NULL;
static QueueHandle_t s_play_q = NULL;
static QueueHandle_t s_ctl_q = NULL;
static SemaphoreHandle_t s_ws_mtx = NULL;           // guards s_ws between send and stop
static SemaphoreHandle_t s_txt_mtx = NULL;
static esp_websocket_client_handle_t s_ws = NULL;

static char s_url[512];

/* on-screen text (guarded by s_txt_mtx) */
static char s_heard[HEARD_MAX_CHARS + 4];
static char s_reply[REPLY_MAX_CHARS + 8];
static char s_note[NOTE_MAX_CHARS + 1];
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

static void text_clear(void)
{
    text_lock();
    s_heard[0] = s_reply[0] = s_note[0] = '\0';
    s_got_text = false;
    text_unlock();
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

static void set_heard(const char *text)
{
    text_lock();
    snprintf(s_heard, sizeof(s_heard), "\"%s\"", text);
    text_unlock();
}

static void reply_append(const char *text)
{
    text_lock();
    size_t cur = strlen(s_reply);
    if (cur > 0 && cur < REPLY_MAX_CHARS) {
        strncat(s_reply, "\n", REPLY_MAX_CHARS - cur);
        cur++;
    }
    if (cur < REPLY_MAX_CHARS) {
        strncat(s_reply, text, REPLY_MAX_CHARS - cur);
    }
    if (strlen(text) + cur > REPLY_MAX_CHARS) {
        strcpy(s_reply + REPLY_MAX_CHARS - 3, "...");
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
    case VOICE_READY:      return "Hold to talk";
    case VOICE_LISTENING:  return "Listening...";
    case VOICE_THINKING:   return "Thinking...";
    case VOICE_SPEAKING:   return "Speaking...";
    case VOICE_ERROR:      return "Not connected";
    }
    return "";
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

static void make_uuid(char *out /* 37 bytes */)
{
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (b[6] & 0x0F) | 0x40;   // version 4
    b[8] = (b[8] & 0x3F) | 0x80;   // variant
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
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

/* ---------------------------------------------------------- display text
 * The built-in Montserrat fonts only contain ASCII, so "smart" punctuation and accented
 * letters from the LLM show up as empty boxes. Convert them to ASCII look-alikes for display
 * (the spoken reply is unaffected). Emoji are dropped; anything else unknown becomes '?'. */
static const char *const s_latin1_hi[64] = {   /* U+00C0 .. U+00FF */
    "A","A","A","A","A","A","AE","C","E","E","E","E","I","I","I","I",
    "D","N","O","O","O","O","O","x","O","U","U","U","U","Y","Th","ss",
    "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
    "d","n","o","o","o","o","o","/","o","u","u","u","u","y","th","y",
};

static const char *ascii_for(uint32_t cp)
{
    if (cp >= 0xC0 && cp <= 0xFF) return s_latin1_hi[cp - 0xC0];
    switch (cp) {
    case 0x00A0: case 0x2002: case 0x2003: case 0x2007: case 0x2009: case 0x200A: case 0x202F:
        return " ";
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: case 0x00B4: case 0x0060:
        return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: case 0x00AB: case 0x00BB:
        return "\"";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
        return "-";
    case 0x2026: return "...";
    case 0x2022: case 0x00B7: case 0x25CF: return "-";
    case 0x00B0: return " deg";
    case 0x00A9: return "(c)";
    case 0x00AE: return "(R)";
    case 0x2122: return "(TM)";
    case 0x00B1: return "+/-";
    case 0x00A3: return "GBP";
    case 0x20AC: return "EUR";
    case 0x00A5: return "JPY";
    case 0x00BF: return "?";
    case 0x00A1: return "!";
    case 0x2192: return "->";
    case 0x2190: return "<-";
    case 0x200B: case 0x200C: case 0x200D: case 0xFE0F: case 0xFEFF: return "";   /* invisible */
    default: break;
    }
    if (cp >= 0x1F000) return "";   /* emoji and pictographs */
    return "?";
}

static void to_display_ascii(const char *in, char *out, size_t out_len)
{
    const unsigned char *p = (const unsigned char *)in;
    size_t o = 0;
    while (*p && o + 1 < out_len) {
        uint32_t cp;
        int n;
        if (p[0] < 0x80)                                  { cp = p[0]; n = 1; }
        else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu); n = 2;
        } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu); n = 3;
        } else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
                   (p[3] & 0xC0) == 0x80) {
            cp = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
            n = 4;
        } else { cp = '?'; n = 1; }                       /* invalid byte */
        p += n;

        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else if (cp == 0x2014 || cp == 0x2015) {
            /* em dash: "word—word" reads better as "word - word" (no doubled spaces) */
            if (o > 0 && out[o - 1] != ' ' && o + 1 < out_len) out[o++] = ' ';
            if (o + 1 < out_len) out[o++] = '-';
            if (*p && *p != ' ' && o + 1 < out_len) out[o++] = ' ';
        } else {
            for (const char *r = ascii_for(cp); *r && o + 1 < out_len; r++) out[o++] = *r;
        }
    }
    out[o] = '\0';
}

/* -------------------------------------------------------- server messages */
static void handle_event_obj(cJSON *obj)
{
    static char m[1024];   /* display-safe copy of "message" (called from the WebSocket task only) */
    cJSON *type = cJSON_GetObjectItem(obj, "type");
    cJSON *msg  = cJSON_GetObjectItem(obj, "message");
    to_display_ascii(cJSON_IsString(msg) ? msg->valuestring : "", m, sizeof(m));

    if (!cJSON_IsString(type)) {
        /* object without a type: show it compactly */
        char *s = cJSON_PrintUnformatted(obj);
        if (s) { to_display_ascii(s, m, sizeof(m)); reply_append(m); cJSON_free(s); }
        s_got_text = true;
        return;
    }

    if (strcmp(type->valuestring, "TRANSCRIBED_REQUEST") == 0) {
        ESP_LOGI(TAG, "Heard: %s", m);
        set_heard(m);
    } else if (strcmp(type->valuestring, "RESPONSE") == 0) {
        ESP_LOGI(TAG, "Reply: %s", m);
        reply_append(m);
        s_got_text = true;
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
        to_display_ascii(text, t, sizeof(t));
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
        ESP_LOGW(TAG, "Server closed the connection, code %d", code);
        if (code == 1006) set_note("Server refused the connection - check the API key");
        else if (code == 1009) set_note("Recording too large for the server");
        else if (code == 1011) set_note("Server error (1011)");
        return;
    }
    if (d->op_code == 0x09 || d->op_code == 0x0A) return;   // ping/pong

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

    if (s_rx_op == 0x01) {
        if (!rx_reserve(s_rx_len + 1)) { s_rx_len = 0; return; }   // also covers empty frames
        s_rx_buf[s_rx_len] = '\0';
        handle_text((const char *)s_rx_buf);
    } else if (s_rx_op == 0x02 && s_rx_len > 0 && s_active) {
        play_item_t item = {
            .data = heap_caps_malloc(s_rx_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
            .len = s_rx_len,
        };
        if (item.data) {
            memcpy(item.data, s_rx_buf, s_rx_len);
            ESP_LOGI(TAG, "Received %u bytes of reply audio", (unsigned)s_rx_len);
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
        if (s_state == VOICE_CONNECTING || s_state == VOICE_ERROR || s_state == VOICE_WAIT_WIFI) {
            set_note("%s", "");
            set_state(VOICE_READY);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected (will retry)");
        s_ws_connected = false;
        if (s_state != VOICE_SPEAKING) set_state(VOICE_CONNECTING);
        break;
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "WebSocket closed");
        s_ws_connected = false;
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error (check URL, API key, TLS)");
        if (!s_ws_connected) set_note("Can't reach the server - check the URL");
        break;
    case WEBSOCKET_EVENT_DATA:
        handle_ws_data(d);
        break;
    default:
        break;
    }
}

/* ----------------------------------------------------- WebSocket lifecycle
 * Runs only in the control task, so starting/stopping never races and never
 * blocks the LVGL task. */
static void ws_start(void)
{
    app_settings_t cfg;
    settings_get(&cfg);

    if (strncmp(cfg.ws_url, "wss://", 6) != 0 && strncmp(cfg.ws_url, "ws://", 5) != 0) {
        ESP_LOGE(TAG, "Server URL must start with wss:// (Voice > swipe up > Server URL)");
        set_note("Set the server URL: swipe up");
        s_cfg_error = true;
        set_state(VOICE_ERROR);
        return;
    }

    char key_enc[3 * SET_KEY_MAX + 1], conv[37];
    url_encode(cfg.ws_api_key, key_enc, sizeof(key_enc));
    make_uuid(conv);

    const char *sep = strchr(cfg.ws_url, '?') ? "&" : "?";
    snprintf(s_url, sizeof(s_url), "%s%sx-api-key=%s&conversationId=%s", cfg.ws_url, sep, key_enc, conv);
    /* never log the key */
    ESP_LOGI(TAG, "Connecting to %s (conversationId=%s)", cfg.ws_url, conv);

    char host[128];
    url_host(cfg.ws_url, host, sizeof(host));

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
    if (esp_websocket_client_is_connected(ws)) {
        esp_websocket_client_close(ws, pdMS_TO_TICKS(2000));
    }
    esp_websocket_client_destroy(ws);
    s_ws_connected = false;
    s_rx_len = 0;
}

/* turn supervision: recover from replies that never come, or text-only replies */
static void supervise(void)
{
    if (s_state != VOICE_THINKING) return;
    int64_t now = esp_timer_get_time();
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
        }
        if (!s_active) {
            ws_stop();
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
    }
}

void voice_set_active(bool active)
{
    if (active == s_active) return;
    if (active) {
        text_clear();
        s_cfg_error = false;
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
        voice_ptt_cancel();
        s_state = VOICE_OFF;
    }
    uint32_t cmd = CMD_WAKE;
    xQueueSend(s_ctl_q, &cmd, 0);
}

void voice_reconnect(void)
{
    uint32_t cmd = CMD_RESTART;
    if (s_active) set_state(VOICE_CONNECTING);
    xQueueSend(s_ctl_q, &cmd, 0);
}

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
    if (s_rec_task && s_state == VOICE_READY && s_ws_connected && !s_recording) {
        s_rec_cancel = false;
        s_recording = true;
        xTaskNotifyGive(s_rec_task);
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
static void rec_task(void *arg)
{
    const size_t io_bytes = READ_FRAMES * I2S_CHANNELS * sizeof(int16_t);
    int16_t *io = heap_caps_malloc(io_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t max_samples = (size_t)REC_MAX_SECONDS * REC_RATE;
    uint8_t *wav = heap_caps_malloc(WAV_HEADER_BYTES + max_samples * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!io || !wav) {
        ESP_LOGE(TAG, "Out of memory for recording buffers");
        vTaskDelete(NULL);
    }
    int16_t *pcm = (int16_t *)(wav + WAV_HEADER_BYTES);

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        set_state(VOICE_LISTENING);
        text_clear();

        size_t got = 0;
        bsp_extra_i2s_read(io, io_bytes, &got, 200);   // drop stale buffered audio

        size_t n = 0;
        while (s_recording && n < max_samples) {
            got = 0;
            if (bsp_extra_i2s_read(io, io_bytes, &got, 1000) != ESP_OK || got == 0) {
                continue;
            }
            size_t frames = got / (I2S_CHANNELS * sizeof(int16_t));
            for (size_t i = 0; i < frames && n < max_samples; i++) {
                int32_t mixed = ((int32_t)io[2 * i] + (int32_t)io[2 * i + 1]) / 2;
                pcm[n++] = (int16_t)mixed;
            }
        }
        s_recording = false;
        s_last_activity_us = esp_timer_get_time();

        uint32_t ms = (uint32_t)(n * 1000 / REC_RATE);
        if (s_rec_cancel || !s_active) {   // the touch was a swipe, or the app closed
            s_rec_cancel = false;
            ESP_LOGI(TAG, "Discarded %u ms of audio", (unsigned)ms);
            set_state(VOICE_READY);
            continue;
        }
        ESP_LOGI(TAG, "Recorded %u ms", (unsigned)ms);

        if (ms < REC_MIN_MS) {
            set_state(VOICE_READY);
            set_note("Hold while you talk");
            continue;
        }
        if (!s_ws_connected) {
            set_state(VOICE_CONNECTING);
            set_note("Not connected to the server");
            continue;
        }

        write_wav_header(wav, (uint32_t)(n * sizeof(int16_t)), REC_RATE, 1, 16);
        size_t total = WAV_HEADER_BYTES + n * sizeof(int16_t);

        s_turn_start_us = esp_timer_get_time();
        s_last_rx_us = s_turn_start_us;
        set_state(VOICE_THINKING);

        int sent = -1;
        xSemaphoreTake(s_ws_mtx, portMAX_DELAY);
        if (s_ws) sent = esp_websocket_client_send_bin(s_ws, (const char *)wav, (int)total, pdMS_TO_TICKS(15000));
        xSemaphoreGive(s_ws_mtx);
        if (sent < 0) {
            ESP_LOGE(TAG, "Send failed");
            set_state(VOICE_READY);
            set_note("Send failed - try again");
        } else {
            ESP_LOGI(TAG, "Sent %d bytes (%u ms of audio)", sent, (unsigned)ms);
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
        if (!s_active) return;                         /* app closed: stop talking */
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

        if (!s_active) {                     /* app closed: drop what's left of the reply */
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
            while (off < in_len && s_active) {
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
            set_state(s_ws_connected ? VOICE_READY : VOICE_CONNECTING);
        }
    }
}

/* ----------------------------------------------------------------- init */
esp_err_t voice_init(void)
{
    s_txt_mtx = xSemaphoreCreateMutex();
    s_ws_mtx = xSemaphoreCreateMutex();
    s_ctl_q = xQueueCreate(8, sizeof(uint32_t));
    s_play_q = xQueueCreate(16, sizeof(play_item_t));

    if (bsp_extra_codec_init() != ESP_OK) {
        ESP_LOGE(TAG, "Audio codec init failed");
        return ESP_FAIL;
    }

    xTaskCreatePinnedToCore(rec_task, "rec", 6 * 1024, NULL, 6, &s_rec_task, 1);
    /* the MP3 decoder needs ~16 KB of stack scratch: put this task's stack in PSRAM */
    if (xTaskCreatePinnedToCoreWithCaps(play_task, "play", 32 * 1024, NULL, 6, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the playback task");
        return ESP_FAIL;
    }
    xTaskCreatePinnedToCore(ctl_task, "voice_ctl", 6 * 1024, NULL, 5, NULL, 0);
    s_audio_ok = true;
    return ESP_OK;
}
