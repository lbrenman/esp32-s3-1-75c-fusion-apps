/* A small MCP client for "Streamable HTTP" servers. See mcp_client.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "mcp_client.h"

/* ============================================================== reply parsing (no network) */

static bool id_matches(const cJSON *msg, int id)
{
    const cJSON *jid = cJSON_GetObjectItemCaseSensitive(msg, "id");
    if (cJSON_IsNumber(jid)) return (int)jid->valuedouble == id;
    if (cJSON_IsString(jid)) return atoi(jid->valuestring) == id;
    return false;
}

/* Takes ownership of parsed; returns the matching message (detached) or NULL. */
static cJSON *pick_message(cJSON *parsed, int id)
{
    if (!parsed) return NULL;
    if (cJSON_IsObject(parsed)) {
        if (id_matches(parsed, id)) return parsed;
        cJSON_Delete(parsed);
        return NULL;
    }
    if (cJSON_IsArray(parsed)) {
        int n = cJSON_GetArraySize(parsed);
        for (int i = 0; i < n; i++) {
            if (id_matches(cJSON_GetArrayItem(parsed, i), id)) {
                cJSON *m = cJSON_DetachItemFromArray(parsed, i);
                cJSON_Delete(parsed);
                return m;
            }
        }
    }
    cJSON_Delete(parsed);
    return NULL;
}

cJSON *mcp_find_reply(const char *body, int id)
{
    if (!body) return NULL;
    const char *p = body;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) return NULL;
    if (*p == '{' || *p == '[') return pick_message(cJSON_Parse(p), id);

    /* Server-Sent Events: "event: message\ndata: {...}\n\n". An event's data may span several
     * "data:" lines, which are joined with "\n". */
    size_t cap = strlen(body) + 1;
    char *data = malloc(cap);
    if (!data) return NULL;
    size_t dlen = 0;
    cJSON *found = NULL;
    const char *line = body;
    for (;;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len && line[len - 1] == '\r') len--;
        if (len >= 5 && strncmp(line, "data:", 5) == 0) {
            const char *d = line + 5;
            size_t dl = len - 5;
            if (dl && *d == ' ') { d++; dl--; }
            if (dlen) data[dlen++] = '\n';
            memcpy(data + dlen, d, dl);
            dlen += dl;
        } else if (len == 0 || !end) {
            if (dlen) {                       /* end of an event */
                data[dlen] = '\0';
                found = pick_message(cJSON_Parse(data), id);
                dlen = 0;
                if (found) break;
            }
        }
        if (!end) break;
        line = end + 1;
    }
    if (!found && dlen) {
        data[dlen] = '\0';
        found = pick_message(cJSON_Parse(data), id);
    }
    free(data);
    return found;
}

char *mcp_result_text(const cJSON *result, bool *is_error)
{
    if (is_error) *is_error = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError"));
    size_t len = 0;
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
    const cJSON *item;
    cJSON_ArrayForEach(item, content) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(item, "text");
        if (cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0 && cJSON_IsString(text)) {
            len += strlen(text->valuestring) + 1;
        }
    }
    if (len == 0) {
        const cJSON *sc = cJSON_GetObjectItemCaseSensitive(result, "structuredContent");
        if (sc && !cJSON_IsNull(sc)) return cJSON_PrintUnformatted(sc);
        return strdup("");
    }
    char *out = malloc(len + 1);
    if (!out) return NULL;
    out[0] = '\0';
    bool first = true;
    cJSON_ArrayForEach(item, content) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(item, "text");
        if (cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0 && cJSON_IsString(text)) {
            if (!first) strcat(out, "\n");
            strcat(out, text->valuestring);
            first = false;
        }
    }
    return out;
}

#ifndef MCP_HOST_TEST
/* ============================================================================ network */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "settings_store.h"
#include "app_config.h"

#define TAG "mcp"

#define MCP_PROTOCOL      "2025-03-26"
#define MCP_TIMEOUT_MS    15000
#define MCP_MAX_BODY      (96 * 1024)      /* a reply bigger than this is an error */
/* Reuse the TLS connection only for calls close together (a refresh, the first load). After a
 * longer pause the server or a proxy may have closed it without us noticing, so start fresh. */
#define MCP_IDLE_CLOSE_MS 30000

