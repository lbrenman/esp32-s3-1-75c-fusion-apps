#pragma once
/* Wi-Fi station + network time. Everything here is safe to call from any task;
 * the UI polls these functions from LVGL timers instead of receiving callbacks. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    NET_NOT_SET,        /* no SSID configured */
    NET_CONNECTING,
    NET_UP,
} net_state_t;

typedef struct {
    char   ssid[33];
    int8_t rssi;
    bool   secure;
} net_ap_t;

#define NET_SCAN_MAX 16

void        net_start(void);         /* once, at boot (after settings_init) */
void        net_reconnect(void);     /* after the SSID/password changed */
net_state_t net_state(void);
bool        net_is_up(void);
bool        net_ip(char *buf, size_t len);   /* false when not connected */
int         net_rssi(void);                  /* dBm, 0 when not connected */
int         net_last_reason(void);           /* last disconnect reason (201 = SSID not found, 15/202 = password) */
bool        net_time_valid(void);            /* clock has been set by NTP */

/* Wi-Fi power saving (modem sleep). On by default to save battery; the voice chat turns it off
 * while its WebSocket is open, because power saving roughly halves upload speed. */
void        net_set_power_save(bool on);

/* Wi-Fi scan in the background. Poll net_scan_result() until it returns true. */
void net_scan_start(void);
bool net_scan_result(const net_ap_t **aps, int *count);   /* true once finished; count may be 0 */
