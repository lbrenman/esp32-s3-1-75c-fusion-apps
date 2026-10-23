/* Phone settings page: HTTP server + JSON API + mDNS name. See web_settings.h.
 *
 *   GET  /              the page (main/web/settings.html, compiled in)
 *   GET  /api/state     current settings and status (401 until signed in)
 *   POST /api/login     {"pin":"123456"} -> session cookie
 *   POST /api/logout
 *   POST /api/settings  {"ws_url": "...", "volume": 60, ...}: only the fields sent are changed
 *   POST /api/action    {"action": "new_conversation" | "stocks_refresh" | "restart"}
 *
 * POSTs need the header "X-Fusion-Apps: 1" (a page on another site can't send it without a CORS
 * preflight, which this server never answers), and the cookie is SameSite=Strict.
 *
 * The HTTP task never touches LVGL or NVS itself: it checks the request, then hands the change to
 * the LVGL task (lv_async_call under the display lock) and waits for it to be applied. */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "mdns.h"
#include "cJSON.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "web_settings.h"
#include "settings_store.h"
#include "app_config.h"
#include "net.h"
#include "voice.h"
#include "stocks.h"
#include "power.h"
#include "translate_langs.h"
#include "ui_theme.h"
#include "ui_nav.h"
#include "ui_widgets.h"
#include "apps.h"

#define TAG "web"

#define BODY_MAX     2048            /* largest request body accepted */
#define SID_HEX      32              /* session id: 16 random bytes as hex */
#define COOKIE_NAME  "fa_sid"
#define APPLY_WAIT_MS 4000           /* how long a request waits for the LVGL task to apply it */

extern const char settings_html_start[] asm("_binary_settings_html_start");
extern const char settings_html_end[]   asm("_binary_settings_html_end");

static httpd_handle_t    s_httpd;
static bool              s_mdns_up;
static SemaphoreHandle_t s_mtx;        /* sessions and the PIN lock */
static SemaphoreHandle_t s_done;       /* the LVGL task finished applying a change */
static volatile esp_err_t s_apply_err;

static char    s_sid[WEB_MAX_SESSIONS][SID_HEX + 1];
static int     s_sid_next;
static int     s_fails;                /* wrong PINs in a row */
static int64_t s_lock_until_us;

/* ================================================================== helpers */
static void lock(void)   { xSemaphoreTake(s_mtx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mtx); }

/* Compares without stopping at the first difference (no timing hint about how much matched). */
static bool same_secret(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned diff = (unsigned)(la ^ lb);
    for (size_t i = 0; i < la && i < lb; i++) diff |= (unsigned)(a[i] ^ b[i]);
    return diff == 0 && la > 0;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *obj)
{
    char *txt = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, txt ? txt : "{}", HTTPD_RESP_USE_STRLEN);
    cJSON_free(txt);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "error", msg);
    return send_json(req, status, o);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, "200 OK", o);
}

/* The session id from the cookie, if it's one of ours. */
static bool authed(httpd_req_t *req)
{
    char sid[SID_HEX + 8];
    size_t len = sizeof(sid);
    if (httpd_req_get_cookie_val(req, COOKIE_NAME, sid, &len) != ESP_OK) return false;
    if (strlen(sid) != SID_HEX) return false;
    bool ok = false;
    lock();
    for (int i = 0; i < WEB_MAX_SESSIONS; i++) {
        if (s_sid[i][0] && same_secret(sid, s_sid[i])) ok = true;
    }
    unlock();
    return ok;
}

static bool from_our_page(httpd_req_t *req)
{
    char v[8];
    return httpd_req_get_hdr_value_str(req, "X-Fusion-Apps", v, sizeof(v)) == ESP_OK && strcmp(v, "1") == 0;
}

/* Reads the JSON body (PSRAM buffer). NULL with a reply already sent on failure. */
static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > BODY_MAX) {
        send_error(req, "400 Bad Request", req->content_len ? "Request too large" : "Empty request");
        return NULL;
    }
    char *buf = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        send_error(req, "500 Internal Server Error", "Out of memory");
        return NULL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            heap_caps_free(buf);
            return NULL;                  /* connection gone: nothing to reply to */
        }
        got += r;
    }
    buf[got] = '\0';
    cJSON *j = cJSON_Parse(buf);
    heap_caps_free(buf);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        send_error(req, "400 Bad Request", "Not a JSON object");
        return NULL;
    }
    return j;
}

