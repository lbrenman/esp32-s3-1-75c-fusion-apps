#pragma once
/* Board: Waveshare ESP32-S3-Touch-AMOLED-2.06. Selected in the top CMakeLists.txt (BOARD=2_06). */

#define BOARD_NAME            "ESP32-S3-Touch-AMOLED-2.06"
#define BOARD_SCREEN_W        410
#define BOARD_SCREEN_H        502
#define BOARD_SCREEN_ROUND    0        /* a rectangle with rounded corners */
#define BOARD_CORNER_RADIUS   120      /* radius of the panel's visible corners (px), measured on the device */
#define BOARD_LOCK_FOREVER    0        /* bsp_display_lock() timeout that waits forever */

/* Takes the LVGL lock; true when it was taken. (This BSP's bsp_display_lock() returns bool.)
 * Needs bsp/esp-bsp.h where it's used. */
#define board_display_lock(timeout_ms)  (bsp_display_lock(timeout_ms))
