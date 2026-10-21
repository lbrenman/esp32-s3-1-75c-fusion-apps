/*
 * display_start.c — starts the display like the BSP's bsp_display_start(), with smaller LVGL
 * draw buffers.
 *
 * Why: the BSP draws in bands of 50 lines (466 x 50 x 2 bytes = 45.5 KB) kept in PSRAM. The SPI
 * driver can't send from PSRAM, so for every band it borrows a DMA buffer of the same size from
 * internal RAM, and with double buffering two of them can be out at once. With the wake word and
 * a TLS connection also using internal RAM, those big blocks weren't always there and every other
 * band failed ("Draw bitmap failed: ESP_ERR_NO_MEM", striped screens). Bands of APP_LVGL_BUF_LINES
 * lines need only small blocks.
 *
 * Everything else is the BSP's own code path: bsp_display_new() (SPI bus, CO5300 panel),
 * bsp_touch_new(), the LVGL adapter, the same rounder (the CO5300 wants even coordinates).
 */

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_lv_adapter.h"

#include "display_start.h"
#include "app_config.h"

#define TAG "display"

/* The CO5300 needs areas that start on an even pixel and end on an odd one (from the BSP) */
static void rounder_event_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

lv_display_t *app_display_start(void)
{
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 1,
            .mirror_y = 1,
        },
    };
    if (esp_lv_adapter_init(&cfg.lv_adapter_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "LVGL adapter init failed");
        return NULL;
    }

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    const bsp_display_config_t disp_config = {
        .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES * BSP_LCD_BITS_PER_PIXEL / 8,
    };
    if (bsp_display_new(&disp_config, &panel, &io) != ESP_OK) {
        ESP_LOGE(TAG, "Display panel init failed");
        return NULL;
    }

    esp_lv_adapter_display_config_t disp_cfg = {
        .panel = panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = cfg.rotation,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = APP_LVGL_BUF_LINES,
            .use_psram = true,
            .enable_ppa_accel = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = cfg.tear_avoid_mode,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "Could not register the display with LVGL");
        return NULL;
    }
    lv_display_add_event_cb(disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    esp_lcd_touch_handle_t tp = NULL;
    if (bsp_touch_new(&cfg, &tp) == ESP_OK && tp) {
        const esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
        if (!esp_lv_adapter_register_touch(&touch_cfg)) {
            ESP_LOGE(TAG, "Could not register the touch screen with LVGL");
        }
    } else {
        ESP_LOGE(TAG, "Touch init failed");
    }

    bsp_display_brightness_init();
    if (esp_lv_adapter_start() != ESP_OK) {
        ESP_LOGE(TAG, "LVGL task start failed");
        return NULL;
    }
    ESP_LOGI(TAG, "Display started: %d-line draw bands (%d KB each)", APP_LVGL_BUF_LINES,
             BSP_LCD_H_RES * APP_LVGL_BUF_LINES * BSP_LCD_BITS_PER_PIXEL / 8 / 1024);
    return disp;
}