/* What the page may know about an API key: whether one is saved, and its last 4 characters
 * (only for keys longer than 8, as the watch shows them). */
static void add_key_info(cJSON *o, const char *name, const char *key)
{
    char field[24];
    size_t n = strlen(key);
    snprintf(field, sizeof(field), "%s_set", name);
    cJSON_AddBoolToObject(o, field, n > 0);
    snprintf(field, sizeof(field), "%s_tail", name);
    cJSON_AddStringToObject(o, field, n > 8 ? key + n - 4 : "");
}

/* ============================================================ applying (LVGL task) */
enum {
    F_WS_URL   = 1 << 0,  F_WS_KEY  = 1 << 1,  F_WAKE    = 1 << 2,
    F_TR_URL   = 1 << 3,  F_TR_KEY  = 1 << 4,  F_TR_LANG = 1 << 5,
    F_MCP_URL  = 1 << 6,  F_MCP_KEY = 1 << 7,  F_STK_AUTO = 1 << 8,
    F_DARK     = 1 << 9,  F_BRIGHT  = 1 << 10, F_VOLUME  = 1 << 11,
};

typedef enum { ACT_NONE, ACT_NEW_CONV, ACT_STOCKS_REFRESH } web_action_t;

typedef struct {
    uint32_t       mask;     /* F_* fields sent by the page */
    app_settings_t v;        /* their values (other fields unused) */
    web_action_t   action;
} web_job_t;

static void refresh_timer_cb(lv_timer_t *t);

