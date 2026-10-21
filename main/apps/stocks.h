#pragma once
/* Stocks engine: talks to the FusionStockWatchListMCP server from its own task (no LVGL in here).
 * The Stocks screens poll stocks_version() / stocks_get() from LVGL timers, and taps only queue
 * work, so the screen never freezes while a request is running. */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "stocks_parse.h"

#define STOCKS_PEND_MAX 16          /* adds/removes waiting to be sent */

typedef enum {
    STOCKS_DETAIL_NONE,
    STOCKS_DETAIL_LOADING,
    STOCKS_DETAIL_OK,
    STOCKS_DETAIL_ERROR,      /* detail_note says why */
} stocks_detail_state_t;

typedef enum {
    STOCKS_OFF,           /* app closed */
    STOCKS_NOT_SET,       /* no usable server URL */
    STOCKS_WAIT_WIFI,
    STOCKS_LOADING,       /* first load since the app opened */
    STOCKS_OK,
    STOCKS_ERROR,         /* last request failed; details in note */
} stocks_state_t;

typedef struct {
    stocks_state_t state;
    bool     busy;                      /* a request is running */
    bool     auto_refresh;
    bool     list_loaded;               /* the symbol list is known: taps can add/remove */
    bool     quotes_loaded;

    int            nrows;               /* watchlist rows in server order; has_price false = no quote */
    stocks_quote_t rows[STOCKS_MAX];
    int            nsyms;               /* the server's symbol list (may include tickers it can't quote) */
    stocks_sym_t   syms[STOCKS_MAX];
    int            npend;               /* symbols with an add/remove queued or running */
    stocks_sym_t   pend[STOCKS_PEND_MAX];

    int64_t  updated_us;                /* esp_timer time of the last successful refresh, 0 = never */
    time_t   updated_at;                /* wall-clock time of it, 0 if the clock wasn't set */
    int64_t  next_us;                   /* next automatic refresh, 0 = none */
    char     note[100];                 /* error text for the status line */
    char     toast[80];                 /* result of the last add/remove */
    uint32_t toast_ver;

    /* detailed quote (GetStockQuote) for the symbol on the Quote screen */
    stocks_sym_t          detail_sym;
    stocks_detail_state_t detail_state;
    stocks_detail_t       detail;
    int64_t               detail_us;    /* when it arrived (esp_timer), 0 = never */
    time_t                detail_at;    /* wall-clock time of it, 0 if the clock wasn't set */
    char                  detail_note[100];
} stocks_snapshot_t;

typedef enum {
    STOCKS_TAP_ADD,         /* queued an add */
    STOCKS_TAP_REMOVE,      /* queued a remove */
    STOCKS_TAP_PENDING,     /* this symbol already has a request queued */
    STOCKS_TAP_NOT_LOADED,  /* the watchlist hasn't loaded yet */
    STOCKS_TAP_FULL,        /* too many requests queued */
} stocks_tap_t;

void         stocks_init(void);                /* once at boot */
void         stocks_set_active(bool active);   /* app opened (loads the list) / closed (disconnects) */
void         stocks_set_auto(bool on);         /* auto refresh every STOCKS_REFRESH_MS; on = refresh now */
bool         stocks_auto(void);
void         stocks_refresh_now(void);
void         stocks_config_changed(void);      /* server URL or API key changed: reconnect, reload */
stocks_tap_t stocks_toggle(const char *sym);   /* add if not in the list, remove if it is */
void         stocks_request_detail(const char *sym);   /* fetch a detailed quote (Quote screen) */
stocks_tap_t stocks_remove(const char *sym);

uint32_t     stocks_version(void);             /* changes whenever anything in the snapshot changes */
void         stocks_get(stocks_snapshot_t *out);