/* The certificate chain fetched at build time from WS_HOST (main/server_ca.pem). */
extern const char server_ca_pem_start[] asm("_binary_server_ca_pem_start");

struct mcp_client {
    char url[SET_URL_MAX + 1];
    char host[96];
    char key_header[32];
    char api_key[SET_KEY_MAX + 1];
    char name[32], version[16];

    esp_http_client_handle_t http;
    int64_t last_used_us;
    char session[128];
    char protocol[24];
    bool ready;                  /* initialize done */
    int  rid;

    char  *body;                 /* reply body (PSRAM) */
    size_t body_len, body_cap;
    bool   body_overflow;
    char   hdr_session[128];
    int    status;
};

static void url_host(const char *url, char *out, size_t out_len)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t n = strcspn(p, ":/?#");
    if (n >= out_len) n = out_len - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

mcp_client_t *mcp_client_create(const char *client_name, const char *client_version)
{
    mcp_client_t *c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c) c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    strlcpy(c->name, client_name, sizeof(c->name));
    strlcpy(c->version, client_version, sizeof(c->version));
    return c;
}

static void http_close(mcp_client_t *c)
{
    if (c->http) {
        esp_http_client_cleanup(c->http);
        c->http = NULL;
    }
}

void mcp_client_close(mcp_client_t *c)
{
    if (!c) return;
    if (c->http) ESP_LOGI(TAG, "closing connection to %s", c->host);
    http_close(c);
    c->ready = false;
    c->session[0] = '\0';
    c->protocol[0] = '\0';
}

void mcp_client_configure(mcp_client_t *c, const char *url, const char *key_header, const char *api_key)
{
    if (strcmp(c->url, url) == 0 && strcmp(c->key_header, key_header) == 0 && strcmp(c->api_key, api_key) == 0) {
        return;
    }
    mcp_client_close(c);
    strlcpy(c->url, url, sizeof(c->url));
    strlcpy(c->key_header, key_header, sizeof(c->key_header));
    strlcpy(c->api_key, api_key, sizeof(c->api_key));
    url_host(url, c->host, sizeof(c->host));
}

int mcp_last_http_status(const mcp_client_t *c) { return c ? c->status : 0; }

static esp_err_t http_event(esp_http_client_event_t *e)
{
    mcp_client_t *c = e->user_data;
    switch (e->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (e->header_key && e->header_value && strcasecmp(e->header_key, "Mcp-Session-Id") == 0) {
            strlcpy(c->hdr_session, e->header_value, sizeof(c->hdr_session));
        }
        break;
    case HTTP_EVENT_ON_DATA:
        if (c->body_overflow || e->data_len <= 0) break;
        if (c->body_len + e->data_len + 1 > MCP_MAX_BODY) {
            c->body_overflow = true;
            break;
        }
        if (c->body_len + e->data_len + 1 > c->body_cap) {
            size_t cap = c->body_cap ? c->body_cap : 4096;
            while (cap < c->body_len + e->data_len + 1) cap *= 2;
            char *nb = heap_caps_realloc(c->body, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!nb) { c->body_overflow = true; break; }
            c->body = nb;
            c->body_cap = cap;
        }
        memcpy(c->body + c->body_len, e->data, e->data_len);
        c->body_len += e->data_len;
        c->body[c->body_len] = '\0';
        break;
    default:
        break;
    }
    return ESP_OK;
}