/* Rebuild the current screen once no transition is running (theme change, or Settings showing). */
static void refresh_soon(void)
{
    if (!ui_nav_busy()) {
        app_settings_refresh();
        return;
    }
    lv_timer_t *t = lv_timer_create(refresh_timer_cb, UI_ANIM_MS + 80, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void refresh_timer_cb(lv_timer_t *t) { refresh_soon(); }

static void apply_cb(void *arg)
{
    web_job_t *j = arg;
    app_settings_t cfg, old;
    settings_get(&old);
    cfg = old;

    /* merge onto the settings as they are now (the watch may have changed something meanwhile) */
    if (j->mask & F_WS_URL)   strlcpy(cfg.ws_url, j->v.ws_url, sizeof(cfg.ws_url));
    if (j->mask & F_WS_KEY)   strlcpy(cfg.ws_api_key, j->v.ws_api_key, sizeof(cfg.ws_api_key));
    if (j->mask & F_WAKE)     cfg.wake_word = j->v.wake_word;
    if (j->mask & F_TR_URL)   strlcpy(cfg.tr_url, j->v.tr_url, sizeof(cfg.tr_url));
    if (j->mask & F_TR_KEY)   strlcpy(cfg.tr_api_key, j->v.tr_api_key, sizeof(cfg.tr_api_key));
    if (j->mask & F_TR_LANG) {
        strlcpy(cfg.tr_src, j->v.tr_src, sizeof(cfg.tr_src));
        strlcpy(cfg.tr_dst, j->v.tr_dst, sizeof(cfg.tr_dst));
    }
    if (j->mask & F_MCP_URL)  strlcpy(cfg.mcp_url, j->v.mcp_url, sizeof(cfg.mcp_url));
    if (j->mask & F_MCP_KEY)  strlcpy(cfg.mcp_api_key, j->v.mcp_api_key, sizeof(cfg.mcp_api_key));
    if (j->mask & F_STK_AUTO) cfg.stocks_auto = j->v.stocks_auto;
    if (j->mask & F_DARK)     cfg.dark_mode = j->v.dark_mode;
    if (j->mask & F_BRIGHT)   cfg.brightness = j->v.brightness;
    if (j->mask & F_VOLUME)   cfg.volume = j->v.volume;

    bool ws_changed   = strcmp(cfg.ws_url, old.ws_url) || strcmp(cfg.ws_api_key, old.ws_api_key);
    bool tr_changed   = strcmp(cfg.tr_url, old.tr_url) || strcmp(cfg.tr_api_key, old.tr_api_key);
    bool lang_changed = strcmp(cfg.tr_src, old.tr_src) || strcmp(cfg.tr_dst, old.tr_dst);
    bool mcp_changed  = strcmp(cfg.mcp_url, old.mcp_url) || strcmp(cfg.mcp_api_key, old.mcp_api_key);
    bool changed = memcmp(&cfg, &old, sizeof(cfg)) != 0;

    esp_err_t err = ESP_OK;
    if (changed) err = settings_set(&cfg);

    /* apply it the way the watch's own screens do */
    bool voice_open = voice_state() != VOICE_OFF;
    if (ws_changed && voice_open && voice_mode() == VOICE_MODE_CHAT && voice_wants_connection()) {
        voice_reconnect();
    }
    if (tr_changed && voice_open && voice_mode() == VOICE_MODE_TRANSLATE && voice_wants_connection()) {
        voice_reconnect();
    } else if (lang_changed && voice_mode() == VOICE_MODE_TRANSLATE) {
        voice_reconnect_when_idle();         /* waits for a translation still on its way */
    }
    if (cfg.wake_word != old.wake_word)     voice_set_wake_word(cfg.wake_word);
    if (cfg.volume != old.volume)           voice_set_volume(cfg.volume);
    if (cfg.brightness != old.brightness)   bsp_display_brightness_set(cfg.brightness);
    if (mcp_changed)                        stocks_config_changed();
    if (cfg.stocks_auto != old.stocks_auto) stocks_set_auto(cfg.stocks_auto);

    if (j->action == ACT_NEW_CONV)        voice_new_conversation();
    if (j->action == ACT_STOCKS_REFRESH)  stocks_refresh_now();

    bool dark_changed = cfg.dark_mode != old.dark_mode;
    if (dark_changed) ui_theme_set_dark(cfg.dark_mode);
    if (dark_changed || ((changed || j->action == ACT_NEW_CONV) && app_settings_showing())) {
        refresh_soon();                      /* show the new values (and colors) on the watch */
    }
    if (changed || j->action == ACT_NEW_CONV) ui_toast("Changed from your phone");

    heap_caps_free(j);
    s_apply_err = err;
    xSemaphoreGive(s_done);
}

/* Hands the job to the LVGL task and waits until it's applied. Frees the job either way. */
static esp_err_t run_job(web_job_t *j)
{
    xSemaphoreTake(s_done, 0);              /* drop a stale "done" from a request that timed out */
    if (bsp_display_lock(2000) != ESP_OK) {      /* esp_err_t in the 1.75C BSP 3.x */
        heap_caps_free(j);
        return ESP_ERR_TIMEOUT;
    }
    lv_result_t r = lv_async_call(apply_cb, j);
    bsp_display_unlock();
    if (r != LV_RESULT_OK) {
        heap_caps_free(j);
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(APPLY_WAIT_MS)) != pdTRUE) return ESP_ERR_TIMEOUT;
    return s_apply_err;
}

/* ================================================================== handlers */
static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    /* EMBED_TXTFILES adds a terminating 0: don't send it */
    return httpd_resp_send(req, settings_html_start, settings_html_end - settings_html_start - 1);
}

