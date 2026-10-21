/* Stocks engine: one worker task that owns the MCP client. See stocks.h.
 *
 * The worker sleeps until something needs doing: a queued add/remove, a refresh request, or the
 * next automatic refresh. Every refresh reads the symbol list and the quotes (two tool calls over
 * one kept-alive TLS connection), so changes made elsewhere, e.g. from Claude through the same MCP
 * server, show up too. Leaving the app closes the connection. */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "stocks.h"
#include "mcp_client.h"
#include "settings_store.h"
#include "net.h"
#include "app_config.h"

#define TAG "stocks"

typedef struct {
    bool         add;
    stocks_sym_t sym;
} change_t;

static stocks_snapshot_t *s;              /* guarded by s_mtx; big, so malloc'd (lands in PSRAM) */
static SemaphoreHandle_t s_mtx;
static volatile uint32_t s_ver = 1;
static TaskHandle_t      s_task;

static change_t s_queue[STOCKS_PEND_MAX]; /* FIFO of changes not started yet (guarded by s_mtx) */
static int      s_qlen;

static volatile bool s_active;
static volatile bool s_refresh_req;
static volatile bool s_reset;
static volatile bool s_detail_req;         /* fetch s->detail_sym */

/* worker-only scratch (big: kept off the task stack and out of internal RAM) */
static stocks_quote_t *s_quotes;
static stocks_sym_t   *s_syms;

static void lock(void)   { xSemaphoreTake(s_mtx, portMAX_DELAY); }
static void unlock(void) { s_ver++; xSemaphoreGive(s_mtx); }
static void wake(void)   { if (s_task) xTaskNotifyGive(s_task); }

uint32_t stocks_version(void) { return s_ver; }

void stocks_get(stocks_snapshot_t *out)
{
    if (!s_mtx) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = *s;
    xSemaphoreGive(s_mtx);
}

bool stocks_auto(void) { return s_mtx && s->auto_refresh; }

static void set_state(stocks_state_t st, const char *note)
{
    lock();
    s->state = st;
    if (note) strlcpy(s->note, note, sizeof(s->note));
    unlock();
}

static void set_toast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_toast(const char *fmt, ...)
{
    lock();
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->toast, sizeof(s->toast), fmt, ap);
    va_end(ap);
    s->toast_ver++;
    unlock();
}

/* ---------------------------------------------------------------- list helpers (locked) */
static int find_sym(const stocks_sym_t *list, int n, const char *sym)
{
    for (int i = 0; i < n; i++) if (strcmp(list[i], sym) == 0) return i;
    return -1;
}

static void remove_at(stocks_sym_t *list, int *n, int i)
{
    memmove(&list[i], &list[i + 1], (size_t)(*n - i - 1) * sizeof(stocks_sym_t));
    (*n)--;
}

static void remove_row(const char *sym)
{
    for (int i = 0; i < s->nrows; i++) {
        if (strcmp(s->rows[i].sym, sym) == 0) {
            memmove(&s->rows[i], &s->rows[i + 1], (size_t)(s->nrows - i - 1) * sizeof(stocks_quote_t));
            s->nrows--;
            return;
        }
    }
}

/* ---------------------------------------------------------------- tool calls (worker) */
static mcp_client_t *s_mcp;

static mcp_status_t call(const char *tool, const char *sym, char **text, char *err, size_t err_len)
{
    cJSON *args = cJSON_CreateObject();
    if (sym) cJSON_AddStringToObject(args, STOCKS_SYMBOL_ARG, sym);
    lock(); s->busy = true; unlock();
    mcp_status_t st = mcp_call_tool(s_mcp, tool, args, text, err, err_len);
    lock(); s->busy = false; unlock();
    cJSON_Delete(args);
    return st;
}

