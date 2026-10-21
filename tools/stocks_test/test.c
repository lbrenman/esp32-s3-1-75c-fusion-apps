/* Host tests for the Stocks app's reply parsing (main/apps/stocks_parse.c) and the MCP client's
 * reply handling (main/mcp_client.c, network code left out). Uses the replies
 * FusionStockWatchListMCP actually sends, plus the other shapes the parsers accept.
 *
 *   cd tools/stocks_test && make
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "cJSON.h"
#include "stocks_parse.h"
#include "mcp_client.h"

static int fails = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static void quotes(void)
{
    stocks_quote_t q[STOCKS_MAX];
    int n;
    char err[96];

    const char *real = "{\"Watchlist\":[{\"Symbol\":\"AAPL\",\"Name\":\"Apple Inc\",\"Price\":341.07,"
                       "\"Change\":5.15,\"ChangePercent\":1.5331},{\"Symbol\":\"INTC\",\"Name\":\"Intel Corp\","
                       "\"Price\":24.1,\"Change\":-0.42,\"ChangePercent\":-1.71}]}";
    CHECK(stocks_parse_quotes(real, q, STOCKS_MAX, &n, err, sizeof(err)), "real reply: %s", err);
    CHECK(n == 2, "real reply count %d", n);
    CHECK(!strcmp(q[0].sym, "AAPL") && !strcmp(q[0].name, "Apple Inc"), "AAPL fields");
    CHECK(q[0].has_price && fabs(q[0].price - 341.07) < 1e-9, "AAPL price");
    CHECK(q[1].has_change && q[1].change < 0 && q[1].has_pct, "INTC change");

    CHECK(stocks_parse_quotes("{\"Watchlist\":[]}", q, STOCKS_MAX, &n, err, sizeof(err)) && n == 0, "empty array");
    CHECK(stocks_parse_quotes("{\"Watchlist\":null}", q, STOCKS_MAX, &n, err, sizeof(err)) && n == 0, "null list");
    CHECK(stocks_parse_quotes("", q, STOCKS_MAX, &n, err, sizeof(err)) && n == 0, "blank");
    CHECK(stocks_parse_quotes("null", q, STOCKS_MAX, &n, err, sizeof(err)) && n == 0, "null");
    CHECK(stocks_parse_quotes("No symbols in the watch list", q, STOCKS_MAX, &n, err, sizeof(err)) && n == 0, "message");
    CHECK(!stocks_parse_quotes("Error getting quotes", q, STOCKS_MAX, &n, err, sizeof(err)), "error text");
    CHECK(!strcmp(err, "Error getting quotes"), "error message kept: %s", err);

    /* encoded twice, other key names, numbers as strings, pct derived */
    const char *twice = "\"{\\\"watch_list\\\":[{\\\"ticker\\\":\\\"msft\\\",\\\"last_price\\\":\\\"$1,012.50\\\","
                        "\\\"net_change\\\":\\\"+12.50\\\"}]}\"";
    CHECK(stocks_parse_quotes(twice, q, STOCKS_MAX, &n, err, sizeof(err)), "twice: %s", err);
    CHECK(n == 1 && !strcmp(q[0].sym, "MSFT"), "twice sym %d %s", n, n ? q[0].sym : "");
    CHECK(n == 1 && fabs(q[0].price - 1012.5) < 1e-9 && fabs(q[0].change - 12.5) < 1e-9, "twice numbers");
    CHECK(n == 1 && q[0].has_pct && fabs(q[0].pct - 1.25) < 1e-9, "derived pct %f", q[0].pct);

    /* plain array, single object */
    CHECK(stocks_parse_quotes("[{\"symbol\":\"NVDA\",\"price\":180}]", q, STOCKS_MAX, &n, err, sizeof(err)) && n == 1,
          "array");
    CHECK(stocks_parse_quotes("{\"symbol\":\"AMD\",\"price\":150,\"dp\":2}", q, STOCKS_MAX, &n, err, sizeof(err))
          && n == 1 && q[0].has_change, "single + derived change");
    CHECK(!stocks_parse_quotes("{\"foo\":1}", q, STOCKS_MAX, &n, err, sizeof(err)), "unrecognized");

    /* max respected */
    CHECK(stocks_parse_quotes(real, q, 1, &n, err, sizeof(err)) && n == 1, "max");
}