static esp_err_t state_get(httpd_req_t *req)
{
    if (!authed(req)) return send_error(req, "401 Unauthorized", "Enter the PIN");

    app_settings_t cfg;
    settings_get(&cfg);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "app", APP_NAME);
    cJSON_AddStringToObject(o, "version", APP_VERSION);

    /* settings */
    cJSON *s = cJSON_AddObjectToObject(o, "settings");
    cJSON_AddStringToObject(s, "ws_url", cfg.ws_url);
    add_key_info(s, "ws_key", cfg.ws_api_key);
    cJSON_AddBoolToObject(s, "wake_word", cfg.wake_word);
    cJSON_AddStringToObject(s, "conversation", cfg.conversation_id);
    cJSON_AddStringToObject(s, "tr_url", cfg.tr_url);
    add_key_info(s, "tr_key", cfg.tr_api_key);
    cJSON_AddStringToObject(s, "tr_src", cfg.tr_src);
    cJSON_AddStringToObject(s, "tr_dst", cfg.tr_dst);
    cJSON_AddStringToObject(s, "mcp_url", cfg.mcp_url);
    add_key_info(s, "mcp_key", cfg.mcp_api_key);
    cJSON_AddBoolToObject(s, "stocks_auto", cfg.stocks_auto);
    cJSON_AddBoolToObject(s, "dark_mode", cfg.dark_mode);
    cJSON_AddNumberToObject(s, "brightness", cfg.brightness);
    cJSON_AddNumberToObject(s, "volume", cfg.volume);

    /* the secrets.h URLs, for the "Use secrets.h URL" buttons */
    cJSON *d = cJSON_AddObjectToObject(o, "defaults");
    cJSON_AddStringToObject(d, "ws_url", settings_default_url());
    cJSON_AddStringToObject(d, "tr_url", settings_default_tr_url());
    cJSON_AddStringToObject(d, "mcp_url", settings_default_mcp_url());

    /* status */
    cJSON *st = cJSON_AddObjectToObject(o, "status");
    char ip[20] = "";
    net_ip(ip, sizeof(ip));
    cJSON_AddStringToObject(st, "wifi_ssid", cfg.wifi_ssid);
    cJSON_AddStringToObject(st, "ip", ip);
    cJSON_AddNumberToObject(st, "rssi", net_rssi());
    power_info_t p;
    power_get(&p);
    cJSON *b = cJSON_AddObjectToObject(st, "battery");
    cJSON_AddBoolToObject(b, "available", p.available);
    cJSON_AddBoolToObject(b, "present", p.battery);
    cJSON_AddNumberToObject(b, "percent", p.percent);
    cJSON_AddNumberToObject(b, "millivolts", p.millivolts);
    cJSON_AddBoolToObject(b, "usb", p.usb);
    cJSON_AddBoolToObject(b, "charging", p.charging);
    cJSON_AddBoolToObject(b, "full", p.full);
    voice_state_t vs = voice_state();
    cJSON_AddStringToObject(st, "voice_app", vs == VOICE_OFF ? "" :
                            voice_mode() == VOICE_MODE_CHAT ? "Voice chat" : "Translator");
    cJSON_AddStringToObject(st, "voice_state", voice_state_text(vs));
    static const char *const wake_txt[] = { "off", "on", "no_model", "no_memory" };
    wake_status_t ws = voice_wake_status();
    cJSON_AddStringToObject(st, "wake", ws <= WAKE_NO_MEMORY ? wake_txt[ws] : "off");
    cJSON_AddNumberToObject(st, "ram_kb", (double)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    cJSON_AddNumberToObject(st, "psram_kb", (double)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    /* languages for the From / Into lists */
    cJSON *langs = cJSON_AddArrayToObject(o, "langs");
    for (int i = 0; i < g_tr_lang_count; i++) {
        cJSON *l = cJSON_CreateObject();
        cJSON_AddStringToObject(l, "code", g_tr_langs[i].code);
        cJSON_AddStringToObject(l, "name", g_tr_langs[i].name);
        cJSON_AddBoolToObject(l, "source", g_tr_langs[i].source);
        cJSON_AddBoolToObject(l, "target", g_tr_langs[i].target);
        cJSON_AddItemToArray(langs, l);
    }
    return send_json(req, "200 OK", o);
}

static esp_err_t login_post(httpd_req_t *req)
{
    if (!from_our_page(req)) return send_error(req, "403 Forbidden", "Use the settings page");

    int64_t now = esp_timer_get_time();
    lock();
    int64_t left_us = s_lock_until_us - now;
    unlock();
    if (left_us > 0) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Too many wrong PINs. Try again in %d s.", (int)(left_us / 1000000) + 1);
        return send_error(req, "429 Too Many Requests", msg);
    }

    cJSON *j = read_json(req);
    if (!j) return ESP_OK;
    const cJSON *pin = cJSON_GetObjectItemCaseSensitive(j, "pin");
    char given[SET_PIN_LEN + 2] = "";
    if (cJSON_IsString(pin) && strlen(pin->valuestring) <= SET_PIN_LEN) {
        strlcpy(given, pin->valuestring, sizeof(given));
    }
    cJSON_Delete(j);

    app_settings_t cfg;
    settings_get(&cfg);
    bool ok = cfg.web_enabled && same_secret(given, cfg.web_pin);

    if (!ok) {
        lock();
        s_fails++;
        int fails = s_fails;
        if (fails >= WEB_PIN_MAX_TRIES) {
            s_fails = 0;
            s_lock_until_us = now + (int64_t)WEB_PIN_LOCK_S * 1000000;
        }
        unlock();
        ESP_LOGW(TAG, "Wrong PIN (%d of %d)%s", fails, WEB_PIN_MAX_TRIES,
                 fails >= WEB_PIN_MAX_TRIES ? ": locked for a while" : "");
        if (fails >= WEB_PIN_MAX_TRIES) {
            char msg[80];
            snprintf(msg, sizeof(msg), "Too many wrong PINs. Try again in %d s.", WEB_PIN_LOCK_S);
            return send_error(req, "429 Too Many Requests", msg);
        }
        return send_error(req, "401 Unauthorized", "Wrong PIN");
    }

    /* new session: 16 random bytes; the oldest one makes room when all slots are taken */
    uint8_t rnd[SID_HEX / 2];
    esp_fill_random(rnd, sizeof(rnd));
    char sid[SID_HEX + 1];
    for (int i = 0; i < (int)sizeof(rnd); i++) snprintf(sid + 2 * i, 3, "%02x", rnd[i]);
    lock();
    s_fails = 0;
    strlcpy(s_sid[s_sid_next], sid, sizeof(s_sid[0]));
    s_sid_next = (s_sid_next + 1) % WEB_MAX_SESSIONS;
    unlock();
    ESP_LOGI(TAG, "Signed in (%d browser%s)", web_settings_sessions(), web_settings_sessions() == 1 ? "" : "s");

    char cookie[120];
    snprintf(cookie, sizeof(cookie), COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=2592000", sid);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    return send_ok(req);
}

static esp_err_t logout_post(httpd_req_t *req)
{
    if (!from_our_page(req)) return send_error(req, "403 Forbidden", "Use the settings page");
    char sid[SID_HEX + 8];
    size_t len = sizeof(sid);
    if (httpd_req_get_cookie_val(req, COOKIE_NAME, sid, &len) == ESP_OK) {
        lock();
        for (int i = 0; i < WEB_MAX_SESSIONS; i++) {
            if (s_sid[i][0] && same_secret(sid, s_sid[i])) s_sid[i][0] = '\0';
        }
        unlock();
    }
    httpd_resp_set_hdr(req, "Set-Cookie", COOKIE_NAME "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    return send_ok(req);
}

/* Copies a string field into dst if present. Returns false (with *err set) if it's invalid. */
static bool take_str(const cJSON *j, const char *name, char *dst, size_t cap, const char *const prefixes[],
                     uint32_t bit, uint32_t *mask, const char **err)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(j, name);
    if (!it) return true;
    if (!cJSON_IsString(it)) { *err = "Expected text"; return false; }
    const char *v = it->valuestring;
    if (strlen(v) >= cap) { *err = "Too long"; return false; }
    for (const char *c = v; *c; c++) {
        if ((unsigned char)*c < 0x20) { *err = "Contains a control character"; return false; }
    }
    if (prefixes) {
        bool ok = false;
        for (int i = 0; prefixes[i]; i++) {
            if (strncmp(v, prefixes[i], strlen(prefixes[i])) == 0) ok = true;
        }
        if (!ok) { *err = "Wrong URL scheme"; return false; }
    }
    strlcpy(dst, v, cap);
    *mask |= bit;
    return true;
}

static bool take_bool(const cJSON *j, const char *name, bool *dst, uint32_t bit, uint32_t *mask, const char **err)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(j, name);
    if (!it) return true;
    if (!cJSON_IsBool(it)) { *err = "Expected true or false"; return false; }
    *dst = cJSON_IsTrue(it);
    *mask |= bit;
    return true;
}

