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
 * bsp_touch_new(), the LVGL glue, the same rounder (the CO5300 wants even coordinates).
 * Two versions, one per board (BOARD in the top CMakeLists.txt):
 *   1.75C  the BSP uses Espressif's LVGL adapter (esp_lv_adapter)
 *   2.06   the BSP uses esp_lvgl_port. Its own bsp_display_start() takes the band height from
 *          Kconfig and allocates a single buffer; here it's double-buffered like on the 1.75C.
 *          It also gets a flush guard (see flush_wait_guard): esp_lvgl_port ignores a failed SPI
 *          transfer, and LVGL would then wait for that band forever (screen frozen, task watchdog).
 */

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#if APP_BOARD_2_06
#include "esp_lvgl_port.h"
#include "lvgl_private.h"         /* disp->flushing, for the flush guard */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#else
#include "esp_lv_adapter.h"
#endif

#include "display_start.h"
#include "app_config.h"
#include "board.h"

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

#if APP_BOARD_2_06
/* ---------------------------------------------------------------- ESP32-S3-Touch-AMOLED-2.06 */

/* Flush guard. Each band goes to the panel over SPI, which needs a DMA buffer from internal RAM.
 * If there isn't one ("spi transmit (queue) color failed"), esp_lvgl_port drops the error and the
 * "transfer done" callback never comes, so LVGL's own wait (while (flushing);) never ends: the
 * screen freezes and the task watchdog fires. This wait gives up after FLUSH_GUARD_MS instead,
 * logs it, and repaints the screen a moment later (the lost band is redrawn). */
#define FLUSH_GUARD_MS    250
#define FLUSH_SPIN_US     3000          /* a normal band takes well under 1 ms: spin first, then sleep */

static volatile uint32_t s_flush_lost;
static uint32_t          s_flush_lost_seen;

static void flush_wait_guard(lv_display_t *disp)
{
    int64_t start = esp_timer_get_time();
    while (disp->flushing) {
        int64_t waited = esp_timer_get_time() - start;
        if (waited > (int64_t)FLUSH_GUARD_MS * 1000) {
            s_flush_lost++;
            ESP_LOGW(TAG, "A display transfer never finished (no DMA memory?); skipped it (%u so far). "
                     "Internal RAM free %u KB, largest DMA block %u KB", (unsigned)s_flush_lost,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024));
            return;                       /* LVGL clears disp->flushing and goes on */
        }
        if (waited > FLUSH_SPIN_US) vTaskDelay(1);   /* lets the idle task (and its watchdog) run */
    }
}

/* after a lost band: repaint the whole screen once things have calmed down */
static void flush_repair_timer_cb(lv_timer_t *t)
{
    if (s_flush_lost == s_flush_lost_seen) return;
    s_flush_lost_seen = s_flush_lost;
    lv_obj_invalidate(lv_screen_active());
}

lv_display_t *app_display_start(void)
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8 * 1024;            /* the UI builds whole screens in this task */
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "LVGL port init failed");
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

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = BSP_LCD_H_RES * APP_LVGL_BUF_LINES,
        .double_buffer = true,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,               /* bands in PSRAM; the SPI driver copies each to DMA RAM */
            .sw_rotate = false,
            .swap_bytes = true,                /* the CO5300 wants big-endian RGB565 (as in the BSP) */
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "Could not register the display with LVGL");
        return NULL;
    }
    lv_display_add_event_cb(disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    if (lvgl_port_lock(0)) {
        lv_display_set_flush_wait_cb(disp, flush_wait_guard);
        lv_timer_create(flush_repair_timer_cb, 500, NULL);
        lvgl_port_unlock();
    }

    esp_lcd_touch_handle_t tp = NULL;
    if (bsp_touch_new(NULL, &tp) == ESP_OK && tp) {
        const lvgl_port_touch_cfg_t touch_cfg = { .disp = disp, .handle = tp };
        if (!lvgl_port_add_touch(&touch_cfg)) {
            ESP_LOGE(TAG, "Could not register the touch screen with LVGL");
        }
    } else {
        ESP_LOGE(TAG, "Touch init failed");
    }

    bsp_display_brightness_init();
    ESP_LOGI(TAG, "Display started (%s, %dx%d): %d-line draw bands (%d KB each)", BOARD_NAME,
             BSP_LCD_H_RES, BSP_LCD_V_RES, APP_LVGL_BUF_LINES,
             BSP_LCD_H_RES * APP_LVGL_BUF_LINES * BSP_LCD_BITS_PER_PIXEL / 8 / 1024);
    return disp;
}

#else
/* ---------------------------------------------------------------- ESP32-S3-Touch-AMOLED-1.75C */
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
    ESP_LOGI(TAG, "Display started (%s, %dx%d): %d-line draw bands (%d KB each)", BOARD_NAME,
             BSP_LCD_H_RES, BSP_LCD_V_RES, APP_LVGL_BUF_LINES,
             BSP_LCD_H_RES * APP_LVGL_BUF_LINES * BSP_LCD_BITS_PER_PIXEL / 8 / 1024);
    return disp;
}
#endif