static void symbols(void)
{
    stocks_sym_t s[STOCKS_MAX];
    int n;
    char err[96];
    CHECK(stocks_parse_symbols("AAPL,INTC,TXN", s, STOCKS_MAX, &n, err, sizeof(err)) && n == 3, "real list");
    CHECK(!strcmp(s[2], "TXN"), "third %s", s[2]);
    CHECK(stocks_parse_symbols("AAPL, INTC ,  TXN\n", s, STOCKS_MAX, &n, err, sizeof(err)) && n == 3, "spaces");
    CHECK(stocks_parse_symbols("aapl", s, STOCKS_MAX, &n, err, sizeof(err)) && n == 1 && !strcmp(s[0], "AAPL"), "lower one");
    CHECK(stocks_parse_symbols("aapl,msft", s, STOCKS_MAX, &n, err, sizeof(err)) && n == 2, "lower list");
    CHECK(stocks_parse_symbols("", s, STOCKS_MAX, &n, err, sizeof(err)) && n == 0, "blank");
    CHECK(stocks_parse_symbols("No symbols found", s, STOCKS_MAX, &n, err, sizeof(err)) && n == 0, "message = empty");
    CHECK(!stocks_parse_symbols("Error reading watch list", s, STOCKS_MAX, &n, err, sizeof(err)), "error");
    CHECK(stocks_parse_symbols("[\"aapl\",\"ZZZZZ\"]", s, STOCKS_MAX, &n, err, sizeof(err)) && n == 2, "json array");
    CHECK(stocks_parse_symbols("{\"Watchlist\":[{\"Symbol\":\"AAPL\",\"Price\":1}]}", s, STOCKS_MAX, &n, err,
                               sizeof(err)) && n == 1, "watchlist object");
    CHECK(stocks_parse_symbols("\"AAPL,MSFT\"", s, STOCKS_MAX, &n, err, sizeof(err)) && n == 2, "json string");
}

static void classify(void)
{
    CHECK(stocks_classify("Symbol AAPL successfully added") == STOCKS_REPLY_OK, "added");
    CHECK(stocks_classify("Symbol already exists") == STOCKS_REPLY_EXISTS, "exists");
    CHECK(stocks_classify("Successfully removed MSFT from watch list") == STOCKS_REPLY_OK, "removed");
    CHECK(stocks_classify("Error deleting symbol from watch list") == STOCKS_REPLY_ERROR, "error");
    CHECK(stocks_classify("hmm") == STOCKS_REPLY_UNKNOWN, "unknown");
    char t[16] = "brk.b";
    CHECK(stocks_normalize_ticker(t) && !strcmp(t, "BRK.B"), "ticker dot");
    char u[16] = "1ABC";
    CHECK(!stocks_normalize_ticker(u), "ticker digit first");
}