static bool take_int(const cJSON *j, const char *name, int lo, int hi, uint8_t *dst, uint32_t bit, uint32_t *mask,
                     const char **err)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(j, name);
    if (!it) return true;
    if (!cJSON_IsNumber(it) || it->valuedouble < lo || it->valuedouble > hi) { *err = "Out of range"; return false; }
    *dst = (uint8_t)it->valueint;
    *mask |= bit;
    return true;
}

static esp_err_t settings_post(httpd_req_t *req)
{
    if (!from_our_page(req)) return send_error(req, "403 Forbidden", "Use the settings page");
    if (!authed(req)) return send_error(req, "401 Unauthorized", "Enter the PIN");
    cJSON *j = read_json(req);
    if (!j) return ESP_OK;

    static const char *const ws_schemes[]   = { "wss://", "ws://", NULL };
    static const char *const http_schemes[] = { "https://", "http://", NULL };
    static const char *const known[] = {
        "ws_url", "ws_key", "wake_word", "tr_url", "tr_key", "tr_src", "tr_dst", "mcp_url", "mcp_key",
        "stocks_auto", "dark_mode", "brightness", "volume", NULL,
    };

    web_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!job) {
        cJSON_Delete(j);
        return send_error(req, "500 Internal Server Error", "Out of memory");
    }
    const char *err = NULL;
    const char *bad = NULL;
    app_settings_t *v = &job->v;
    uint32_t *m = &job->mask;

    /* unknown names are an error, so a typo in the page never fails silently */
    for (const cJSON *it = j->child; it && !err; it = it->next) {
        bool k = false;
        for (int i = 0; known[i]; i++) if (it->string && strcmp(it->string, known[i]) == 0) k = true;
        if (!k) { err = "Unknown setting"; bad = it->string; }
    }

