/* Parsers for FusionStockWatchListMCP's tool replies. See stocks_parse.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include "cJSON.h"
#include "stocks_parse.h"

/* ----------------------------------------------------------------- key matching */
static const char *const K_SYMBOL[] = { "symbol", "ticker", "sym", NULL };
static const char *const K_NAME[]   = { "name", "companyname", "shortname", "longname", NULL };
static const char *const K_PRICE[]  = { "price", "currentprice", "current", "last", "lastprice",
                                        "latestprice", "regularmarketprice", "c", "close", NULL };
static const char *const K_CHANGE[] = { "change", "pricechange", "netchange", "changeamount",
                                        "dailychange", "regularmarketchange", "d", NULL };
static const char *const K_PCT[]    = { "changepercent", "percentchange", "changepct", "pctchange",
                                        "changespercentage", "changepercentage", "percentagechange",
                                        "regularmarketchangepercent", "percent", "dp", NULL };

/* "Change_Percent" -> "changepercent" */
static void norm_key(const char *in, char *out, size_t len)
{
    size_t n = 0;
    for (; *in && n + 1 < len; in++) {
        if (*in == '_' || *in == '-' || *in == ' ') continue;
        out[n++] = (char)tolower((unsigned char)*in);
    }
    out[n] = '\0';
}

static const cJSON *pick_direct(const cJSON *obj, const char *const *keys)
{
    char nk[48];
    for (int k = 0; keys[k]; k++) {
        const cJSON *it;
        cJSON_ArrayForEach(it, obj) {
            if (!it->string || cJSON_IsNull(it)) continue;
            norm_key(it->string, nk, sizeof(nk));
            if (strcmp(nk, keys[k]) == 0) return it;
        }
    }
    return NULL;
}

/* First value whose key matches (ignoring case, '_', '-', ' '); also one level into nested objects. */
static const cJSON *pick(const cJSON *obj, const char *const *keys)
{
    if (!cJSON_IsObject(obj)) return NULL;
    const cJSON *v = pick_direct(obj, keys);
    if (v) return v;
    const cJSON *it;
    cJSON_ArrayForEach(it, obj) {
        if (cJSON_IsObject(it) && (v = pick_direct(it, keys))) return v;
    }
    return NULL;
}

static bool to_double(const cJSON *v, double *out)
{
    if (cJSON_IsNumber(v)) { *out = v->valuedouble; return true; }
    if (!cJSON_IsString(v)) return false;
    char buf[32];
    size_t n = 0;
    for (const char *p = v->valuestring; *p && n + 1 < sizeof(buf); p++) {
        if (*p == '$' || *p == ',' || *p == '%' || *p == ' ' || *p == '+') continue;
        buf[n++] = *p;
    }
    buf[n] = '\0';
    if (!n) return false;
    char *end;
    double d = strtod(buf, &end);
    if (*end) return false;
    *out = d;
    return true;
}

/* ----------------------------------------------------------------- helpers */
bool stocks_normalize_ticker(char *s)
{
    size_t n = strlen(s);
    if (n < 1 || n > STOCKS_SYM_LEN) return false;
    for (size_t i = 0; i < n; i++) s[i] = (char)toupper((unsigned char)s[i]);
    if (s[0] < 'A' || s[0] > 'Z') return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    }
    return true;
}

static bool contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        if (strncasecmp(hay, needle, n) == 0) return true;
    }
    return false;
}

static bool looks_like_error(const char *t)
{
    static const char *const words[] = { "error", "fail", "exception", "invalid", "unauthorized",
                                         "not found", NULL };
    for (int i = 0; words[i]; i++) if (contains_ci(t, words[i])) return true;
    return false;
}

static bool is_blank(const char *t)
{
    while (*t && isspace((unsigned char)*t)) t++;
    if (!*t) return true;
    char buf[8];
    size_t n = 0;
    for (; *t && n + 1 < sizeof(buf); t++) if (!isspace((unsigned char)*t)) buf[n++] = (char)tolower((unsigned char)*t);
    buf[n] = '\0';
    if (*t) return false;
    return !strcmp(buf, "null") || !strcmp(buf, "[]") || !strcmp(buf, "{}") || !strcmp(buf, "\"\"");
}

static void copy_msg(const char *text, char *err, size_t err_len)
{
    while (*text && isspace((unsigned char)*text)) text++;
    snprintf(err, err_len, "%s", text);
    size_t n = strlen(err);
    while (n && isspace((unsigned char)err[n - 1])) err[--n] = '\0';
}

/* Parses JSON; if the result is a JSON string that itself holds JSON (encoded twice), parses that.
 * Returns NULL if text isn't JSON. */
