/*
 * esp32-s3-1-75c-fusion-apps — a small app launcher for the Waveshare ESP32-S3-Touch-AMOLED-1.75C
 *
 *   Home screen ─┬─ Voice chat  (push-to-talk client for the Amplify Fusion voice server)
 *                ├─ Stocks      (watchlist client for the FusionStockWatchListMCP server)
 *                └─ Settings    (Wi-Fi, dark mode, brightness, volume, reset)
 *
 * Boot order: settings (NVS) -> display -> battery -> UI -> audio -> stocks engine -> Wi-Fi.
 * Apps live in main/apps/, shared UI pieces in main/ui/.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "settings_store.h"
#include "net.h"
#include "voice.h"
#include "stocks.h"
#include "power.h"
#include "apps.h"
#include "ui_theme.h"
#include "ui_nav.h"
#include "app_config.h"

#define TAG "app"

void app_main(void)
{
    ESP_LOGI(TAG, "%s v%s for ESP32-S3-Touch-AMOLED-1.75C", APP_NAME, APP_VERSION);

    settings_init();
    app_settings_t cfg;
    settings_get(&cfg);

    lv_display_t *disp = bsp_display_start();
    if (!disp) {
        ESP_LOGE(TAG, "Display init failed");
        return;
    }
    bsp_display_brightness_set(cfg.brightness);
    power_init();                     /* battery level (AXP2101 on the I2C bus the display started) */

    bsp_display_lock(-1);
    ui_theme_init(disp, cfg.dark_mode);
    ui_nav_init(home_build);
    bsp_display_unlock();

    if (voice_init() != ESP_OK) {
        ESP_LOGE(TAG, "Audio init failed: Voice chat will show an error");
    }
    voice_set_volume(cfg.volume);
    stocks_init();

    net_start();

    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "Free memory: internal %u KB (largest DMA block %u KB), PSRAM %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