#define CHECK(name, call) if (!err && !(call)) bad = name
    CHECK("ws_url",  take_str(j, "ws_url", v->ws_url, sizeof(v->ws_url), ws_schemes, F_WS_URL, m, &err));
    CHECK("ws_key",  take_str(j, "ws_key", v->ws_api_key, sizeof(v->ws_api_key), NULL, F_WS_KEY, m, &err));
    CHECK("wake_word", take_bool(j, "wake_word", &v->wake_word, F_WAKE, m, &err));
    CHECK("tr_url",  take_str(j, "tr_url", v->tr_url, sizeof(v->tr_url), ws_schemes, F_TR_URL, m, &err));
    CHECK("tr_key",  take_str(j, "tr_key", v->tr_api_key, sizeof(v->tr_api_key), NULL, F_TR_KEY, m, &err));
    CHECK("mcp_url", take_str(j, "mcp_url", v->mcp_url, sizeof(v->mcp_url), http_schemes, F_MCP_URL, m, &err));
    CHECK("mcp_key", take_str(j, "mcp_key", v->mcp_api_key, sizeof(v->mcp_api_key), NULL, F_MCP_KEY, m, &err));
    CHECK("stocks_auto", take_bool(j, "stocks_auto", &v->stocks_auto, F_STK_AUTO, m, &err));
    CHECK("dark_mode",   take_bool(j, "dark_mode", &v->dark_mode, F_DARK, m, &err));
    CHECK("brightness",  take_int(j, "brightness", 5, 100, &v->brightness, F_BRIGHT, m, &err));
    CHECK("volume",      take_int(j, "volume", 0, 100, &v->volume, F_VOLUME, m, &err));
#undef CHECK

    /* the language pair is sent as both sides together, and must be one the Translator can use */
    if (!err) {
        uint32_t lm = 0;
        const char *e2 = NULL;
        take_str(j, "tr_src", v->tr_src, sizeof(v->tr_src), NULL, 1, &lm, &e2);
        take_str(j, "tr_dst", v->tr_dst, sizeof(v->tr_dst), NULL, 2, &lm, &e2);
        if (e2) {
            err = e2; bad = "tr_src";
        } else if (lm && lm != 3) {
            err = "Send tr_src and tr_dst together"; bad = "tr_src";
        } else if (lm == 3) {
            if (!tr_pair_valid(v->tr_src, v->tr_dst)) {
                err = "The Translator can't use that language pair"; bad = "tr_dst";
            } else {
                *m |= F_TR_LANG;
            }
        }
    }
    cJSON_Delete(j);

    if (err) {
        heap_caps_free(job);
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: %s", bad ? bad : "request", err);
        return send_error(req, "400 Bad Request", msg);
    }
    if (!job->mask) {
        heap_caps_free(job);
        return send_ok(req);
    }
    ESP_LOGI(TAG, "Settings changed from the page (fields 0x%03x)", (unsigned)job->mask);   /* names only, never values */
    esp_err_t r = run_job(job);
    if (r == ESP_ERR_TIMEOUT) return send_error(req, "503 Service Unavailable", "The watch is busy - try again");
    if (r != ESP_OK) return send_error(req, "500 Internal Server Error", "Saving failed - see the serial log");
    return send_ok(req);
}

static void restart_cb(void *arg) { esp_restart(); }

