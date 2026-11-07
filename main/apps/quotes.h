#pragma once
/* Quotes engine: talks to the Stock API (plain REST over HTTPS) from its own task; no LVGL in here.
 *
 *   GET <base>/quote?symbol=AAPL            one detailed quote (Quote screen)
 *   GET <base>/watchlist?symbols=AAPL,INTC  short quotes for several symbols (Watchlist screen)
 *
 * The API key goes in the x-api-key header. The Quotes screens poll quotes_version() /
 * quotes_get() from LVGL timers, and taps only queue work, so the screen never freezes while a
 * request is running. Replies are parsed with the Stocks app's parsers (stocks_parse.c): the
 * Stock API answers with the same {"Quote":{...}} and {"Watchlist":[...]} shapes. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "stocks_parse.h"
#include "app_config.h"

typedef enum {
    QT_IDLE,          /* nothing asked for yet */
    QT_LOADING,       /* a request is queued or running */
    QT_OK,
    QT_ERROR,         /* note says why */
} qt_state_t;

typedef enum {
    QT_LINK_OFF,      /* app closed */
    QT_LINK_NOT_SET,  /* no usable base URL */
    QT_LINK_WAIT_WIFI,
    QT_LINK_READY,    /* configured and Wi-Fi up (says nothing about the last request) */
} qt_link_t;

typedef struct {
    qt_link_t  link;
    bool       busy;                        /* a request is running right now */

    /* last request of any kind, for the Stock API screen */
    bool       last_ok;
    int        last_http;                   /* HTTP status, 0 = no answer */
    int64_t    last_us;                     /* esp_timer time, 0 = none yet */
    char       last_note[100];

    /* one detailed quote (GET /quote) */
    stocks_sym_t    q_sym;
    qt_state_t      q_state;
    stocks_detail_t q;
    int64_t         q_us;                   /* when it arrived (esp_timer), 0 = never */
    time_t          q_at;                   /* wall-clock time of it, 0 if the clock wasn't set */
    char            q_note[100];

    /* the watchlist (GET /watchlist): rows in the order the symbols were picked */
    int             wl_n;
    stocks_sym_t    wl_syms[QT_SEL_MAX];    /* what was asked for */
    stocks_quote_t  wl_rows[QT_SEL_MAX];    /* has_price false = no quote for that symbol */
    qt_state_t      wl_state;
    bool            wl_loaded;              /* rows hold a successful answer for wl_syms */
    int64_t         wl_us;
    time_t          wl_at;
    char            wl_note[100];
} qt_snapshot_t;

void     quotes_init(void);                  /* once at boot */
void     quotes_set_active(bool active);     /* app opened / closed (closes the connection) */
void     quotes_config_changed(void);        /* base URL or API key changed: new connection */
void     quotes_request_quote(const char *sym);
/* Asks for the watchlist of these symbols (up to QT_SEL_MAX). A different list than last time
 * clears the rows first. */
void     quotes_request_watchlist(const stocks_sym_t *syms, int n);

uint32_t quotes_version(void);               /* changes whenever anything in the snapshot changes */
void     quotes_get(qt_snapshot_t *out);

/* "https://host:4443/stock2api/" + "/quote" -> "https://host:4443/stock2api/quote".
 * A base URL that already ends in /quote or /watchlist (pasted from a test tool) is trimmed first,
 * and anything after '?' is dropped. Pure string work: the screens use it too. */
#define QT_ENDPOINT_MAX 288          /* room for any base URL (SET_URL_MAX) + a path */
static inline void quotes_endpoint(const char *base, const char *path, char *out, size_t out_len)
{
    char b[QT_ENDPOINT_MAX - 32];
    snprintf(b, sizeof(b), "%s", base);
    char *q = strchr(b, '?');
    if (q) *q = '\0';
    size_t n = strlen(b);
    while (n > 0 && b[n - 1] == '/') b[--n] = '\0';
    static const char *const tails[] = { QT_PATH_QUOTE, QT_PATH_WATCHLIST };
    for (int i = 0; i < 2; i++) {
        size_t t = strlen(tails[i]);
        if (n >= t && strcmp(b + n - t, tails[i]) == 0) { b[n - t] = '\0'; n -= t; break; }
    }
    snprintf(out, out_len, "%s%s", b, path);
}

/* "AAPL,intc, TXN" -> up to max upper-cased, unique tickers (letters, digits, '.', '-'; starting
 * with a letter; others are skipped). Returns how many. */
static inline int quotes_split_symbols(const char *list, stocks_sym_t *out, int max)
{
    int n = 0;
    const char *p = list;
    while (*p && n < max) {
        while (*p == ',' || *p == ' ') p++;
        stocks_sym_t t;
        size_t len = 0;
        bool ok = true;
        for (; *p && *p != ','; p++) {
            char c = *p;
            if (c == ' ') continue;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) ok = false;
            if (len < STOCKS_SYM_LEN) t[len++] = c; else ok = false;
        }
        t[len] = '\0';
        if (!ok || len == 0 || t[0] < 'A' || t[0] > 'Z') continue;
        bool dup = false;
        for (int i = 0; i < n; i++) if (strcmp(out[i], t) == 0) dup = true;
        if (!dup) memcpy(out[n++], t, sizeof(stocks_sym_t));
    }
    return n;
}
