/*
 * esp32-s3-1-75c-fusion-apps — a small app launcher for the Waveshare ESP32-S3-Touch-AMOLED-1.75C
 * and ESP32-S3-Touch-AMOLED-2.06 (BOARD in the top CMakeLists.txt; boards/<BOARD>/board_bsp)
 *
 *   Home screen ─┬─ Voice chat  (push-to-talk client for the Amplify Fusion voice server)
 *                ├─ Translator  (push-to-talk voice translator, same engine as Voice chat)
 *                ├─ Stocks      (watchlist client for the FusionStockWatchListMCP server)
 *                ├─ Quotes      (quotes and a watchlist from the Stock API, plain REST)
 *                └─ Settings    (Wi-Fi, dark mode, brightness, volume, phone page, reset)
 *
 * Phone settings page: http://fusion-apps.local on the same Wi-Fi (web_settings.c), PIN in Settings.
 *
 * Boot order: settings (NVS) -> display -> battery -> UI -> audio -> stocks + quotes engines -> Wi-Fi -> phone page.
 * Apps live in main/apps/, shared UI pieces in main/ui/.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "settings_store.h"
#include "display_start.h"
#include "net.h"
#include "voice.h"
#include "stocks.h"
#include "quotes.h"
#include "power.h"
#include "web_settings.h"
#include "apps.h"
#include "ui_theme.h"
#include "ui_nav.h"
#include "app_config.h"
#include "board.h"

#define TAG "app"

void app_main(void)
{
    ESP_LOGI(TAG, "%s v%s for %s", APP_NAME, APP_VERSION, BOARD_NAME);

    settings_init();
    app_settings_t cfg;
    settings_get(&cfg);

    lv_display_t *disp = app_display_start();     /* bsp_display_start() with smaller draw bands */
    if (!disp) {
        ESP_LOGE(TAG, "Display init failed");
        return;
    }
    bsp_display_brightness_set(cfg.brightness);
    power_init();                     /* battery level (AXP2101 on the I2C bus the display started) */

    /* Take the LVGL lock to build the first screen. If the LVGL task is stuck (a display transfer
     * that never finished), this would wait forever without a word: say so every 3 s instead. */
    ESP_LOGI(TAG, "Building the UI (free internal %u KB, largest DMA block %u KB)",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024));
    for (int waited = 3; !board_display_lock(3000); waited += 3) {
        ESP_LOGW(TAG, "Still waiting for the display lock after %d s: the LVGL task is stuck "
                      "(free internal %u KB, largest DMA block %u KB)", waited,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024));
    }
    ui_theme_init(disp, cfg.dark_mode);
    ui_nav_init(home_build);
    bsp_display_unlock();
    ESP_LOGI(TAG, "UI built");

    if (voice_init() != ESP_OK) {
        ESP_LOGE(TAG, "Audio init failed: Voice chat will show an error");
    }
    voice_set_volume(cfg.volume);
    stocks_init();
    quotes_init();

    net_start();
    web_settings_start();             /* phone settings page, if switched on (Settings > Phone) */

    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "Free memory: internal %u KB (largest DMA block %u KB), PSRAM %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