static void refresh(void)
{
    char err[100] = "";
    char *text = NULL;
    int nsyms = 0, nq = 0;
    s_refresh_req = false;

    lock();
    if (!s->quotes_loaded) s->state = STOCKS_LOADING;
    unlock();

    /* 1. the symbol list (also covers symbols the server can't quote) */
    mcp_status_t st = call(STOCKS_TOOL_LIST, NULL, &text, err, sizeof(err));
    bool ok = st == MCP_OK && stocks_parse_symbols(text, s_syms, STOCKS_MAX, &nsyms, err, sizeof(err));
    if (st == MCP_OK && !ok) ESP_LOGW(TAG, "symbols reply: %.200s", text);
    free(text);
    text = NULL;
    if (ok) {
        lock();
        memcpy(s->syms, s_syms, (size_t)nsyms * sizeof(stocks_sym_t));
        s->nsyms = nsyms;
        s->list_loaded = true;
        unlock();

        /* 2. the quotes */
        st = call(STOCKS_TOOL_QUOTES, NULL, &text, err, sizeof(err));
        ok = st == MCP_OK && stocks_parse_quotes(text, s_quotes, STOCKS_MAX, &nq, err, sizeof(err));
        if (st == MCP_OK && !ok) ESP_LOGW(TAG, "quotes reply: %.200s", text);
        free(text);
    }

    int64_t now = esp_timer_get_time();
    lock();
    if (ok) {
        /* rows in the server's list order; listed symbols without a quote get a "no quote" row */
        int n = 0;
        for (int i = 0; i < nsyms && n < STOCKS_MAX; i++) {
            bool dup = false;
            for (int j = 0; j < n; j++) if (!strcmp(s->rows[j].sym, s_syms[i])) dup = true;
            if (dup) continue;
            stocks_quote_t r = { 0 };
            strlcpy(r.sym, s_syms[i], sizeof(r.sym));
            for (int k = 0; k < nq; k++) {
                if (!strcmp(s_quotes[k].sym, s_syms[i])) { r = s_quotes[k]; break; }
            }
            s->rows[n++] = r;
        }
        for (int k = 0; k < nq && n < STOCKS_MAX; k++) {         /* quoted but not listed */
            bool seen = false;
            for (int j = 0; j < n; j++) if (!strcmp(s->rows[j].sym, s_quotes[k].sym)) seen = true;
            if (!seen) s->rows[n++] = s_quotes[k];
        }
        s->nrows = n;
        s->quotes_loaded = true;
        s->updated_us = now;
        s->updated_at = net_time_valid() ? time(NULL) : 0;
        s->state = STOCKS_OK;
        s->note[0] = '\0';
        s->next_us = s->auto_refresh ? now + (int64_t)STOCKS_REFRESH_MS * 1000 : 0;
        ESP_LOGI(TAG, "%d symbols, %d quotes", nsyms, nq);
    } else {
        s->state = STOCKS_ERROR;
        strlcpy(s->note, err[0] ? err : "Request failed", sizeof(s->note));
        s->next_us = s->auto_refresh ? now + (int64_t)STOCKS_RETRY_MS * 1000 : 0;
        ESP_LOGW(TAG, "refresh failed: %s", s->note);
    }
    unlock();
}

static void fetch_detail(void)
{
    stocks_sym_t sym;
    lock();
    strlcpy(sym, s->detail_sym, sizeof(sym));
    s->detail_state = STOCKS_DETAIL_LOADING;
    unlock();
    s_detail_req = false;

    char err[100] = "";
    char *text = NULL;
    static stocks_detail_t d;                 /* worker only; keeps it off the stack */
    mcp_status_t st = call(STOCKS_TOOL_QUOTE, sym, &text, err, sizeof(err));
    bool ok = st == MCP_OK && stocks_parse_detail(text, sym, &d, err, sizeof(err));
    if (st == MCP_OK && !ok) ESP_LOGW(TAG, "quote reply for %s: %.200s", sym, text ? text : "");
    free(text);

    lock();
    if (strcmp(sym, s->detail_sym) == 0) {    /* still the symbol on screen */
        if (ok) {
            s->detail = d;
            s->detail_state = STOCKS_DETAIL_OK;
            s->detail_us = esp_timer_get_time();
            s->detail_at = net_time_valid() ? time(NULL) : 0;
            s->detail_note[0] = '\0';
        } else {
            s->detail_state = STOCKS_DETAIL_ERROR;
            strlcpy(s->detail_note, err[0] ? err : "Request failed", sizeof(s->detail_note));
        }
    }
    unlock();
    if (ok) ESP_LOGI(TAG, "quote %s: %.2f", sym, d.q.price);
}

