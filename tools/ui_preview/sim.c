/* Host simulator: runs the real UI code against LVGL 9.5 with a fake 466x466 display + touch,
 * drives it like a user would, and writes screenshots (PPM, round-masked). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lvgl.h"
#include "ui_theme.h"
#include "ui_nav.h"
#include "ui_widgets.h"
#include "apps.h"
#include "voice.h"
#include "net.h"
#include "stocks.h"

#define W 466
extern int64_t g_sim_us;
extern voice_state_t g_vs;
extern char g_heard[], g_reply[], g_note[];
extern uint32_t g_ver;
extern bool g_vactive;
extern net_state_t g_net;
extern stocks_snapshot_t g_ss;
void stocks_fake_load(int n);
void stocks_fake_finish(void);
void stocks_bump(void);

static uint16_t fb[W * W];
static uint8_t  drawbuf[W * W * 2];
static int px = 0, py = 0, pressed = 0;

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *map)
{
    int w = lv_area_get_width(a);
    uint16_t *src = (uint16_t *)map;
    for (int y = a->y1; y <= a->y2; y++)
        memcpy(&fb[y * W + a->x1], &src[(y - a->y1) * w], w * 2);
    lv_display_flush_ready(d);
}

static void read_cb(lv_indev_t *i, lv_indev_data_t *data)
{
    data->point.x = px;
    data->point.y = py;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void step(int ms)
{
    for (int t = 0; t < ms; t += 5) {
        g_sim_us += 5000;
        lv_tick_inc(5);
        lv_timer_handler();
    }
}

static void shot(const char *name)
{
    step(500);
    lv_refr_now(NULL);
    char path[128];
    snprintf(path, sizeof(path), "shots/%s.ppm", name);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, W);
    for (int y = 0; y < W; y++) for (int x = 0; x < W; x++) {
        uint16_t c = fb[y * W + x];
        unsigned char rgb[3] = { (c >> 11) << 3, ((c >> 5) & 0x3F) << 2, (c & 0x1F) << 3 };
        int dx = x - W / 2, dy = y - W / 2;
        if (dx * dx + dy * dy > (W / 2) * (W / 2)) { rgb[0] = 0x30; rgb[1] = 0x30; rgb[2] = 0x34; }
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("shot %s\n", name);
}

static void tap(int x, int y)
{
    px = x; py = y; pressed = 1; step(80);
    pressed = 0; step(400);
}

static void hold(int x, int y) { px = x; py = y; pressed = 1; step(300); }
static void release(void) { pressed = 0; step(100); }

static void swipe(int x0, int y0, int x1, int y1)
{
    px = x0; py = y0; pressed = 1; step(40);
    for (int i = 1; i <= 10; i++) { px = x0 + (x1 - x0) * i / 10; py = y0 + (y1 - y0) * i / 10; step(20); }
    pressed = 0; step(500);
}

static lv_obj_t *find_text(lv_obj_t *o, const char *text)
{
    if (lv_obj_check_type(o, &lv_label_class) && strcmp(lv_label_get_text(o), text) == 0) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *r = find_text(lv_obj_get_child(o, i), text);
        if (r) return r;
    }
    return NULL;
}

static void tap_text(const char *text)
{
    lv_obj_t *l = find_text(lv_screen_active(), text);
    if (!l) l = find_text(lv_layer_top(), text);
    if (!l) { printf("!! no '%s' on screen\n", text); return; }
    lv_area_t a;
    lv_obj_get_coords(l, &a);
    printf("tap '%s' at %d,%d\n", text, (a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    tap((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
}

static void bump(void) { g_ver++; }

int main(void)
{
    lv_init();
    lv_display_t *d = lv_display_create(W, W);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, drawbuf, NULL, sizeof(drawbuf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, read_cb);
    g_sim_us = 1000000;

    ui_theme_init(d, true);
    ui_nav_init(home_build);
    shot("01_home_dark");

    tap_text("Voice chat");
    printf("voice active=%d\n", g_vactive);
    shot("02_voice_ready_dark");

    hold(233, 250);
    shot("03_voice_listening");
    release();
    shot("04_voice_thinking");
    strcpy(g_heard, "\"What's the weather like in Boston today?\""); bump();
    shot("05_voice_thinking_heard");
    strcpy(g_reply, "It's 58 degrees and partly cloudy in Boston, with a light breeze off the harbor. Grab a jacket if you're heading out tonight.");
    g_vs = VOICE_SPEAKING; bump();
    shot("06_voice_reply");
    g_vs = VOICE_READY; g_heard[0] = g_reply[0] = 0; strcpy(g_note, "Hold while you talk"); bump();
    shot("07_voice_note");
    g_note[0] = 0; bump();

    swipe(233, 330, 233, 170);           /* swipe up -> voice settings */
    printf("after swipe up: g_vs=%d (should be READY, recording cancelled)\n", g_vs);
    shot("08_voice_settings");
    tap_text("Server URL");
    shot("09_editor_url");
    tap(233 - 130, 52);                  /* x button */
    shot("10_back_to_voice_settings");
    tap_text("API key");
    shot("11_editor_key");
    tap(233 - 130, 52);
    swipe(233, 180, 233, 340);           /* swipe down -> voice chat */
    shot("12_back_to_voice");
    swipe(330, 250, 120, 250);           /* swipe left -> home */
    printf("voice active=%d (should be 0)\n", g_vactive);
    shot("13_home_again");

    tap_text("Settings");
    shot("14_settings_dark");
    swipe(233, 380, 233, 120);           /* scroll */
    shot("15_settings_scrolled");
    swipe(233, 120, 233, 400);
    tap_text("Network");
    step(600);
    shot("16_wifi_scan");
    tap_text("Guest-WiFi");
    shot("17_wifi_password");
    tap(233 - 130, 52);
    step(400);
    swipe(120, 240, 360, 240);           /* swipe right = back */
    shot("18_settings_back");
    swipe(233, 330, 233, 230);
    shot("18b_settings_display");
    lv_obj_t *sw = NULL;
    {   /* toggle dark mode switch */
        lv_obj_t *l = find_text(lv_screen_active(), "Dark mode");
        lv_obj_t *row = lv_obj_get_parent(lv_obj_get_parent(l));
        sw = lv_obj_get_child(row, -1);
        lv_area_t a; lv_obj_get_coords(sw, &a);
        tap((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    }
    shot("19_settings_light");
    swipe(233, 380, 233, 100);
    tap_text("Reset settings");
    shot("20_confirm");
    tap_text("Cancel");
    swipe(120, 240, 360, 240);
    shot("21_home_light");
    tap_text("Voice chat");
    shot("22_voice_light");
    swipe(233, 330, 233, 170);
    shot("23_voice_settings_light");
    swipe(330, 250, 120, 250);
    g_net = NET_CONNECTING;
    shot("24_home_light_connecting");
    g_net = NET_UP;

    /* connection switch */
    ui_theme_set_dark(true); ui_nav_refresh(); step(500);
    tap_text("Voice chat");
    swipe(233, 330, 233, 170);
    {
        lv_obj_t *l = find_text(lv_screen_active(), "Connection");
        lv_obj_t *row = lv_obj_get_parent(lv_obj_get_parent(l));
        lv_obj_t *sw = lv_obj_get_child(row, -1);
        lv_area_t a; lv_obj_get_coords(sw, &a);
        tap((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    }
    printf("want after switch tap: %d (should be 0)\n", voice_wants_connection());
    shot("25_voice_settings_disconnected");
    swipe(233, 180, 233, 340);
    strcpy(g_note, "Disconnected after 3 min idle"); bump();
    shot("26_voice_disconnected");
    tap(233, 233);
    g_note[0] = 0; bump();
    printf("want after mic tap: %d (should be 1)\n", voice_wants_connection());
    shot("27_voice_reconnected");

    /* new conversation */
    swipe(233, 330, 233, 170);
    shot("28_voice_settings_buttons");
    swipe(330, 250, 120, 250);
    tap_text("Settings");
    lv_obj_scroll_to_view_recursive(lv_obj_get_parent(lv_obj_get_parent(find_text(lv_screen_active(), "Conversation"))), LV_ANIM_OFF);
    lv_obj_send_event(lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(find_text(lv_screen_active(), "Conversation")))), LV_EVENT_SCROLL, NULL);
    shot("29_settings_conversation");
    tap_text("New conversation");
    shot("30_new_conversation_confirm");
    tap_text("Start new");
    step(300);
    shot("31_after_new_conversation");

    /* ---- Stocks ---- */
    swipe(120, 240, 360, 240);           /* Settings -> Home */
    shot("40_home_three_apps");
    g_ss.auto_refresh = true;
    tap_text("Stocks");
    shot("41_stocks_loading");
    stocks_fake_load(7);
    shot("42_stocks_watchlist");
    swipe(233, 360, 233, 200);           /* scroll the list */
    shot("43_stocks_scrolled");
    swipe(233, 200, 233, 380);
    tap(163, 410);                       /* auto refresh button -> paused */
    step(2200);
    shot("44_stocks_paused");
    tap(163, 410);                       /* back on */
    step(2200);
    {   /* long-press the NVDA row */
        lv_obj_t *l = find_text(lv_screen_active(), "NVDA");
        lv_area_t a; lv_obj_get_coords(l, &a);
        hold(a.x1 + 100, (a.y1 + a.y2) / 2); step(700); release();
    }
    shot("45_stocks_remove_confirm");
    tap_text("Cancel");
    swipe(360, 250, 110, 250);           /* swipe left -> Pick */
    shot("46_pick_qqq1");
    tap_text("NVDA");                    /* listed: remove */
    tap_text("AMZN");                    /* not listed: add */
    shot("47_pick_pending");
    stocks_fake_finish();
    step(300);
    shot("48_pick_done");
    step(2500);
    tap_text("Other");
    shot("49_pick_other");
    tap_text("Blue Chip");
    shot("50_pick_bluechip");
    swipe(110, 250, 360, 250);           /* swipe right -> Watchlist */
    step(2500);
    shot("51_stocks_after_pick");
    tap(303, 410);                       /* gear */
    shot("52_stock_server");
    tap_text("API key");
    shot("53_stock_server_key_editor");
    tap(233 - 130, 52);
    step(400);
    swipe(110, 250, 360, 250);
    g_ss.state = STOCKS_ERROR; strcpy(g_ss.note, "Can't reach the server"); g_ss.updated_us = g_sim_us - 185000000LL;
    stocks_bump();
    shot("54_stocks_error_with_data");
    g_ss.nrows = g_ss.nsyms = 0; g_ss.state = STOCKS_OK; g_ss.note[0] = 0; stocks_bump();
    shot("55_stocks_empty");
    g_ss.quotes_loaded = false; g_ss.state = STOCKS_NOT_SET; strcpy(g_ss.note, "Set the server URL (gear button)"); stocks_bump();
    shot("56_stocks_not_set");
    ui_theme_set_dark(false); ui_nav_refresh(); step(300);
    stocks_fake_load(7);
    shot("57_stocks_light");
    swipe(360, 250, 110, 250);
    tap_text("QQQ 1");
    shot("58_pick_light");
    swipe(110, 250, 360, 250);
    swipe(110, 250, 360, 250);
    printf("stocks active should be 0 now\n");
    shot("59_home_light_three_apps");
    return 0;
}
