/* Battery and charging from the AXP2101 power chip. See power.h.
 * Register meanings follow the AXP2101 datasheet / XPowersLib (lewisxhe/XPowersLib). */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"
#include "power.h"

#define TAG "power"

#define AXP2101_ADDR        0x34
#define REG_STATUS1         0x00   /* bit5 VBUS good, bit3 battery present */
#define REG_STATUS2         0x01   /* bits 6:5 current direction (01 charging, 10 discharging), bits 2:0 charger state */
#define REG_ADC_CTRL        0x30   /* bit0 battery voltage measurement on */
#define REG_BAT_V_H         0x34   /* battery voltage, 14 bits, mV (high 6 bits) */
#define REG_BAT_V_L         0x35
#define REG_BAT_DET_CTRL    0x68   /* bit0 battery detection on */
#define REG_BAT_PERCENT     0xA4   /* fuel gauge, 0-100 % */
#define CHG_STATE_DONE      4

#define POLL_US             (2 * 1000 * 1000)

static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t       s_mtx;
static power_info_t            s_info;
static int64_t                 s_read_us;

static bool reg_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 50) == ESP_OK;
}

static bool reg_set_bit(uint8_t reg, uint8_t bit)
{
    uint8_t v;
    if (!reg_read(reg, &v)) return false;
    if (v & (1u << bit)) return true;
    uint8_t buf[2] = { reg, (uint8_t)(v | (1u << bit)) };
    return i2c_master_transmit(s_dev, buf, 2, 50) == ESP_OK;
}

static void read_chip(power_info_t *p)
{
    memset(p, 0, sizeof(*p));
    p->percent = -1;
    uint8_t st1, st2;
    if (!reg_read(REG_STATUS1, &st1) || !reg_read(REG_STATUS2, &st2)) return;
    p->available = true;
    p->battery = st1 & (1u << 3);
    p->usb = st1 & (1u << 5);
    p->charging = p->battery && ((st2 >> 5) & 0x03) == 0x01;
    p->full = p->battery && p->usb && !p->charging && (st2 & 0x07) == CHG_STATE_DONE;
    if (p->battery) {
        uint8_t pct, vh, vl;
        if (reg_read(REG_BAT_PERCENT, &pct) && pct <= 100) p->percent = pct;
        if (reg_read(REG_BAT_V_H, &vh) && reg_read(REG_BAT_V_L, &vl)) p->millivolts = ((vh & 0x3F) << 8) | vl;
    }
}

void power_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    s_info.percent = -1;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGW(TAG, "no I2C bus: battery level unavailable");
        return;
    }
    if (i2c_master_probe(bus, AXP2101_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not found at 0x%02X: battery level unavailable", AXP2101_ADDR);
        return;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "can't add the AXP2101 to the I2C bus");
        s_dev = NULL;
        return;
    }
    /* make sure battery detection and the voltage measurement are on (usually already are) */
    reg_set_bit(REG_BAT_DET_CTRL, 0);
    reg_set_bit(REG_ADC_CTRL, 0);

    read_chip(&s_info);
    s_read_us = esp_timer_get_time();
    if (s_info.battery) {
        ESP_LOGI(TAG, "battery %d%%, %d mV, %s", s_info.percent, s_info.millivolts,
                 s_info.charging ? "charging" : s_info.full ? "full" : s_info.usb ? "on USB" : "on battery");
    } else {
        ESP_LOGI(TAG, "no battery connected (%s)", s_info.usb ? "on USB" : "?");
    }
}

void power_get(power_info_t *out)
{
    if (!s_mtx) { memset(out, 0, sizeof(*out)); out->percent = -1; return; }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_dev && esp_timer_get_time() - s_read_us >= POLL_US) {
        read_chip(&s_info);
        s_read_us = esp_timer_get_time();
    }
    *out = s_info;
    xSemaphoreGive(s_mtx);
}