static void do_change(const change_t *c)
{
    char err[100] = "";
    char *text = NULL;
    ESP_LOGI(TAG, "%s %s", c->add ? "add" : "remove", c->sym);
    mcp_status_t st = call(c->add ? STOCKS_TOOL_ADD : STOCKS_TOOL_REMOVE, c->sym, &text, err, sizeof(err));
    stocks_reply_t kind = st == MCP_OK ? stocks_classify(text) : STOCKS_REPLY_ERROR;
    if (text) ESP_LOGI(TAG, "%s %s -> %s", c->add ? "add" : "remove", c->sym, text);

    lock();
    int i = find_sym(s->syms, s->nsyms, c->sym);
    if (st != MCP_OK) {
        /* didn't get an answer: leave the list alone; the refresh after this sorts it out */
    } else if (c->add && (kind == STOCKS_REPLY_OK || kind == STOCKS_REPLY_EXISTS)) {
        if (i < 0 && s->nsyms < STOCKS_MAX) strlcpy(s->syms[s->nsyms++], c->sym, sizeof(stocks_sym_t));
    } else if (!c->add && (kind == STOCKS_REPLY_OK || kind == STOCKS_REPLY_ERROR)) {
        /* "Error deleting..." means it wasn't there. Removes one copy if it was listed twice. */
        if (i >= 0) remove_at(s->syms, &s->nsyms, i);
        if (find_sym(s->syms, s->nsyms, c->sym) < 0) remove_row(c->sym);
    }
    int p = find_sym(s->pend, s->npend, c->sym);
    if (p >= 0) remove_at(s->pend, &s->npend, p);
    unlock();

    if (st != MCP_OK)                                   set_toast("%s %s failed: %s", c->add ? "Add" : "Remove", c->sym, err);
    else if (c->add && kind == STOCKS_REPLY_OK)         set_toast("Added %s", c->sym);
    else if (c->add && kind == STOCKS_REPLY_EXISTS)     set_toast("%s was already listed", c->sym);
    else if (!c->add && kind == STOCKS_REPLY_OK)        set_toast("Removed %s", c->sym);
    else if (!c->add && kind == STOCKS_REPLY_ERROR)     set_toast("%s wasn't in the list", c->sym);
    else                                                set_toast("%.70s", text ? text : "?");
    free(text);
}

static bool pop_change(change_t *out)
{
    bool got = false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_qlen > 0) {
        *out = s_queue[0];
        memmove(&s_queue[0], &s_queue[1], (size_t)(s_qlen - 1) * sizeof(change_t));
        s_qlen--;
        got = true;
    }
    xSemaphoreGive(s_mtx);
    return got;
}

static bool url_usable(const char *url)
{
    if (strstr(url, "your-") || strstr(url, "YOUR-")) return false;   /* secrets.h placeholder */
    return strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0;
}

static TickType_t next_wait(void)
{
    if (s_reset) return 0;
    if (s_detail_req && s_active && s->state != STOCKS_WAIT_WIFI && s->state != STOCKS_NOT_SET) return 0;
    if (s->state == STOCKS_WAIT_WIFI) return pdMS_TO_TICKS(1000);
    if (s->state == STOCKS_NOT_SET) return portMAX_DELAY;      /* until the URL changes */
    if (s_qlen > 0) return 0;
    if (!s_active) return portMAX_DELAY;
    if (s_refresh_req) return 0;
    if (s->next_us == 0) return portMAX_DELAY;
    int64_t ms = (s->next_us - esp_timer_get_time()) / 1000;
    return ms <= 0 ? 0 : pdMS_TO_TICKS(ms);
}

static void clear_data(void)
{
    lock();
    s->nrows = s->nsyms = 0;
    s->list_loaded = s->quotes_loaded = false;
    s->updated_us = 0;
    s->updated_at = 0;
    s->next_us = 0;
    s->note[0] = '\0';
    unlock();
}

static void worker(void *arg)
{
    s_mcp = mcp_client_create("fusion-apps-stocks", APP_VERSION);
    bool open = false;                      /* a connection may be open */

    for (;;) {
        ulTaskNotifyTake(pdTRUE, next_wait());

        if (s_reset) {
            s_reset = false;
            mcp_client_close(s_mcp);
            clear_data();
            s_refresh_req = true;
        }
        if (!s_active && s_qlen == 0) {
            if (open) { mcp_client_close(s_mcp); open = false; }
            if (s->state != STOCKS_OFF) set_state(STOCKS_OFF, NULL);
            continue;
        }

        app_settings_t cfg;
        settings_get(&cfg);
        if (!url_usable(cfg.mcp_url)) {
            set_state(STOCKS_NOT_SET, "Set the server URL (gear button)");
            continue;
        }
        mcp_client_configure(s_mcp, cfg.mcp_url, STOCKS_KEY_HEADER, cfg.mcp_api_key);

        if (!net_is_up()) {
            set_state(STOCKS_WAIT_WIFI, "Waiting for Wi-Fi...");
            continue;
        }
        if (s->state == STOCKS_NOT_SET || s->state == STOCKS_WAIT_WIFI || s->state == STOCKS_OFF) {
            set_state(s->quotes_loaded ? STOCKS_OK : STOCKS_LOADING, "");
        }
        open = true;

        /* queued adds/removes first, one at a time; then one refresh to pick up the result */
        change_t c;
        bool changed = false;
        while (pop_change(&c)) {
            do_change(&c);
            changed = true;
        }
        if (changed) s_refresh_req = true;
        if (!s_active) continue;            /* finished the queue after the app closed */

        if (s_detail_req) fetch_detail();   /* the Quote screen is waiting: before the refresh */

        bool due = s->auto_refresh && s->next_us && esp_timer_get_time() >= s->next_us;
        if (s_refresh_req || due) refresh();
    }
}

