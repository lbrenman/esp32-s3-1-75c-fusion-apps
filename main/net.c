#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"

#include "net.h"
#include "settings_store.h"
#include "app_config.h"

#define TAG "net"

static volatile net_state_t s_state = NET_NOT_SET;
static volatile bool s_scanning = false;
static volatile bool s_scan_done = false;
static volatile int  s_last_reason = 0;
static esp_netif_t  *s_netif = NULL;
static bool          s_sntp_started = false;

static net_ap_t s_aps[NET_SCAN_MAX];
static int      s_ap_count = 0;

static void sntp_start_once(void)
{
    if (s_sntp_started) return;
    s_sntp_started = true;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(APP_NTP_SERVER);
    cfg.wait_for_sync = false;
    esp_netif_sntp_init(&cfg);
    ESP_LOGI(TAG, "NTP started (%s)", APP_NTP_SERVER);
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_state == NET_CONNECTING) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        s_last_reason = e ? e->reason : -1;
        if (s_state == NET_NOT_SET) return;
        s_state = NET_CONNECTING;
        if (!s_scanning) {
            ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d), retrying", s_last_reason);
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_state = NET_UP;
        s_last_reason = 0;
        sntp_start_once();
    }
}

static void apply_config(void)
{
    app_settings_t cfg;
    settings_get(&cfg);

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, cfg.wifi_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, cfg.wifi_pass, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    if (cfg.wifi_ssid[0] == '\0') {
        s_state = NET_NOT_SET;
        ESP_LOGW(TAG, "No Wi-Fi network set (Settings > Wi-Fi)");
    } else {
        s_state = NET_CONNECTING;
        ESP_LOGI(TAG, "Connecting to Wi-Fi \"%s\"", cfg.wifi_ssid);   /* never log the password */
    }
}

void net_start(void)
{
    setenv("TZ", APP_TZ, 1);
    tzset();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));   /* credentials live in settings, not in Wi-Fi NVS */

    apply_config();
    ESP_ERROR_CHECK(esp_wifi_start());   /* STA_START event calls esp_wifi_connect() */
}

void net_reconnect(void)
{
    s_state = NET_NOT_SET;               /* keeps the disconnect handler from reconnecting with old values */
    esp_wifi_disconnect();
    apply_config();
    if (s_state == NET_CONNECTING) esp_wifi_connect();
}

net_state_t net_state(void) { return s_state; }
bool net_is_up(void)        { return s_state == NET_UP; }
int  net_last_reason(void)  { return s_last_reason; }

bool net_ip(char *buf, size_t len)
{
    esp_netif_ip_info_t ip;
    if (s_state != NET_UP || !s_netif || esp_netif_get_ip_info(s_netif, &ip) != ESP_OK) {
        if (len) buf[0] = '\0';
        return false;
    }
    snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    return true;
}

int net_rssi(void)
{
    if (s_state != NET_UP) return 0;
    int rssi = 0;
    if (esp_wifi_sta_get_rssi(&rssi) != ESP_OK) return 0;
    return rssi;
}

bool net_time_valid(void)
{
    return time(NULL) > 1700000000;      /* anything after Nov 2023 means NTP has run */
}

/* ------------------------------------------------------------------- scanning */
static void scan_task(void *arg)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err == ESP_ERR_WIFI_STATE) {
        /* still trying to connect: pause that, scan, then resume below */
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        err = esp_wifi_scan_start(&sc, true);
    }

    int n = 0;
    if (err == ESP_OK) {
        uint16_t num = 0;
        esp_wifi_scan_get_ap_num(&num);
        wifi_ap_record_t *recs = calloc(num ? num : 1, sizeof(wifi_ap_record_t));
        if (recs) {
            uint16_t got = num;
            esp_wifi_scan_get_ap_records(&got, recs);
            /* records come sorted by signal strength; drop hidden and duplicate names */
            for (int i = 0; i < got && n < NET_SCAN_MAX; i++) {
                const char *ssid = (const char *)recs[i].ssid;
                if (!ssid[0]) continue;
                bool dup = false;
                for (int j = 0; j < n; j++) if (!strcmp(s_aps[j].ssid, ssid)) { dup = true; break; }
                if (dup) continue;
                strlcpy(s_aps[n].ssid, ssid, sizeof(s_aps[n].ssid));
                s_aps[n].rssi = recs[i].rssi;
                s_aps[n].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                n++;
            }
            free(recs);
        } else {
            esp_wifi_clear_ap_list();
        }
        ESP_LOGI(TAG, "Scan found %d networks", n);
    } else {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
    }

    s_ap_count = n;
    s_scanning = false;
    s_scan_done = true;
    if (s_state == NET_CONNECTING) esp_wifi_connect();
    vTaskDelete(NULL);
}

void net_scan_start(void)
{
    if (s_scanning) return;
    s_scan_done = false;
    s_scanning = true;
    if (xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 4, NULL) != pdPASS) {
        s_scanning = false;
        s_ap_count = 0;
        s_scan_done = true;
    }
}

bool net_scan_result(const net_ap_t **aps, int *count)
{
    if (!s_scan_done) return false;
    *aps = s_aps;
    *count = s_ap_count;
    return true;
}