static void detail(void)
{
    stocks_detail_t d;
    char err[96];
    const char *real = "{\"Quote\":{\"Symbol\":\"AAPL\",\"Name\":\"Apple Inc\",\"Logo\":\"https://x/aapl.png\","
                       "\"ChangePercent\":1.5331,\"Price\":341.07,\"Change\":5.15,\"DayHigh\":343.2,"
                       "\"DayLow\":335.8,\"OpenPrice\":336.1,\"PreviousClose\":335.92}}";
    CHECK(stocks_parse_detail(real, "AAPL", &d, err, sizeof(err)), "real detail: %s", err);
    CHECK(!strcmp(d.q.sym, "AAPL") && !strcmp(d.q.name, "Apple Inc"), "detail sym/name");
    CHECK(d.has_high && fabs(d.high - 343.2) < 1e-9 && d.has_low && fabs(d.low - 335.8) < 1e-9, "high/low");
    CHECK(d.has_open && d.has_prev_close && fabs(d.prev_close - 335.92) < 1e-9, "open/prev");
    CHECK(d.q.has_change && d.q.has_pct && !d.has_volume, "change/pct");

    const char *zeros = "{\"Quote\":{\"Symbol\":\"ZZZZZ\",\"Price\":0,\"Change\":null,\"PreviousClose\":0}}";
    CHECK(!stocks_parse_detail(zeros, "ZZZZZ", &d, err, sizeof(err)) && !strcmp(err, "No quote for ZZZZZ"), "zeros: %s", err);
    CHECK(!stocks_parse_detail("Error getting quote", "AAPL", &d, err, sizeof(err)) && !strcmp(err, "Error getting quote"), "error");
    CHECK(!stocks_parse_detail("", "AAPL", &d, err, sizeof(err)), "blank");

    /* encoded twice, no symbol field, Finnhub-style keys */
    const char *fh = "\"{\\\"c\\\":180.5,\\\"d\\\":-1.2,\\\"dp\\\":-0.66,\\\"h\\\":182,\\\"l\\\":179,"
                     "\\\"o\\\":181,\\\"pc\\\":181.7}\"";
    CHECK(stocks_parse_detail(fh, "NVDA", &d, err, sizeof(err)), "finnhub: %s", err);
    CHECK(!strcmp(d.q.sym, "NVDA") && fabs(d.q.price - 180.5) < 1e-9 && d.has_low && d.has_prev_close, "finnhub fields");

    const char *list = "[{\"symbol\":\"MSFT\",\"price\":1},{\"symbol\":\"AAPL\",\"price\":2,\"high\":3}]";
    CHECK(stocks_parse_detail(list, "AAPL", &d, err, sizeof(err)) && fabs(d.q.price - 2) < 1e-9 && d.has_high, "list match");
}

static void mcp(void)
{
    const char *sse = "event: message\r\ndata: {\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"content\":[{\"type\":\"text\","
                      "\"text\":\"AAPL,INTC\"}],\"isError\":false}}\r\n\r\n";
    cJSON *m = mcp_find_reply(sse, 3);
    CHECK(m != NULL, "sse reply found");
    bool is_err = true;
    char *t = m ? mcp_result_text(cJSON_GetObjectItem(m, "result"), &is_err) : NULL;
    CHECK(t && !strcmp(t, "AAPL,INTC") && !is_err, "sse text %s", t ? t : "(null)");
    free(t);
    cJSON_Delete(m);

    CHECK(mcp_find_reply(sse, 4) == NULL, "wrong id");

    const char *two = "event: message\ndata: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\"}\n\n"
                      "event: message\ndata: {\"jsonrpc\":\"2.0\",\n"
                      "data: \"id\":7,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"a\"},"
                      "{\"type\":\"text\",\"text\":\"b\"}],\"isError\":true}}";
    m = mcp_find_reply(two, 7);
    CHECK(m != NULL, "multi-line data, no trailing blank line");
    t = m ? mcp_result_text(cJSON_GetObjectItem(m, "result"), &is_err) : NULL;
    CHECK(t && !strcmp(t, "a\nb") && is_err, "joined text + isError");
    free(t);
    cJSON_Delete(m);

    m = mcp_find_reply(" {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"structuredContent\":{\"x\":1}}}", 1);
    t = m ? mcp_result_text(cJSON_GetObjectItem(m, "result"), &is_err) : NULL;
    CHECK(t && !strcmp(t, "{\"x\":1}"), "plain json + structuredContent: %s", t ? t : "(null)");
    free(t);
    cJSON_Delete(m);

    m = mcp_find_reply("[{\"id\":1,\"result\":{}},{\"id\":2,\"error\":{\"message\":\"nope\"}}]", 2);
    CHECK(m && cJSON_GetObjectItem(m, "error"), "batch array");
    cJSON_Delete(m);
    CHECK(mcp_find_reply("", 1) == NULL && mcp_find_reply("garbage", 1) == NULL, "junk");
}

int main(void)
{
    quotes();
    symbols();
    classify();
    detail();
    mcp();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
