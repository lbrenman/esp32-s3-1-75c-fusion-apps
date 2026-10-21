#pragma once
/* User settings, saved in NVS (flash). The first boot uses the values from secrets.h;
 * anything changed on the device is saved and survives reboots and reflashing
 * (until "Reset settings" in the Settings app, or an erase-flash). */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SET_SSID_MAX  32
#define SET_PASS_MAX  64
#define SET_URL_MAX   200
#define SET_KEY_MAX   128
#define SET_CONV_LEN  36            /* UUID, e.g. 3f2a9c1e-....-c11e */

typedef struct {
    char    wifi_ssid[SET_SSID_MAX + 1];
    char    wifi_pass[SET_PASS_MAX + 1];
    char    ws_url[SET_URL_MAX + 1];      /* wss://host:port/path */
    char    ws_api_key[SET_KEY_MAX + 1];
    bool    dark_mode;
    uint8_t brightness;                   /* 5-100 % */
    uint8_t volume;                       /* 0-100 % */
    char    conversation_id[SET_CONV_LEN + 1];  /* sent with every voice turn; kept until "New conversation" */
} app_settings_t;

/* Initializes NVS and loads the settings. Call once, first thing in app_main. */
void settings_init(void);

/* Copies the current settings (thread-safe). */
void settings_get(app_settings_t *out);

/* Saves every field that differs from the current settings. */
esp_err_t settings_set(const app_settings_t *in);

/* Forgets everything saved on the device: back to the secrets.h / app_config.h defaults. */
esp_err_t settings_reset(void);

/* Makes and saves a new conversation ID (copied to out if not NULL). */
esp_err_t settings_new_conversation(char *out, size_t out_len);

/* The server URL built from WS_HOST / WS_PORT / WS_PATH in secrets.h. */
const char *settings_default_url(void);

/* The WS_HOST from secrets.h (the host main/server_ca.pem was fetched from). */
const char *settings_build_host(void);