static esp_err_t action_post(httpd_req_t *req)
{
    if (!from_our_page(req)) return send_error(req, "403 Forbidden", "Use the settings page");
    if (!authed(req)) return send_error(req, "401 Unauthorized", "Enter the PIN");
    cJSON *j = read_json(req);
    if (!j) return ESP_OK;
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(j, "action");
    char act[24] = "";
    if (cJSON_IsString(a)) strlcpy(act, a->valuestring, sizeof(act));
    cJSON_Delete(j);

    if (strcmp(act, "restart") == 0) {
        ESP_LOGI(TAG, "Restart requested from the page");
        static esp_timer_handle_t t;
        if (!t) {
            const esp_timer_create_args_t args = { .callback = restart_cb, .name = "web_restart" };
            esp_timer_create(&args, &t);
        }
        esp_timer_start_once(t, 800 * 1000);  /* time for this reply to go out */
        return send_ok(req);
    }

    web_action_t action = strcmp(act, "new_conversation") == 0 ? ACT_NEW_CONV :
                          strcmp(act, "stocks_refresh") == 0   ? ACT_STOCKS_REFRESH : ACT_NONE;
    if (action == ACT_NONE) return send_error(req, "400 Bad Request", "Unknown action");
    web_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!job) return send_error(req, "500 Internal Server Error", "Out of memory");
    job->action = action;
    ESP_LOGI(TAG, "Action from the page: %s", act);
    esp_err_t r = run_job(job);
    if (r == ESP_ERR_TIMEOUT) return send_error(req, "503 Service Unavailable", "The watch is busy - try again");
    if (r != ESP_OK) return send_error(req, "500 Internal Server Error", "Failed - see the serial log");
    return send_ok(req);
}

/* ================================================================ start / stop */
static void start_mdns(void)
{
    if (s_mdns_up) return;
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS failed (%s): use the IP address instead", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(WEB_HOSTNAME);
    mdns_instance_name_set(APP_NAME);
    mdns_service_add(APP_NAME " settings", "_http", "_tcp", 80, NULL, 0);
    s_mdns_up = true;
}

static void stop_mdns(void)
{
    if (!s_mdns_up) return;
    mdns_free();
    s_mdns_up = false;
}

static void start_server(void)
{
    if (s_httpd) return;
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.stack_size = 8192;
    c.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;   /* keep internal RAM for the display */
    c.max_open_sockets = 3;          /* the page needs few; leaves sockets for Wi-Fi apps */
    c.lru_purge_enable = true;
    c.max_uri_handlers = 8;
    const httpd_uri_t uris[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = page_get },
        { .uri = "/api/state",    .method = HTTP_GET,  .handler = state_get },
        { .uri = "/api/login",    .method = HTTP_POST, .handler = login_post },
        { .uri = "/api/logout",   .method = HTTP_POST, .handler = logout_post },
        { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post },
        { .uri = "/api/action",   .method = HTTP_POST, .handler = action_post },
    };
    esp_err_t err = httpd_start(&s_httpd, &c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Web server didn't start: %s", esp_err_to_name(err));
        s_httpd = NULL;
        return;
    }
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_httpd, &uris[i]);
    start_mdns();
    ESP_LOGI(TAG, "Phone settings page on: http://%s (internal RAM free %u KB)", web_settings_host(),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

static void stop_server(void)
{
    if (!s_httpd) return;
    httpd_stop(s_httpd);
    s_httpd = NULL;
    stop_mdns();
    web_settings_sign_out_all();
    ESP_LOGI(TAG, "Phone settings page off");
}

void web_settings_start(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (!s_done) s_done = xSemaphoreCreateBinary();
    app_settings_t cfg;
    settings_get(&cfg);
    if (cfg.web_enabled) start_server();
}

void web_settings_set_enabled(bool on)
{
    app_settings_t cfg;
    settings_get(&cfg);
    if (cfg.web_enabled != on) {
        cfg.web_enabled = on;
        settings_set(&cfg);
    }
    if (on) start_server();
    else    stop_server();
}

bool web_settings_running(void) { return s_httpd != NULL; }

int web_settings_sessions(void)
{
    if (!s_mtx) return 0;
    int n = 0;
    lock();
    for (int i = 0; i < WEB_MAX_SESSIONS; i++) if (s_sid[i][0]) n++;
    unlock();
    return n;
}

void web_settings_sign_out_all(void)
{
    if (!s_mtx) return;
    lock();
    memset(s_sid, 0, sizeof(s_sid));
    s_fails = 0;
    unlock();
}

const char *web_settings_host(void) { return WEB_HOSTNAME ".local"; }
