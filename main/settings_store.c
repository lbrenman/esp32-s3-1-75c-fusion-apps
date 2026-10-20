#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "settings_store.h"
#include "app_config.h"
#include "secrets.h"

#define TAG "settings"
#define NVS_NS "fusionapps"

#ifndef WS_PORT
#define WS_PORT 443
#endif
#define XSTR(x) STR(x)
#define STR(x)  #x

static const char s_default_url[] = "wss://" WS_HOST ":" XSTR(WS_PORT) WS_PATH;

static app_settings_t    s_cur;
static SemaphoreHandle_t s_mtx;

const char *settings_default_url(void) { return s_default_url; }
const char *settings_build_host(void)  { return WS_HOST; }

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
        get_str(h, "ssid",   s_cur.wifi_ssid,  sizeof(s_cur.wifi_ssid));
        get_str(h, "pass",   s_cur.wifi_pass,  sizeof(s_cur.wifi_pass));
        get_str(h, "url",    s_cur.ws_url,     sizeof(s_cur.ws_url));
        get_str(h, "apikey", s_cur.ws_api_key, sizeof(s_cur.ws_api_key));
        get_u8(h, "dark",   &dark);
        get_u8(h, "bright", &s_cur.brightness);
        get_u8(h, "vol",    &s_cur.volume);
        s_cur.dark_mode = dark != 0;
        nvs_close(h);
        ESP_LOGI(TAG, "Loaded saved settings");
    } else {
        ESP_LOGI(TAG, "No saved settings yet; using secrets.h defaults");
    }
    if (s_cur.brightness < 5)   s_cur.brightness = 5;
    if (s_cur.brightness > 100) s_cur.brightness = 100;
    if (s_cur.volume > 100)     s_cur.volume = 100;
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
    if (old.dark_mode != in->dark_mode)         err |= nvs_set_u8(h, "dark", in->dark_mode ? 1 : 0);
    if (old.brightness != in->brightness)       err |= nvs_set_u8(h, "bright", in->brightness);
    if (old.volume != in->volume)               err |= nvs_set_u8(h, "vol", in->volume);
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
    return ESP_OK;
}