static bool http_open(mcp_client_t *c)
{
    if (c->http) return true;
    esp_http_client_config_t cfg = {
        .url = c->url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = MCP_TIMEOUT_MS,
        .event_handler = http_event,
        .user_data = c,
        .keep_alive_enable = true,
        .disable_auto_redirect = true,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    if (strncmp(c->url, "https://", 8) == 0) {
        if (strcasecmp(c->host, settings_build_host()) == 0) {
            cfg.cert_pem = server_ca_pem_start;           /* same host as the voice server */
        } else {
            cfg.crt_bundle_attach = esp_crt_bundle_attach; /* any other host: built-in CA bundle */
        }
    }
    c->http = esp_http_client_init(&cfg);
    if (!c->http) return false;
    esp_http_client_set_header(c->http, "Content-Type", "application/json");
    esp_http_client_set_header(c->http, "Accept", "application/json, text/event-stream");
    if (c->key_header[0] && c->api_key[0]) esp_http_client_set_header(c->http, c->key_header, c->api_key);
    ESP_LOGI(TAG, "connecting to %s (%s)", c->host,
             cfg.cert_pem ? "server_ca.pem" : (cfg.crt_bundle_attach ? "built-in CA bundle" : "no TLS"));
    return true;
}

/* One POST. Fills c->body / c->status. */
static mcp_status_t post(mcp_client_t *c, const char *json)
{
    if (c->http && esp_timer_get_time() - c->last_used_us > (int64_t)MCP_IDLE_CLOSE_MS * 1000) http_close(c);
    if (!http_open(c)) return MCP_ERR_NET;
    if (c->session[0]) esp_http_client_set_header(c->http, "Mcp-Session-Id", c->session);
    else esp_http_client_delete_header(c->http, "Mcp-Session-Id");
    if (c->protocol[0]) esp_http_client_set_header(c->http, "MCP-Protocol-Version", c->protocol);
    else esp_http_client_delete_header(c->http, "MCP-Protocol-Version");
    esp_http_client_set_post_field(c->http, json, strlen(json));

    c->body_len = 0;
    c->body_overflow = false;
    if (c->body) c->body[0] = '\0';
    c->hdr_session[0] = '\0';
    c->status = 0;

    esp_err_t err = esp_http_client_perform(c->http);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "request to %s failed: %s", c->host, esp_err_to_name(err));
        http_close(c);
        return MCP_ERR_NET;
    }
    c->status = esp_http_client_get_status_code(c->http);
    c->last_used_us = esp_timer_get_time();
    if (c->hdr_session[0]) strlcpy(c->session, c->hdr_session, sizeof(c->session));
    if (c->body_overflow) {
        ESP_LOGW(TAG, "reply too big (over %d KB)", MCP_MAX_BODY / 1024);
        http_close(c);                        /* the rest of the body is still on the wire */
        return MCP_ERR_PROTOCOL;
    }
    return c->status >= 400 ? MCP_ERR_HTTP : MCP_OK;
}

static void http_error_text(int status, char *err, size_t len)
{
    if (status == 401 || status == 403) snprintf(err, len, "HTTP %d - check the API key", status);
    else if (status == 404)             snprintf(err, len, "HTTP 404 - check the server URL");
    else if (status >= 500)             snprintf(err, len, "Server error (HTTP %d)", status);
    else                                snprintf(err, len, "HTTP %d from the server", status);
}

/* A JSON-RPC request. On MCP_OK, *result is the "result" object (caller deletes *msg_out). */
static mcp_status_t rpc(mcp_client_t *c, const char *method, cJSON *params,
                        cJSON **msg_out, const cJSON **result, char *err, size_t err_len)
{
    int id = ++c->rid;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", id);
    cJSON_AddStringToObject(req, "method", method);
    cJSON_AddItemToObject(req, "params", params ? params : cJSON_CreateObject());
    char *json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!json) { snprintf(err, err_len, "Out of memory"); return MCP_ERR_PROTOCOL; }

    int64_t t0 = esp_timer_get_time();
    mcp_status_t st = post(c, json);
    free(json);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);

    if (st == MCP_ERR_NET) {
        snprintf(err, err_len, "Can't reach the server");
        return st;
    }
    ESP_LOGI(TAG, "%s -> HTTP %d, %u bytes in %d ms", method, c->status, (unsigned)c->body_len, ms);
#if MCP_DEBUG
    ESP_LOGI(TAG, "reply: %.300s", c->body ? c->body : "");
#endif
    if (st == MCP_ERR_HTTP) {
        http_error_text(c->status, err, err_len);
        return st;
    }
    if (st != MCP_OK) {
        snprintf(err, err_len, "Reply too big");
        return st;
    }

    cJSON *msg = mcp_find_reply(c->body, id);
    if (!msg) {
        ESP_LOGW(TAG, "no reply to request %d in: %.120s", id, c->body ? c->body : "");
        snprintf(err, err_len, "Bad reply from the server");
        return MCP_ERR_PROTOCOL;
    }
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(msg, "error");
    if (e) {
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(e, "message");
        snprintf(err, err_len, "%s", cJSON_IsString(m) ? m->valuestring : "Server returned an error");
        ESP_LOGW(TAG, "%s: JSON-RPC error: %s", method, err);
        cJSON_Delete(msg);
        return MCP_ERR_RPC;
    }
    *msg_out = msg;
    *result = cJSON_GetObjectItemCaseSensitive(msg, "result");
    return MCP_OK;
}