static cJSON *parse_json(const char *text)
{
    const char *p = text;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '{' && *p != '[' && *p != '"') return NULL;
    cJSON *root = cJSON_Parse(p);
    if (cJSON_IsString(root)) {
        const char *s = root->valuestring;
        while (*s && isspace((unsigned char)*s)) s++;
        if (*s == '{' || *s == '[') {
            cJSON *inner = cJSON_Parse(s);
            if (inner) { cJSON_Delete(root); root = inner; }
        }
    }
    return root;
}

static const cJSON *watchlist_item(const cJSON *root)
{
    if (!cJSON_IsObject(root)) return NULL;
    const cJSON *it;
    cJSON_ArrayForEach(it, root) {
        if (it->string && strcasecmp(it->string, "watchlist") == 0) return it;
    }
    return NULL;
}

/* The list of per-symbol records anywhere in a reply: an array of objects with a symbol key,
 * or a single object with a symbol and a price. */
static const cJSON *find_records(const cJSON *node, int depth)
{
    if (!node || depth > 4) return NULL;
    if (cJSON_IsArray(node)) {
        const cJSON *it;
        cJSON_ArrayForEach(it, node) {
            if (cJSON_IsObject(it) && pick(it, K_SYMBOL)) return node;
        }
        cJSON_ArrayForEach(it, node) {
            const cJSON *r = find_records(it, depth + 1);
            if (r) return r;
        }
        return NULL;
    }
    if (cJSON_IsObject(node)) {
        if (pick(node, K_SYMBOL) && pick(node, K_PRICE)) return node;
        const cJSON *it;
        cJSON_ArrayForEach(it, node) {
            const cJSON *r = find_records(it, depth + 1);
            if (r) return r;
        }
    }
    return NULL;
}

static bool record_symbol(const cJSON *rec, stocks_sym_t out)
{
    const cJSON *s = pick(rec, K_SYMBOL);
    if (!cJSON_IsString(s)) return false;
    char buf[32];
    snprintf(buf, sizeof(buf), "%s", s->valuestring);
    char *b = buf;
    while (*b == ' ') b++;
    size_t n = strlen(b);
    while (n && b[n - 1] == ' ') b[--n] = '\0';
    if (!stocks_normalize_ticker(b)) return false;
    strcpy(out, b);
    return true;
}

static bool add_quote(const cJSON *rec, stocks_quote_t *out, int max, int *count)
{
    if (*count >= max || !cJSON_IsObject(rec)) return false;
    stocks_quote_t q;
    memset(&q, 0, sizeof(q));
    if (!record_symbol(rec, q.sym)) return false;
    const cJSON *name = pick(rec, K_NAME);
    if (cJSON_IsString(name)) snprintf(q.name, sizeof(q.name), "%s", name->valuestring);
    q.has_price  = to_double(pick(rec, K_PRICE), &q.price);
    q.has_change = to_double(pick(rec, K_CHANGE), &q.change);
    q.has_pct    = to_double(pick(rec, K_PCT), &q.pct);
    if (!q.has_pct && q.has_change && q.has_price && q.price - q.change != 0) {
        q.pct = q.change / (q.price - q.change) * 100.0;
        q.has_pct = true;
    }
    if (!q.has_change && q.has_pct && q.has_price && 1.0 + q.pct / 100.0 != 0) {
        q.change = q.price - q.price / (1.0 + q.pct / 100.0);
        q.has_change = true;
    }
    out[(*count)++] = q;
    return true;
}

static void add_records(const cJSON *recs, stocks_quote_t *out, int max, int *count)
{
    if (cJSON_IsArray(recs)) {
        const cJSON *it;
        cJSON_ArrayForEach(it, recs) add_quote(it, out, max, count);
    } else {
        add_quote(recs, out, max, count);
    }
}

/* ----------------------------------------------------------------- quotes */
bool stocks_parse_quotes(const char *text, stocks_quote_t *out, int max, int *count, char *err, size_t err_len)
{
    *count = 0;
    err[0] = '\0';
    if (!text || is_blank(text)) return true;

    cJSON *root = parse_json(text);
    if (!root || cJSON_IsString(root)) {
        const char *msg = cJSON_IsString(root) ? root->valuestring : text;
        bool ok = !looks_like_error(msg);       /* e.g. "No symbols in the watch list" = empty */
        if (!ok) copy_msg(msg, err, err_len);
        cJSON_Delete(root);
        return ok;
    }

    bool ok = true;
    const cJSON *wl = watchlist_item(root);
    if (wl) {
        if (cJSON_IsArray(wl)) add_records(wl, out, max, count);
        else if (!cJSON_IsNull(wl)) { snprintf(err, err_len, "Unrecognized quotes reply"); ok = false; }
    } else if (cJSON_IsArray(root) && cJSON_GetArraySize(root) == 0) {
        /* empty */
    } else {
        const cJSON *recs = find_records(root, 0);
        if (recs) add_records(recs, out, max, count);
        else { snprintf(err, err_len, "Unrecognized quotes reply"); ok = false; }
    }
    cJSON_Delete(root);
    return ok;
}

