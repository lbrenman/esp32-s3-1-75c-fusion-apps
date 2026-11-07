/* Quotes engine: one worker task that owns an HTTPS connection to the Stock API. See quotes.h.
 *
 * The worker sleeps until a screen asks for something (a quote, a watchlist), then does one GET
 * for it. The TLS connection is kept open between requests that come close together and closed
 * after QT_IDLE_CLOSE_MS without one, or when the app closes. Nothing secret is logged: the API
 * key is only ever in a request header. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "quotes.h"
#include "settings_store.h"
#include "net.h"
#include "app_config.h"

#define TAG "quotes"

#define QT_MAX_BODY       (32 * 1024)     /* a reply bigger than this is an error */
#define QT_IDLE_CLOSE_MS  30000           /* reuse the connection only for requests close together */

/* The certificate chain fetched at build time from WS_HOST (main/server_ca.pem). */
extern const char server_ca_pem_start[] asm("_binary_server_ca_pem_start");

static qt_snapshot_t    *s;               /* guarded by s_mtx; malloc'd in PSRAM */
static SemaphoreHandle_t s_mtx;
static volatile uint32_t s_ver = 1;
static TaskHandle_t      s_task;

static volatile bool s_active;
static volatile bool s_reset;
static volatile bool s_quote_req;
static volatile bool s_wl_req;

/* ---- HTTP state (worker only; in PSRAM) */
typedef struct {
    char   host[96];
    char   key[SET_KEY_MAX + 1];
    char   url[QT_ENDPOINT_MAX + 200];    /* the request being made: base + path + query */
    esp_http_client_handle_t http;
    bool   uses_ca_pem;
    int64_t last_used_us;
    char  *body;
    size_t body_len, body_cap;
    bool   body_overflow;
    int    status;
    /* worker scratch, here so it's in PSRAM rather than static (internal RAM) */
    stocks_detail_t detail;
    stocks_sym_t    syms[QT_SEL_MAX];
} qt_http_t;

static qt_http_t      *h;
static stocks_quote_t *s_scratch;         /* parsed watchlist (worker only, PSRAM) */

static void lock(void)   { xSemaphoreTake(s_mtx, portMAX_DELAY); }
static void unlock(void) { s_ver++; xSemaphoreGive(s_mtx); }
static void wake(void)   { if (s_task) xTaskNotifyGive(s_task); }

uint32_t quotes_version(void) { return s_ver; }

void quotes_get(qt_snapshot_t *out)
{
    if (!s_mtx) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = *s;
    xSemaphoreGive(s_mtx);
}

static void set_link(qt_link_t l)
{
    if (s->link == l) return;
    lock();
    s->link = l;
    unlock();
}

/* ================================================================== HTTP */
static void url_host(const char *url, char *out, size_t out_len)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t n = strcspn(p, ":/?#");
    if (n >= out_len) n = out_len - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

static void http_close(void)
{
    if (h->http) {
        ESP_LOGI(TAG, "closing connection to %s", h->host);
        esp_http_client_cleanup(h->http);
        h->http = NULL;
    }
}

static esp_err_t http_event(esp_http_client_event_t *e)
{
    if (e->event_id != HTTP_EVENT_ON_DATA || h->body_overflow || e->data_len <= 0) return ESP_OK;
    if (h->body_len + e->data_len + 1 > QT_MAX_BODY) {
        h->body_overflow = true;
        return ESP_OK;
    }
    if (h->body_len + e->data_len + 1 > h->body_cap) {
        size_t cap = h->body_cap ? h->body_cap : 2048;
        while (cap < h->body_len + e->data_len + 1) cap *= 2;
        char *nb = heap_caps_realloc(h->body, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) { h->body_overflow = true; return ESP_OK; }
        h->body = nb;
        h->body_cap = cap;
    }
    memcpy(h->body + h->body_len, e->data, e->data_len);
    h->body_len += e->data_len;
    h->body[h->body_len] = '\0';
    return ESP_OK;
}

