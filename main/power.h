#pragma once
/* Battery and charging, from the board's AXP2101 power chip (I2C address 0x34, on the BSP's
 * shared I2C bus). No LVGL in here; the Home screen and Settings poll power_get(). */

#include <stdbool.h>

typedef struct {
    bool available;       /* the AXP2101 answered */
    bool battery;         /* a battery is connected */
    int  percent;         /* 0-100 from the chip's fuel gauge, -1 if unknown */
    int  millivolts;      /* battery voltage, 0 if unknown */
    bool usb;             /* USB power is plugged in */
    bool charging;        /* the battery is charging right now */
    bool full;            /* on USB and charging has finished */
} power_info_t;

void power_init(void);                 /* once at boot, after the display (which starts the I2C bus) */
void power_get(power_info_t *out);     /* reads the chip at most every 2 s; cheap to call often */
