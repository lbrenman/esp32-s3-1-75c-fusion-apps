#pragma once
/* Parsers for FusionStockWatchListMCP's tool replies. Pure C + cJSON: no FreeRTOS, no LVGL,
 * so tools/stocks_test can run them on a PC.
 *
 *   GetStockWatchList          {"Watchlist":[{"Symbol":"AAPL","Name":"Apple Inc","Price":341.07,
 *                                             "Change":5.15,"ChangePercent":1.5331}, ...]}
 *   GetStockWatchListSymbols   AAPL,INTC,TXN
 *   AddSymbolToStockWatchList  "Symbol AAPL successfully added" / "Symbol already exists"
 *   RemoveSymbol...            "Successfully removed AAPL from watch list" / "Error deleting ..."
 *
 * The server always answers isError: false, so add/remove results come from the message text.
 * Other JSON shapes (arrays, other key names, numbers as strings) are accepted too. */

#include <stdbool.h>
#include <stddef.h>

#define STOCKS_MAX      64          /* symbols in the watchlist that the app keeps */
#define STOCKS_SYM_LEN  8           /* longest ticker accepted */
#define STOCKS_NAME_LEN 40

typedef char stocks_sym_t[STOCKS_SYM_LEN + 1];

typedef struct {
    stocks_sym_t sym;
    char   name[STOCKS_NAME_LEN];
    double price, change, pct;
    bool   has_price, has_change, has_pct;
} stocks_quote_t;

/* GetStockQuote: {"Quote":{"Symbol":"AAPL","Name":"Apple Inc","Logo":"...","Price":341.07,
 *   "Change":5.15,"ChangePercent":1.53,"DayHigh":343.2,"DayLow":335.8,"OpenPrice":336.1,
 *   "PreviousClose":335.92}} */
typedef struct {
    stocks_quote_t q;
    double high, low, open, prev_close;
    double volume;
    bool   has_high, has_low, has_open, has_prev_close, has_volume;
} stocks_detail_t;

typedef enum {
    STOCKS_REPLY_OK,        /* added / removed */
    STOCKS_REPLY_EXISTS,    /* "already exists" */
    STOCKS_REPLY_ERROR,     /* "Error deleting ..." and other failure messages */
    STOCKS_REPLY_UNKNOWN,
} stocks_reply_t;

/* Return true on success (an empty list is a success). On failure err gets a short message. */
bool stocks_parse_quotes(const char *text, stocks_quote_t *out, int max, int *count, char *err, size_t err_len);
bool stocks_parse_symbols(const char *text, stocks_sym_t *out, int max, int *count, char *err, size_t err_len);
stocks_reply_t stocks_classify(const char *text);

/* One detailed quote. False (with err) on an error message, an unrecognized reply, or a quote of
 * all zeros (what the quote service returns for a ticker it doesn't know). */
bool stocks_parse_detail(const char *text, const char *sym, stocks_detail_t *out, char *err, size_t err_len);

/* "AAPL", "BRK.B": 1-8 chars, starts with a letter. Uppercases in place; false if not a ticker. */
bool stocks_normalize_ticker(char *s);