static bool http_open(void)
{
    if (h->http) return true;
    esp_http_client_config_t cfg = {
        .url = h->url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = QT_TIMEOUT_MS,
        .event_handler = http_event,
        .keep_alive_enable = true,
        .disable_auto_redirect = true,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    h->uses_ca_pem = false;
    if (strncmp(h->url, "https://", 8) == 0) {
        if (strcasecmp(h->host, settings_build_host()) == 0) {
            cfg.cert_pem = server_ca_pem_start;           /* same host as the voice server */
            h->uses_ca_pem = true;
        } else {
            cfg.crt_bundle_attach = esp_crt_bundle_attach; /* any other host: built-in CA bundle */
        }
    }
    h->http = esp_http_client_init(&cfg);
    if (!h->http) return false;
    esp_http_client_set_header(h->http, "Accept", "application/json");
    if (h->key[0]) esp_http_client_set_header(h->http, QT_KEY_HEADER, h->key);
    ESP_LOGI(TAG, "connecting to %s (%s)", h->host,
             h->uses_ca_pem ? "server_ca.pem" : (cfg.crt_bundle_attach ? "built-in CA bundle" : "no TLS"));
    return true;
}

/* One GET of h->url (path shown in the log, never a header). False = no answer from the server. */
static bool get_once(const char *what)
{
    if (h->http && esp_timer_get_time() - h->last_used_us > (int64_t)QT_IDLE_CLOSE_MS * 1000) http_close();
    bool fresh = !h->http;
    if (!http_open()) return false;
    if (!fresh) esp_http_client_set_url(h->http, h->url);

    h->body_len = 0;
    h->body_overflow = false;
    if (h->body) h->body[0] = '\0';
    h->status = 0;

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(h->http);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (err != ESP_OK) {
        /* esp_http_client turns a 401 without a WWW-Authenticate header (API gateways send none)
         * into ESP_ERR_NOT_SUPPORTED. The server did answer: report the status, don't retry. */
        int st = esp_http_client_get_status_code(h->http);
        if (err == ESP_ERR_NOT_SUPPORTED && st >= 400) {
            h->status = st;
            ESP_LOGW(TAG, "%s -> HTTP %d in %d ms (API key rejected?)", what, st, ms);
            http_close();                      /* the body wasn't read: don't reuse the connection */
            return true;
        }
        ESP_LOGW(TAG, "%s: request to %s failed after %d ms: %s", what, h->host, ms, esp_err_to_name(err));
        http_close();
        return false;
    }
    h->status = esp_http_client_get_status_code(h->http);
    h->last_used_us = esp_timer_get_time();
    ESP_LOGI(TAG, "%s -> HTTP %d, %u bytes in %d ms", what, h->status, (unsigned)h->body_len, ms);
#if QT_DEBUG
    ESP_LOGI(TAG, "reply: %.300s", h->body ? h->body : "");
#endif
    if (h->body_overflow) {
        ESP_LOGW(TAG, "reply too big (over %d KB)", QT_MAX_BODY / 1024);
        http_close();                          /* the rest of the body is still on the wire */
    }
    return true;
}

/* GET with one retry on a network error (a kept-alive connection the server had closed). */
static bool get(const char *what)
{
    if (get_once(what)) return true;
    ESP_LOGW(TAG, "%s: retrying with a new connection", what);
    return get_once(what);
}

/* {"message":"Missing symbol"} -> "Missing symbol" */
static bool body_message(char *out, size_t len)
{
    if (!h->body || !h->body_len) return false;
    cJSON *j = cJSON_Parse(h->body);
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(j, "message");
    bool ok = cJSON_IsString(m) && m->valuestring[0];
    if (ok) snprintf(out, len, "%s", m->valuestring);
    cJSON_Delete(j);
    return ok;
}

/* Text for a reply that isn't a 200 with a body. */
static void http_error_text(char *err, size_t len)
{
    char msg[80];
    int st = h->status;
    if (st == 401 || st == 403)      snprintf(err, len, "HTTP %d - check the API key", st);
    else if (st == 404)              snprintf(err, len, "HTTP 404 - check the API URL");
    else if (body_message(msg, sizeof(msg)))
                                     snprintf(err, len, "%s (HTTP %d)", msg, st);
    else if (st >= 500)              snprintf(err, len, "Server error (HTTP %d)", st);
    else                             snprintf(err, len, "HTTP %d from the server", st);
}

static void record_last(bool ok, const char *note)
{
    lock();
    s->last_ok = ok;
    s->last_http = h->status;
    s->last_us = esp_timer_get_time();
    strlcpy(s->last_note, note ? note : "", sizeof(s->last_note));
    unlock();
}

static void set_busy(bool b)
{
    lock();
    s->busy = b;
    unlock();
}

/* ================================================================== requests */
static void do_quote(const char *base)
{
    stocks_sym_t sym;
    lock();
    strlcpy(sym, s->q_sym, sizeof(sym));
    unlock();
    s_quote_req = false;
    if (!sym[0]) return;

    char ep[QT_ENDPOINT_MAX];
    quotes_endpoint(base, QT_PATH_QUOTE, ep, sizeof(ep));
    snprintf(h->url, sizeof(h->url), "%s?symbol=%s", ep, sym);

    char err[100] = "";
    char what[32];
    snprintf(what, sizeof(what), "quote %s", sym);
    stocks_detail_t *d = &h->detail;
    set_busy(true);
    bool answered = get(what);
    set_busy(false);
    bool ok = false;
    if (!answered)                       snprintf(err, sizeof(err), "Can't reach the server");
    else if (h->body_overflow)           snprintf(err, sizeof(err), "Reply too big");
    else if (h->status == 204)           snprintf(err, sizeof(err), "No quote for %s", sym);
    else if (h->status != 200)           http_error_text(err, sizeof(err));
    else {
        ok = stocks_parse_detail(h->body, sym, d, err, sizeof(err));
        if (!ok) ESP_LOGW(TAG, "quote reply for %s: %.200s", sym, h->body ? h->body : "");
    }
    /* "No quote for X" is an answer from a working server, not a connection problem */
    record_last(ok || (answered && (h->status == 200 || h->status == 204)), ok ? "" : err);

    lock();
    if (strcmp(sym, s->q_sym) == 0) {         /* still the symbol on screen */
        if (ok) {
            s->q = *d;
            s->q_state = QT_OK;
            s->q_us = esp_timer_get_time();
            s->q_at = net_time_valid() ? time(NULL) : 0;
            s->q_note[0] = '\0';
        } else {
            s->q_state = QT_ERROR;
            strlcpy(s->q_note, err[0] ? err : "Request failed", sizeof(s->q_note));
        }
    }
    unlock();
    if (ok) ESP_LOGI(TAG, "quote %s: %.2f", sym, d->q.price);
}

static void do_watchlist(const char *base)
{
    stocks_sym_t *syms = h->syms;
    int n;
    lock();
    n = s->wl_n;
    memcpy(syms, s->wl_syms, (size_t)n * sizeof(stocks_sym_t));
    unlock();
    s_wl_req = false;
    if (n == 0) return;

    char ep[QT_ENDPOINT_MAX];
    quotes_endpoint(base, QT_PATH_WATCHLIST, ep, sizeof(ep));
    size_t len = (size_t)snprintf(h->url, sizeof(h->url), "%s?symbols=", ep);
    for (int i = 0; i < n && len < sizeof(h->url); i++) {
        len += (size_t)snprintf(h->url + len, sizeof(h->url) - len, "%s%s", i ? "," : "", syms[i]);
    }

    char err[100] = "";
    char what[32];
    snprintf(what, sizeof(what), "watchlist (%d)", n);
    int nq = 0;
    set_busy(true);
    bool answered = get(what);
    set_busy(false);
    bool ok = false;
    if (!answered)                       snprintf(err, sizeof(err), "Can't reach the server");
    else if (h->body_overflow)           snprintf(err, sizeof(err), "Reply too big");
    else if (h->status == 204)           ok = true;           /* none of them found: all "no quote" */
    else if (h->status != 200)           http_error_text(err, sizeof(err));
    else {
        ok = stocks_parse_quotes(h->body, s_scratch, QT_SEL_MAX * 2, &nq, err, sizeof(err));
        if (!ok) ESP_LOGW(TAG, "watchlist reply: %.200s", h->body ? h->body : "");
    }
    record_last(ok, ok ? "" : err);

    lock();
    bool same = n == s->wl_n && memcmp(syms, s->wl_syms, (size_t)n * sizeof(stocks_sym_t)) == 0;
    if (same && ok) {
        /* rows in the order they were picked; a symbol the server didn't quote gets "no quote" */
        int found = 0;
        for (int i = 0; i < n; i++) {
            stocks_quote_t r = { 0 };
            strlcpy(r.sym, syms[i], sizeof(r.sym));
            for (int k = 0; k < nq; k++) {
                if (strcasecmp(s_scratch[k].sym, syms[i]) == 0) { r = s_scratch[k]; strlcpy(r.sym, syms[i], sizeof(r.sym)); break; }
            }
            /* the quote service answers an unknown ticker with all zeros */
            if (r.has_price && r.price == 0 && (!r.has_change || r.change == 0)) r.has_price = false;
            if (r.has_price) found++;
            s->wl_rows[i] = r;
        }
        s->wl_state = QT_OK;
        s->wl_loaded = true;
        s->wl_us = esp_timer_get_time();
        s->wl_at = net_time_valid() ? time(NULL) : 0;
        s->wl_note[0] = '\0';
        ESP_LOGI(TAG, "watchlist: %d of %d symbols quoted", found, n);
    } else if (same) {
        s->wl_state = QT_ERROR;
        strlcpy(s->wl_note, err[0] ? err : "Request failed", sizeof(s->wl_note));
        ESP_LOGW(TAG, "watchlist failed: %s", s->wl_note);
    }
    unlock();
}

/* ================================================================== worker */
static bool url_usable(const char *url)
{
    if (strstr(url, "your-") || strstr(url, "YOUR-")) return false;   /* secrets.h placeholder */
    return strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0;
}

/* Requests waiting while the URL isn't set: fail them, so the screens say why. */
static void fail_pending(const char *why)
{
    lock();
    if (s_quote_req) { s->q_state = QT_ERROR; strlcpy(s->q_note, why, sizeof(s->q_note)); }
    if (s_wl_req)    { s->wl_state = QT_ERROR; strlcpy(s->wl_note, why, sizeof(s->wl_note)); }
    unlock();
    s_quote_req = s_wl_req = false;
}

static TickType_t next_wait(void)
{
    if (s_reset) return 0;
    if (!s_active) return portMAX_DELAY;
    if (s->link == QT_LINK_WAIT_WIFI) return pdMS_TO_TICKS(1000);
    if (s_quote_req || s_wl_req) return 0;
    if (h->http) {                                         /* wake up to close an idle connection */
        int64_t ms = QT_IDLE_CLOSE_MS - (esp_timer_get_time() - h->last_used_us) / 1000;
        return ms <= 0 ? 0 : pdMS_TO_TICKS(ms) + 1;
    }
    return portMAX_DELAY;
}

static void worker(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, next_wait());

        if (s_reset) {
            s_reset = false;
            http_close();
        }
        if (!s_active) {
            http_close();
            set_link(QT_LINK_OFF);
            continue;
        }
        if (h->http && esp_timer_get_time() - h->last_used_us > (int64_t)QT_IDLE_CLOSE_MS * 1000) http_close();

        app_settings_t *cfg = heap_caps_malloc(sizeof(*cfg), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!cfg) continue;
        settings_get(cfg);
        if (!url_usable(cfg->qt_url)) {
            set_link(QT_LINK_NOT_SET);
            fail_pending("Set the API URL (Stock API screen)");
            free(cfg);
            continue;
        }
        char host[sizeof(h->host)];
        url_host(cfg->qt_url, host, sizeof(host));
        if (strcmp(host, h->host) != 0 || strcmp(cfg->qt_api_key, h->key) != 0) {
            http_close();                       /* another server or key: start over */
            strlcpy(h->host, host, sizeof(h->host));
            strlcpy(h->key, cfg->qt_api_key, sizeof(h->key));
        }
        if (!net_is_up()) {
            set_link(QT_LINK_WAIT_WIFI);
            free(cfg);
            continue;
        }
        set_link(QT_LINK_READY);

        if (s_quote_req) do_quote(cfg->qt_url);     /* the Quote screen is waiting: first */
        if (s_wl_req && s_active) do_watchlist(cfg->qt_url);
        free(cfg);
    }
}