/* ---------------------------------------------------------------- API (any task) */
void stocks_init(void)
{
    s = calloc(1, sizeof(*s));
    s_quotes = calloc(STOCKS_MAX, sizeof(stocks_quote_t));
    s_syms = calloc(STOCKS_MAX, sizeof(stocks_sym_t));
    if (!s || !s_quotes || !s_syms) {
        ESP_LOGE(TAG, "out of memory: Stocks disabled");
        return;
    }
    s_mtx = xSemaphoreCreateMutex();
    app_settings_t cfg;
    settings_get(&cfg);
    s->auto_refresh = cfg.stocks_auto;
    s->state = STOCKS_OFF;
    /* the stack can live in PSRAM: this task only does networking, never flash writes */
    if (xTaskCreatePinnedToCoreWithCaps(worker, "stocks", 10 * 1024, NULL, 4, &s_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        xTaskCreatePinnedToCore(worker, "stocks", 8 * 1024, NULL, 4, &s_task, 0);
    }
}

void stocks_set_active(bool active)
{
    if (!s_mtx) return;
    s_active = active;
    if (active) s_refresh_req = true;
    lock();
    s->toast[0] = '\0';
    unlock();
    wake();
}

void stocks_set_auto(bool on)
{
    if (!s_mtx) return;
    lock();
    s->auto_refresh = on;
    s->next_us = 0;
    unlock();
    if (on) s_refresh_req = true;
    wake();
}

void stocks_refresh_now(void)
{
    s_refresh_req = true;
    wake();
}

void stocks_config_changed(void)
{
    s_reset = true;
    wake();
}

static stocks_tap_t queue_change(const char *sym, int mode /* 0 toggle, 1 remove */)
{
    stocks_tap_t r;
    if (!s_mtx) return STOCKS_TAP_NOT_LOADED;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (mode == 0 && !s->list_loaded)                  r = STOCKS_TAP_NOT_LOADED;
    else if (find_sym(s->pend, s->npend, sym) >= 0)     r = STOCKS_TAP_PENDING;
    else if (s->npend >= STOCKS_PEND_MAX)              r = STOCKS_TAP_FULL;
    else {
        bool add = mode == 0 && find_sym(s->syms, s->nsyms, sym) < 0;
        change_t c = { .add = add };
        strlcpy(c.sym, sym, sizeof(c.sym));
        s_queue[s_qlen++] = c;
        strlcpy(s->pend[s->npend++], sym, sizeof(stocks_sym_t));
        r = add ? STOCKS_TAP_ADD : STOCKS_TAP_REMOVE;
    }
    s_ver++;
    xSemaphoreGive(s_mtx);
    if (r == STOCKS_TAP_ADD || r == STOCKS_TAP_REMOVE) wake();
    return r;
}

stocks_tap_t stocks_toggle(const char *sym) { return queue_change(sym, 0); }

void stocks_request_detail(const char *sym)
{
    if (!s_mtx) return;
    lock();
    if (strcmp(s->detail_sym, sym) != 0) {    /* another symbol: forget the old one */
        memset(&s->detail, 0, sizeof(s->detail));
        s->detail_us = 0;
        s->detail_at = 0;
        s->detail_note[0] = '\0';
        strlcpy(s->detail_sym, sym, sizeof(s->detail_sym));
    }
    s->detail_state = STOCKS_DETAIL_LOADING;
    unlock();
    s_detail_req = true;
    wake();
}
stocks_tap_t stocks_remove(const char *sym) { return queue_change(sym, 1); }
