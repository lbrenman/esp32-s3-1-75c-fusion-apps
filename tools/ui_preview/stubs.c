/* Host stand-ins for the ESP-IDF side (settings, Wi-Fi, voice engine, BSP) so the real UI code
 * in main/ui and main/apps runs on a Mac or Linux PC. Edit the fake values to preview other states. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "settings_store.h"
#include "net.h"
#include "voice.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

int64_t g_sim_us = 0;
int64_t esp_timer_get_time(void) { return g_sim_us; }
void esp_restart(void) { printf("RESTART\n"); }
size_t heap_caps_get_free_size(unsigned c) { return 187 * 1024; }
esp_err_t bsp_display_brightness_set(int p) { printf("brightness %d\n", p); return 0; }

static app_settings_t s = { "Home-WiFi", "example-password", "wss://voice.example.com:4443/ws/speech",
                            "example-api-key-abcd", 1, 80, 75, "3f2a9c1e-7b4d-4e21-9a6f-0c8d5e11c11e" };
void settings_init(void) {}
void settings_get(app_settings_t *o) { *o = s; }
esp_err_t settings_set(const app_settings_t *i) { s = *i; printf("settings saved (dark=%d)\n", s.dark_mode); return 0; }
esp_err_t settings_reset(void) { return 0; }
esp_err_t settings_new_conversation(char *out, size_t len) {
    snprintf(s.conversation_id, sizeof(s.conversation_id), "9d81b7aa-1c2e-4f00-8b3a-7e6f2d90a4b2");
    if (out) snprintf(out, len, "%s", s.conversation_id);
    return 0; }
const char *settings_default_url(void) { return "wss://example:4443/ws/speech"; }
const char *settings_build_host(void) { return "example"; }

net_state_t g_net = NET_UP;
void net_start(void) {}
void net_reconnect(void) { printf("net_reconnect -> %s\n", s.wifi_ssid); }
net_state_t net_state(void) { return g_net; }
bool net_is_up(void) { return g_net == NET_UP; }
bool net_ip(char *b, size_t l) { snprintf(b, l, "192.168.1.42"); return true; }
int net_rssi(void) { return -54; }
int net_last_reason(void) { return 0; }
bool net_time_valid(void) { return true; }
static net_ap_t aps[] = { {"Home-WiFi", -48, true}, {"Guest-WiFi", -61, true}, {"xfinitywifi", -72, false}, {"NETGEAR-5G", -83, true} };
int g_scan_polls = 0;
void net_scan_start(void) { g_scan_polls = 0; }
bool net_scan_result(const net_ap_t **a, int *n) { if (++g_scan_polls < 5) return false; *a = aps; *n = 4; return true; }

voice_state_t g_vs = VOICE_READY;
char g_heard[220] = "", g_reply[340] = "", g_note[100] = "";
uint32_t g_ver = 1;
bool g_vactive = false;
esp_err_t voice_init(void) { return 0; }
void voice_set_active(bool a) { g_vactive = a; printf("voice active %d\n", a); }
bool g_want = true;
void voice_reconnect(void) { printf("voice_reconnect\n"); g_want = true; }
void voice_set_connected(bool on) { printf("voice_set_connected %d\n", on); g_want = on; g_vs = on ? VOICE_READY : VOICE_DISCONNECTED; }
bool voice_wants_connection(void) { return g_want; }
void voice_new_conversation(void) { settings_new_conversation(NULL, 0); printf("voice_new_conversation\n"); }
void voice_set_volume(int p) {}
void voice_ptt_press(void) { printf("ptt press\n"); if (!g_want) { voice_set_connected(true); return; } if (g_vs == VOICE_READY) g_vs = VOICE_LISTENING; }
void voice_ptt_release(void) { printf("ptt release\n"); if (g_vs == VOICE_LISTENING) g_vs = VOICE_THINKING; }
void voice_ptt_cancel(void) { printf("ptt cancel\n"); if (g_vs == VOICE_LISTENING) g_vs = VOICE_READY; }
voice_state_t voice_state(void) { return g_vs; }
const char *voice_state_text(voice_state_t st) {
    static const char *t[] = {"Off","Waiting for Wi-Fi...","Connecting...","Hold to talk","Listening...","Thinking...","Speaking...","Not connected","Disconnected"};
    return t[st]; }
bool voice_connected(void) { return g_vs >= VOICE_READY && g_vs != VOICE_ERROR; }
int64_t voice_idle_us(void) { return 0; }
uint32_t voice_text_version(void) { return g_ver; }
void voice_text_get(char *h, size_t hl, char *r, size_t rl, char *n, size_t nl) {
    if (h) snprintf(h, hl, "%s", g_heard);
    if (r) snprintf(r, rl, "%s", g_reply);
    if (n) snprintf(n, nl, "%s", g_note);
}
