#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_random.h"

#include "settings_store.h"
#include "app_config.h"
#include "secrets.h"

#define TAG "settings"
#define NVS_NS "fusionapps"

#ifndef WS_PORT
#define WS_PORT 443
#endif
/* Stocks app (added later: an older secrets.h without these still builds) */
#ifndef STOCKS_MCP_URL
#define STOCKS_MCP_URL ""
#endif
#ifndef STOCKS_MCP_API_KEY
#define STOCKS_MCP_API_KEY ""
#endif
/* Translator app (added later). Without these, it uses the voice server's host and API key. */
#ifndef TRANSLATE_API_KEY
#define TRANSLATE_API_KEY WS_API_KEY
#endif
#ifndef TRANSLATE_SOURCE_LANG
#define TRANSLATE_SOURCE_LANG DEFAULT_TR_SOURCE
#endif
#ifndef TRANSLATE_TARGET_LANG
#define TRANSLATE_TARGET_LANG DEFAULT_TR_TARGET
#endif
#define XSTR(x) STR(x)
#define STR(x)  #x

static const char s_default_url[] = "wss://" WS_HOST ":" XSTR(WS_PORT) WS_PATH;
#ifdef TRANSLATE_WS_URL
static const char s_default_tr_url[] = TRANSLATE_WS_URL;
#else
static const char s_default_tr_url[] = "wss://" WS_HOST ":" XSTR(WS_PORT) "/translator";
#endif

static app_settings_t    s_cur;
static SemaphoreHandle_t s_mtx;

const char *settings_default_url(void) { return s_default_url; }
const char *settings_build_host(void)  { return WS_HOST; }
const char *settings_default_mcp_url(void) { return STOCKS_MCP_URL; }
const char *settings_default_tr_url(void) { return s_default_tr_url; }

static void make_uuid(char *out /* SET_CONV_LEN + 1 bytes */)
{
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (b[6] & 0x0F) | 0x40;   /* version 4 */
    b[8] = (b[8] & 0x3F) | 0x80;   /* variant */
    snprintf(out, SET_CONV_LEN + 1, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static void load_defaults(app_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    strlcpy(s->wifi_ssid, WIFI_SSID, sizeof(s->wifi_ssid));
    strlcpy(s->wifi_pass, WIFI_PASSWORD, sizeof(s->wifi_pass));
    strlcpy(s->ws_url, s_default_url, sizeof(s->ws_url));
    strlcpy(s->ws_api_key, WS_API_KEY, sizeof(s->ws_api_key));
    s->dark_mode  = DEFAULT_DARK_MODE;
    s->brightness = DEFAULT_BRIGHTNESS;
    s->volume     = DEFAULT_VOLUME;
    strlcpy(s->mcp_url, STOCKS_MCP_URL, sizeof(s->mcp_url));
    strlcpy(s->mcp_api_key, STOCKS_MCP_API_KEY, sizeof(s->mcp_api_key));
    s->stocks_auto = DEFAULT_STOCKS_AUTO;
    s->wake_word   = DEFAULT_WAKE_WORD;
    strlcpy(s->tr_url, s_default_tr_url, sizeof(s->tr_url));
    strlcpy(s->tr_api_key, TRANSLATE_API_KEY, sizeof(s->tr_api_key));
    strlcpy(s->tr_src, TRANSLATE_SOURCE_LANG, sizeof(s->tr_src));
    strlcpy(s->tr_dst, TRANSLATE_TARGET_LANG, sizeof(s->tr_dst));
}

static void get_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        return;    /* not saved yet (or too long): keep the default */
    }
}

static void get_u8(nvs_handle_t h, const char *key, uint8_t *dst)
{
    uint8_t v;
    if (nvs_get_u8(h, key, &v) == ESP_OK) *dst = v;
}

void settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_mtx = xSemaphoreCreateMutex();
    load_defaults(&s_cur);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t dark = s_cur.dark_mode;
        uint8_t stk_auto = s_cur.stocks_auto;
        uint8_t wake = s_cur.wake_word;
        get_str(h, "ssid",   s_cur.wifi_ssid,  sizeof(s_cur.wifi_ssid));
        get_str(h, "pass",   s_cur.wifi_pass,  sizeof(s_cur.wifi_pass));
        get_str(h, "url",    s_cur.ws_url,     sizeof(s_cur.ws_url));
        get_str(h, "apikey", s_cur.ws_api_key, sizeof(s_cur.ws_api_key));
        get_str(h, "conv",   s_cur.conversation_id, sizeof(s_cur.conversation_id));
        get_u8(h, "dark",   &dark);
        get_u8(h, "bright", &s_cur.brightness);
        get_u8(h, "vol",    &s_cur.volume);
        get_str(h, "mcpurl", s_cur.mcp_url,     sizeof(s_cur.mcp_url));
        get_str(h, "mcpkey", s_cur.mcp_api_key, sizeof(s_cur.mcp_api_key));
        get_u8(h, "stkauto", &stk_auto);
        get_u8(h, "wake",   &wake);
        get_str(h, "trurl",  s_cur.tr_url,     sizeof(s_cur.tr_url));
        get_str(h, "trkey",  s_cur.tr_api_key, sizeof(s_cur.tr_api_key));
        get_str(h, "trsrc",  s_cur.tr_src,     sizeof(s_cur.tr_src));
        get_str(h, "trdst",  s_cur.tr_dst,     sizeof(s_cur.tr_dst));
        s_cur.dark_mode = dark != 0;
        s_cur.stocks_auto = stk_auto != 0;
        s_cur.wake_word = wake != 0;
        nvs_close(h);
        ESP_LOGI(TAG, "Loaded saved settings");
    } else {
        ESP_LOGI(TAG, "No saved settings yet; using secrets.h defaults");
    }
    if (s_cur.brightness < 5)   s_cur.brightness = 5;
    if (s_cur.brightness > 100) s_cur.brightness = 100;
    if (s_cur.volume > 100)     s_cur.volume = 100;

    /* first start (or after a reset): make the conversation ID and keep it */
    if (strlen(s_cur.conversation_id) != SET_CONV_LEN) {
        settings_new_conversation(NULL, 0);
    } else {
        ESP_LOGI(TAG, "Conversation %s", s_cur.conversation_id);
    }
}

esp_err_t settings_new_conversation(char *out, size_t out_len)
{
    app_settings_t s;
    settings_get(&s);
    make_uuid(s.conversation_id);
    esp_err_t err = settings_set(&s);
    ESP_LOGI(TAG, "New conversation %s", s.conversation_id);
    if (out && out_len) strlcpy(out, s.conversation_id, out_len);
    return err;
}

void settings_get(app_settings_t *out)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_cur;
    xSemaphoreGive(s_mtx);
}

esp_err_t settings_set(const app_settings_t *in)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    app_settings_t old = s_cur;
    s_cur = *in;
    xSemaphoreGive(s_mtx);

    /* only write what changed: less flash wear, and never logs any values */
    if (strcmp(old.wifi_ssid, in->wifi_ssid))   err |= nvs_set_str(h, "ssid", in->wifi_ssid);
    if (strcmp(old.wifi_pass, in->wifi_pass))   err |= nvs_set_str(h, "pass", in->wifi_pass);
    if (strcmp(old.ws_url, in->ws_url))         err |= nvs_set_str(h, "url", in->ws_url);
    if (strcmp(old.ws_api_key, in->ws_api_key)) err |= nvs_set_str(h, "apikey", in->ws_api_key);
    if (strcmp(old.conversation_id, in->conversation_id)) err |= nvs_set_str(h, "conv", in->conversation_id);
    if (old.dark_mode != in->dark_mode)         err |= nvs_set_u8(h, "dark", in->dark_mode ? 1 : 0);
    if (old.brightness != in->brightness)       err |= nvs_set_u8(h, "bright", in->brightness);
    if (old.volume != in->volume)               err |= nvs_set_u8(h, "vol", in->volume);
    if (strcmp(old.mcp_url, in->mcp_url))       err |= nvs_set_str(h, "mcpurl", in->mcp_url);
    if (strcmp(old.mcp_api_key, in->mcp_api_key)) err |= nvs_set_str(h, "mcpkey", in->mcp_api_key);
    if (old.stocks_auto != in->stocks_auto)     err |= nvs_set_u8(h, "stkauto", in->stocks_auto ? 1 : 0);
    if (old.wake_word != in->wake_word)         err |= nvs_set_u8(h, "wake", in->wake_word ? 1 : 0);
    if (strcmp(old.tr_url, in->tr_url))         err |= nvs_set_str(h, "trurl", in->tr_url);
    if (strcmp(old.tr_api_key, in->tr_api_key)) err |= nvs_set_str(h, "trkey", in->tr_api_key);
    if (strcmp(old.tr_src, in->tr_src))         err |= nvs_set_str(h, "trsrc", in->tr_src);
    if (strcmp(old.tr_dst, in->tr_dst))         err |= nvs_set_str(h, "trdst", in->tr_dst);
    err |= nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) ESP_LOGE(TAG, "Saving settings failed");
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t settings_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    load_defaults(&s_cur);
    xSemaphoreGive(s_mtx);
    ESP_LOGI(TAG, "Settings reset to defaults");
    settings_new_conversation(NULL, 0);    /* a reset also starts a new conversation */
    return ESP_OK;
}
