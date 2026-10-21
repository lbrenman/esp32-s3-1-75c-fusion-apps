/* Host stand-ins for the ESP-IDF side (settings, Wi-Fi, voice engine, BSP) so the real UI code
 * in main/ui and main/apps runs on a Mac or Linux PC. Edit the fake values to preview other states. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "settings_store.h"
#include "net.h"
#include "voice.h"
#include "stocks.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

int64_t g_sim_us = 0;
int64_t esp_timer_get_time(void) { return g_sim_us; }
void esp_restart(void) { printf("RESTART\n"); }
size_t heap_caps_get_free_size(unsigned c) { return 187 * 1024; }
esp_err_t bsp_display_brightness_set(int p) { printf("brightness %d\n", p); return 0; }

static app_settings_t s = { "Leor Brenman\xE2\x80\x99s iPhone 15 Pro", "example-password", "wss://voice.example.com:4443/ws/speech",
                            "example-api-key-abcd", 1, 80, 75, "3f2a9c1e-7b4d-4e21-9a6f-0c8d5e11c11e",
                            "https://fusion.example.com:4443/FusionStockWatchListMCP", "example-mcp-key-wxyz", 1 };
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
const char *settings_default_mcp_url(void) { return "https://example:4443/FusionStockWatchListMCP"; }

net_state_t g_net = NET_UP;
void net_start(void) {}
void net_reconnect(void) { printf("net_reconnect -> %s\n", s.wifi_ssid); }
net_state_t net_state(void) { return g_net; }
bool net_is_up(void) { return g_net == NET_UP; }
bool net_ip(char *b, size_t l) { snprintf(b, l, "192.168.1.42"); return true; }
int net_rssi(void) { return -54; }
int net_last_reason(void) { return 0; }
bool net_time_valid(void) { return true; }
static net_ap_t aps[] = { {"Leor Brenman\xE2\x80\x99s iPhone 15 Pro", -42, true}, {"Home-WiFi", -48, true}, {"Guest-WiFi", -61, true}, {"xfinitywifi", -72, false}, {"NETGEAR-5G", -83, true} };
int g_scan_polls = 0;
void net_scan_start(void) { g_scan_polls = 0; }
bool net_scan_result(const net_ap_t **a, int *n) { if (++g_scan_polls < 5) return false; *a = aps; *n = 5; return true; }

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

/* ---- Stocks engine: a fake watchlist. sim.c changes g_ss to preview other states. */
stocks_snapshot_t g_ss;
static uint32_t g_ss_ver = 1;
static const stocks_quote_t fake_quotes[] = {
    { "AAPL", "Apple Inc", 341.07, 5.15, 1.5331, true, true, true },
    { "NVDA", "NVIDIA Corp", 182.44, -3.12, -1.6813, true, true, true },
    { "MSFT", "Microsoft Corp", 1012.5, 0.0, 0.0, true, true, true },
    { "INTC", "Intel Corp", 24.10, -0.42, -1.7128, true, true, true },
    { "TXN", "Texas Instruments Incorporated", 198.32, 2.71, 1.3855, true, true, true },
    { "ZZZZZ", "", 0, 0, 0, false, false, false },
    { "AMD", "Advanced Micro Devices", 151.9, 4.02, 2.718, true, true, true },
};
void stocks_fake_load(int n)
{
    g_ss.nrows = n;
    g_ss.nsyms = n;
    for (int i = 0; i < n; i++) { g_ss.rows[i] = fake_quotes[i]; strcpy(g_ss.syms[i], fake_quotes[i].sym); }
    g_ss.list_loaded = g_ss.quotes_loaded = true;
    g_ss.state = STOCKS_OK;
    g_ss.updated_us = g_sim_us;
    g_ss.updated_at = 1792600860;   /* a fixed time so screenshots don't change */
    g_ss_ver++;
}
void stocks_bump(void) { g_ss_ver++; }
void stocks_init(void) {}
void stocks_set_active(bool a) { printf("stocks active %d\n", a); if (a && g_ss.state == STOCKS_OFF) { g_ss.state = STOCKS_LOADING; g_ss_ver++; } }
void stocks_set_auto(bool on) { g_ss.auto_refresh = on; printf("stocks auto %d\n", on); g_ss_ver++; }
bool stocks_auto(void) { return g_ss.auto_refresh; }
void stocks_refresh_now(void) { printf("stocks refresh\n"); }
void stocks_config_changed(void) { printf("stocks config changed\n"); }
static stocks_tap_t queue(const char *sym, bool toggle)
{
    if (toggle && !g_ss.list_loaded) return STOCKS_TAP_NOT_LOADED;
    for (int i = 0; i < g_ss.npend; i++) if (!strcmp(g_ss.pend[i], sym)) return STOCKS_TAP_PENDING;
    bool listed = false;
    for (int i = 0; i < g_ss.nsyms; i++) if (!strcmp(g_ss.syms[i], sym)) listed = true;
    strcpy(g_ss.pend[g_ss.npend++], sym);
    g_ss_ver++;
    printf("stocks %s %s\n", toggle && !listed ? "add" : "remove", sym);
    return toggle && !listed ? STOCKS_TAP_ADD : STOCKS_TAP_REMOVE;
}
stocks_tap_t stocks_toggle(const char *sym) { return queue(sym, true); }
stocks_tap_t stocks_remove(const char *sym) { return queue(sym, false); }
/* finish the queued changes the way the real engine would */
void stocks_fake_finish(void)
{
    for (int k = 0; k < g_ss.npend; k++) {
        const char *sym = g_ss.pend[k];
        int idx = -1;
        for (int i = 0; i < g_ss.nsyms; i++) if (!strcmp(g_ss.syms[i], sym)) idx = i;
        if (idx >= 0) {
            memmove(&g_ss.syms[idx], &g_ss.syms[idx + 1], (g_ss.nsyms - idx - 1) * sizeof(stocks_sym_t)); g_ss.nsyms--;
            for (int i = 0; i < g_ss.nrows; i++) if (!strcmp(g_ss.rows[i].sym, sym)) {
                memmove(&g_ss.rows[i], &g_ss.rows[i + 1], (g_ss.nrows - i - 1) * sizeof(stocks_quote_t)); g_ss.nrows--; break; }
            snprintf(g_ss.toast, sizeof(g_ss.toast), "Removed %s", sym);
        } else {
            strcpy(g_ss.syms[g_ss.nsyms++], sym);
            stocks_quote_t q = { 0 }; strcpy(q.sym, sym); q.has_price = q.has_change = q.has_pct = true;
            q.price = 100.25; q.change = 1.5; q.pct = 1.52; strcpy(q.name, "Added from Pick");
            g_ss.rows[g_ss.nrows++] = q;
            snprintf(g_ss.toast, sizeof(g_ss.toast), "Added %s", sym);
        }
        g_ss.toast_ver++;
    }
    g_ss.npend = 0;
    g_ss_ver++;
}
uint32_t stocks_version(void) { return g_ss_ver; }
void stocks_get(stocks_snapshot_t *o) { *o = g_ss; }