/* ================================================================== API (any task) */
void quotes_init(void)
{
    s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    h = heap_caps_calloc(1, sizeof(*h), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_scratch = heap_caps_calloc(QT_SEL_MAX * 2, sizeof(stocks_quote_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s || !h || !s_scratch) {
        ESP_LOGE(TAG, "out of memory: Quotes disabled");
        return;
    }
    s_mtx = xSemaphoreCreateMutex();
    s->link = QT_LINK_OFF;
    /* the stack can live in PSRAM: this task only does networking, never flash writes */
    if (xTaskCreatePinnedToCoreWithCaps(worker, "quotes", 8 * 1024, NULL, 4, &s_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        xTaskCreatePinnedToCore(worker, "quotes", 8 * 1024, NULL, 4, &s_task, 0);
    }
}

void quotes_set_active(bool active)
{
    if (!s_mtx) return;
    s_active = active;
    if (!active) s_quote_req = s_wl_req = false;
    wake();
}

void quotes_config_changed(void)
{
    if (!s_mtx) return;
    s_reset = true;
    wake();
}

void quotes_request_quote(const char *sym)
{
    if (!s_mtx || !sym || !sym[0]) return;
    lock();
    if (strcmp(s->q_sym, sym) != 0) {         /* another symbol: forget the old one */
        memset(&s->q, 0, sizeof(s->q));
        s->q_us = 0;
        s->q_at = 0;
        s->q_note[0] = '\0';
        strlcpy(s->q_sym, sym, sizeof(s->q_sym));
    }
    s->q_state = QT_LOADING;
    unlock();
    s_quote_req = true;
    wake();
}

void quotes_request_watchlist(const stocks_sym_t *syms, int n)
{
    if (!s_mtx) return;
    if (n > QT_SEL_MAX) n = QT_SEL_MAX;
    if (n < 0) n = 0;
    lock();
    bool same = n == s->wl_n && memcmp(syms, s->wl_syms, (size_t)n * sizeof(stocks_sym_t)) == 0;
    if (!same) {                              /* a new list: rows show "..." until the answer */
        s->wl_n = n;
        memcpy(s->wl_syms, syms, (size_t)n * sizeof(stocks_sym_t));
        memset(s->wl_rows, 0, sizeof(s->wl_rows));
        for (int i = 0; i < n; i++) strlcpy(s->wl_rows[i].sym, syms[i], sizeof(stocks_sym_t));
        s->wl_loaded = false;
        s->wl_us = 0;
        s->wl_at = 0;
        s->wl_note[0] = '\0';
    }
    s->wl_state = n ? QT_LOADING : QT_IDLE;
    unlock();
    if (n) {
        s_wl_req = true;
        wake();
    }
}