static mcp_status_t initialize(mcp_client_t *c, char *err, size_t err_len)
{
    c->session[0] = '\0';
    c->protocol[0] = '\0';
    c->ready = false;

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "protocolVersion", MCP_PROTOCOL);
    cJSON_AddItemToObject(params, "capabilities", cJSON_CreateObject());
    cJSON *info = cJSON_AddObjectToObject(params, "clientInfo");
    cJSON_AddStringToObject(info, "name", c->name);
    cJSON_AddStringToObject(info, "version", c->version);

    cJSON *msg = NULL;
    const cJSON *res = NULL;
    mcp_status_t st = rpc(c, "initialize", params, &msg, &res, err, err_len);
    if (st != MCP_OK) return st;

    const cJSON *pv = cJSON_GetObjectItemCaseSensitive(res, "protocolVersion");
    strlcpy(c->protocol, cJSON_IsString(pv) ? pv->valuestring : MCP_PROTOCOL, sizeof(c->protocol));
    const cJSON *si = cJSON_GetObjectItemCaseSensitive(res, "serverInfo");
    const cJSON *sn = cJSON_GetObjectItemCaseSensitive(si, "name");
    const cJSON *sv = cJSON_GetObjectItemCaseSensitive(si, "version");
    ESP_LOGI(TAG, "connected: %s %s | protocol %s | %s",
             cJSON_IsString(sn) ? sn->valuestring : "?", cJSON_IsString(sv) ? sv->valuestring : "",
             c->protocol, c->session[0] ? "session id received" : "no session id");
    cJSON_Delete(msg);

    /* a notification: no id, no reply expected */
    if (post(c, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}") == MCP_ERR_NET) {
        ESP_LOGW(TAG, "initialized notification failed (continuing)");
    }
    c->ready = true;
    return MCP_OK;
}

static bool retryable(mcp_client_t *c, mcp_status_t st)
{
    if (st == MCP_ERR_NET || st == MCP_ERR_PROTOCOL) return true;
    if (st == MCP_ERR_HTTP) {
        /* 404 (or 400) with a session: the server forgot it; anything 5xx may be temporary */
        if ((c->status == 404 || c->status == 400) && c->session[0]) return true;
        return c->status >= 500;
    }
    return false;
}

mcp_status_t mcp_call_tool(mcp_client_t *c, const char *tool, const cJSON *args,
                           char **text_out, char *err, size_t err_len)
{
    *text_out = NULL;
    err[0] = '\0';
    if (strncmp(c->url, "https://", 8) != 0 && strncmp(c->url, "http://", 7) != 0) {
        snprintf(err, err_len, "Set the server URL");
        return MCP_ERR_CONFIG;
    }

    mcp_status_t st = MCP_OK;
    for (int attempt = 1; attempt <= 2; attempt++) {
        if (!c->ready) {
            st = initialize(c, err, err_len);
            if (st != MCP_OK) {
                if (attempt == 1 && retryable(c, st)) { http_close(c); continue; }
                return st;
            }
        }

        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "name", tool);
        cJSON_AddItemToObject(params, "arguments", args ? cJSON_Duplicate(args, true) : cJSON_CreateObject());
        cJSON *msg = NULL;
        const cJSON *res = NULL;
        st = rpc(c, "tools/call", params, &msg, &res, err, err_len);
        if (st == MCP_OK) {
            bool is_error = false;
            char *text = mcp_result_text(res, &is_error);
            cJSON_Delete(msg);
            if (!text) { snprintf(err, err_len, "Out of memory"); return MCP_ERR_PROTOCOL; }
            *text_out = text;
            if (is_error) {
                snprintf(err, err_len, "%s", text[0] ? text : "Tool reported an error");
                return MCP_ERR_TOOL;
            }
            return MCP_OK;
        }
        if (attempt == 2 || !retryable(c, st)) return st;
        ESP_LOGW(TAG, "%s failed (%s), retrying", tool, err);
        if (st == MCP_ERR_HTTP && c->status < 500) c->ready = false;   /* session expired */
        http_close(c);
    }
    return st;
}
#endif /* MCP_HOST_TEST */