/* ----------------------------------------------------------------- symbols */
static void add_symbol(const char *s, stocks_sym_t *out, int max, int *count)
{
    if (*count >= max) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%s", s);
    if (!stocks_normalize_ticker(buf)) return;
    strcpy(out[(*count)++], buf);
}

static bool parse_symbol_text(const char *text, stocks_sym_t *out, int max, int *count, char *err, size_t err_len)
{
    /* "AAPL,INTC,TXN" (commas and/or spaces) */
    bool has_lower = false, has_comma = strchr(text, ',') != NULL;
    for (const char *p = text; *p; p++) if (islower((unsigned char)*p)) has_lower = true;

    char tok[32];
    int ntok = 0, nbad = 0;
    stocks_sym_t tmp[STOCKS_MAX];
    int ntmp = 0;
    const char *p = text;
    while (*p) {
        while (*p && (*p == ',' || isspace((unsigned char)*p))) p++;
        if (!*p) break;
        size_t n = 0;
        while (*p && *p != ',' && !isspace((unsigned char)*p)) {
            if (n + 1 < sizeof(tok)) tok[n++] = *p;
            p++;
        }
        tok[n] = '\0';
        ntok++;
        if (stocks_normalize_ticker(tok)) { if (ntmp < STOCKS_MAX) strcpy(tmp[ntmp++], tok); }
        else nbad++;
    }
    /* A sentence such as "No symbols found" isn't a list: lowercase words without commas. */
    bool is_list = ntok > 0 && nbad == 0 && (has_comma || ntok == 1 || !has_lower);
    if (is_list) {
        for (int i = 0; i < ntmp && *count < max; i++) strcpy(out[(*count)++], tmp[i]);
        return true;
    }
    if (looks_like_error(text)) {
        copy_msg(text, err, err_len);
        return false;
    }
    return true;                           /* some other message: an empty list */
}

bool stocks_parse_symbols(const char *text, stocks_sym_t *out, int max, int *count, char *err, size_t err_len)
{
    *count = 0;
    err[0] = '\0';
    if (!text || is_blank(text)) return true;

    cJSON *root = parse_json(text);
    if (!root) return parse_symbol_text(text, out, max, count, err, err_len);
    if (cJSON_IsString(root)) {
        bool ok = parse_symbol_text(root->valuestring, out, max, count, err, err_len);
        cJSON_Delete(root);
        return ok;
    }

    bool ok = true;
    const cJSON *it;
    const cJSON *wl = watchlist_item(root);
    if (wl) {
        cJSON_ArrayForEach(it, wl) {
            stocks_sym_t s;
            if (cJSON_IsString(it)) add_symbol(it->valuestring, out, max, count);
            else if (record_symbol(it, s)) add_symbol(s, out, max, count);
        }
    } else if (cJSON_IsArray(root)) {
        cJSON_ArrayForEach(it, root) {
            stocks_sym_t s;
            if (cJSON_IsString(it)) add_symbol(it->valuestring, out, max, count);
            else if (record_symbol(it, s)) add_symbol(s, out, max, count);
        }
    } else {
        const cJSON *recs = find_records(root, 0);
        if (cJSON_IsArray(recs)) {
            cJSON_ArrayForEach(it, recs) {
                stocks_sym_t s;
                if (record_symbol(it, s)) add_symbol(s, out, max, count);
            }
        } else if (recs) {
            stocks_sym_t s;
            if (record_symbol(recs, s)) add_symbol(s, out, max, count);
        } else {
            snprintf(err, err_len, "Unrecognized symbols reply");
            ok = false;
        }
    }
    cJSON_Delete(root);
    return ok;
}

/* ----------------------------------------------------------------- add / remove */
stocks_reply_t stocks_classify(const char *text)
{
    if (!text) return STOCKS_REPLY_UNKNOWN;
    if (contains_ci(text, "already")) return STOCKS_REPLY_EXISTS;
    if (looks_like_error(text))       return STOCKS_REPLY_ERROR;
    if (contains_ci(text, "success") || contains_ci(text, "added") || contains_ci(text, "removed")) {
        return STOCKS_REPLY_OK;
    }
    return STOCKS_REPLY_UNKNOWN;
}
