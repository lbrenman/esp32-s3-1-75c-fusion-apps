#pragma once
/* Board: Waveshare ESP32-S3-Touch-AMOLED-1.75C. Selected in the top CMakeLists.txt (BOARD=1_75c). */

#define BOARD_NAME            "ESP32-S3-Touch-AMOLED-1.75C"
#define BOARD_SCREEN_W        466
#define BOARD_SCREEN_H        466
#define BOARD_SCREEN_ROUND    1        /* a circle: content stays inside it */
#define BOARD_CORNER_RADIUS   233      /* for round screens, half the width */
#define BOARD_LOCK_FOREVER    (-1)     /* bsp_display_lock() timeout that waits forever */

/* Takes the LVGL lock; true when it was taken. (This BSP's bsp_display_lock() returns esp_err_t.)
 * Needs bsp/esp-bsp.h where it's used. */
#define board_display_lock(timeout_ms)  (bsp_display_lock(timeout_ms) == ESP_OK)
