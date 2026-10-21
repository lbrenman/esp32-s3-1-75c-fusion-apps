#pragma once
/* Starts the display, touch and LVGL like bsp_display_start(), with smaller draw bands
 * (APP_LVGL_BUF_LINES in app_config.h) so each flush needs only a small internal DMA buffer. */

#include "lvgl.h"

lv_display_t *app_display_start(void);
