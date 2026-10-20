/* Settings app: Wi-Fi (scan + password), display (dark mode, brightness), sound, about, reset. */

#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"

#include "apps.h"
#include "ui_widgets.h"
#include "net.h"
#include "settings_store.h"
#include "voice.h"
#include "app_config.h"

static lv_obj_t   *s_scr;
static lv_obj_t   *s_list;
static ui_row_t    s_net_row;
static ui_row_t    s_status_row;
static lv_timer_t *s_timer;
static int32_t     s_keep_scroll = -1;   /* restore the scroll position after a theme rebuild */

/* ---------------------------------------------------------------- helpers */
static void status_text(char *buf, size_t len)
{
    char ip[20];
    switch (net_state()) {
    case NET_UP:
        net_ip(ip, sizeof(ip));
        snprintf(buf, len, "Connected  %s  %d dBm", ip, net_rssi());
        break;
    case NET_CONNECTING: {
        int r = net_last_reason();
        if (r == 201)                 snprintf(buf, len, "Network not found");
        else if (r == 15 || r == 202) snprintf(buf, len, "Wrong password?");
        else                          snprintf(buf, len, "Connecting...");
        break;
    }
    default:
        snprintf(buf, len, "Choose a network");
        break;
    }
}

static void refresh_rows(void)
{
    if (!s_scr) return;
    app_settings_t cfg;
    settings_get(&cfg);
    lv_label_set_text(s_net_row.value, cfg.wifi_ssid[0] ? cfg.wifi_ssid : "Not set");
    char buf[64];
    status_text(buf, sizeof(buf));
    lv_label_set_text(s_status_row.value, buf);
    lv_obj_t *icon_box = lv_obj_get_child(s_status_row.row, 0);
    uint32_t c = net_is_up() ? UI_C_GREEN : (net_state() == NET_CONNECTING ? UI_C_ORANGE : UI_C_RED);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(c), 0);
}

static void tick_cb(lv_timer_t *t) { refresh_rows(); }

static void delete_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_scr) return;
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    s_scr = NULL;
}

static void apply_wifi(const char *ssid, const char *pass)
{
    app_settings_t cfg;
    settings_get(&cfg);
    strlcpy(cfg.wifi_ssid, ssid, sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_pass, pass, sizeof(cfg.wifi_pass));
    settings_set(&cfg);
    net_reconnect();
    char msg[64];
    snprintf(msg, sizeof(msg), "Connecting to %s", ssid);
    ui_toast(msg);
}

/* ------------------------------------------------------------ Wi-Fi flow */
static char s_pending_ssid[SET_SSID_MAX + 1];

static bool password_saved(const char *text, void *user)
{
    apply_wifi(s_pending_ssid, text);
    return false;                     /* editor pops itself (pop_after) */
}

static void open_password_editor(const char *ssid, bool replace, int pop_after)
{
    strlcpy(s_pending_ssid, ssid, sizeof(s_pending_ssid));
    app_settings_t cfg;
    settings_get(&cfg);
    char title[64];
    snprintf(title, sizeof(title), "%s", ssid);
    ui_editor_cfg_t ed = {
        .title = title,
        .icon = ICON_LOCK,
        .initial = strcmp(cfg.wifi_ssid, ssid) == 0 ? cfg.wifi_pass : "",
        .placeholder = "Wi-Fi password",
        .max_len = SET_PASS_MAX,
        .password = true,
        .on_save = password_saved,
        .pop_after = pop_after,
    };
    ui_editor_open(&ed, replace);
}

static bool manual_ssid_saved(const char *text, void *user)
{
    if (!text[0]) return false;
    /* replace this editor with the password editor; saving that pops it and the scan list */
    open_password_editor(text, true, 2);
    return true;
}

/* scan screen */
static lv_obj_t   *s_scan_scr;
static lv_obj_t   *s_scan_list;
static lv_obj_t   *s_scan_busy;
static lv_timer_t *s_scan_timer;

static void scan_pick_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    const net_ap_t *ap = lv_event_get_user_data(e);
    if (ap->secure) {
        open_password_editor(ap->ssid, false, 2);
    } else {
        apply_wifi(ap->ssid, "");
        ui_nav_pop(LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
    }
}

static void scan_manual_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    ui_editor_cfg_t ed = {
        .title = "Network name",
        .icon = ICON_WIFI,
        .placeholder = "SSID (2.4 GHz)",
        .max_len = SET_SSID_MAX,
        .on_save = manual_ssid_saved,
    };
    ui_editor_open(&ed, false);
}

static void scan_again_cb(lv_event_t *e);

static void scan_fill(const net_ap_t *aps, int n)
{
    lv_obj_clean(s_scan_list);
    app_settings_t cfg;
    settings_get(&cfg);
    ui_section(s_scan_list, n ? "NETWORKS" : "NO NETWORKS FOUND");
    for (int i = 0; i < n; i++) {
        char val[40];
        const char *quality = aps[i].rssi > -60 ? "Excellent" : aps[i].rssi > -70 ? "Good" : aps[i].rssi > -80 ? "Fair" : "Weak";
        bool current = strcmp(aps[i].ssid, cfg.wifi_ssid) == 0;
        snprintf(val, sizeof(val), "%s%s, %s", current ? "Current, " : "", aps[i].secure ? "Secured" : "Open", quality);
        uint32_t c = aps[i].rssi > -70 ? UI_C_BLUE : aps[i].rssi > -80 ? UI_C_ORANGE : UI_C_GRAY;
        ui_row(s_scan_list, aps[i].secure ? ICON_LOCK : ICON_WIFI, c, aps[i].ssid, val, scan_pick_cb, (void *)&aps[i]);
    }
    ui_section(s_scan_list, "OTHER");
    ui_row(s_scan_list, ICON_PEN, UI_C_GRAY, "Enter name", "Hidden or not listed", scan_manual_cb, NULL);
    ui_row(s_scan_list, ICON_ROTATE, UI_C_TEAL, "Scan again", NULL, scan_again_cb, NULL);
    lv_obj_send_event(s_scan_list, LV_EVENT_SCROLL, NULL);   /* apply the edge fade */
}

static void scan_poll_cb(lv_timer_t *t)
{
    const net_ap_t *aps;
    int n;
    if (!s_scan_scr || !net_scan_result(&aps, &n)) return;
    lv_timer_pause(t);
    scan_fill(aps, n);
}

static void scan_show_busy(void)
{
    lv_obj_clean(s_scan_list);
    ui_section(s_scan_list, "SEARCHING");
    lv_obj_t *box = lv_obj_create(s_scan_list);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, UI_LIST_WIDTH, 120);
    lv_obj_t *sp = lv_spinner_create(box);
    lv_obj_set_size(sp, 56, 56);
    lv_obj_center(sp);
    lv_obj_set_style_arc_width(sp, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sp, ui_pal.line, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp, ui_pal.accent, LV_PART_INDICATOR);
    s_scan_busy = box;
}

static void scan_again_cb(lv_event_t *e)
{
    scan_show_busy();
    net_scan_start();
    lv_timer_resume(s_scan_timer);
}

static void scan_delete_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_scan_scr) return;
    if (s_scan_timer) lv_timer_delete(s_scan_timer);
    s_scan_timer = NULL;
    s_scan_scr = NULL;
}

static lv_obj_t *scan_build(void *arg)
{
    lv_obj_t *scr;
    s_scan_list = ui_list_screen(&scr, "Wi-Fi", true);
    s_scan_scr = scr;
    if (s_scan_timer) lv_timer_delete(s_scan_timer);
    s_scan_timer = lv_timer_create(scan_poll_cb, 200, NULL);
    scan_show_busy();
    net_scan_start();
    lv_obj_add_event_cb(scr, scan_delete_cb, LV_EVENT_DELETE, NULL);
    return scr;
}

/* ------------------------------------------------------------ main list */
static void net_row_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    ui_nav_push(scan_build, NULL, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

static void pass_row_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    app_settings_t cfg;
    settings_get(&cfg);
    if (!cfg.wifi_ssid[0]) {
        ui_toast("Choose a network first");
        return;
    }
    open_password_editor(cfg.wifi_ssid, false, 1);
}

static void dark_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target_obj(e);
    app_settings_t cfg;
    settings_get(&cfg);
    cfg.dark_mode = lv_obj_has_state(sw, LV_STATE_CHECKED);
    settings_set(&cfg);
    ui_theme_set_dark(cfg.dark_mode);
    s_keep_scroll = lv_obj_get_scroll_y(s_list);
    ui_nav_refresh();
}

static void bright_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target_obj(e);
    int v = lv_slider_get_value(s);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        bsp_display_brightness_set(v);
    } else {                                   /* released: save */
        app_settings_t cfg;
        settings_get(&cfg);
        cfg.brightness = (uint8_t)v;
        settings_set(&cfg);
    }
}

static void volume_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target_obj(e);
    int v = lv_slider_get_value(s);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        voice_set_volume(v);
    } else {
        app_settings_t cfg;
        settings_get(&cfg);
        cfg.volume = (uint8_t)v;
        settings_set(&cfg);
    }
}

static void do_restart(void *user) { esp_restart(); }

static void restart_cb(lv_event_t *e)
{
    ui_confirm("Restart?", "The device restarts now. Your settings are kept.", "Restart", false, do_restart, NULL);
}

static void do_reset(void *user)
{
    settings_reset();
    app_settings_t cfg;
    settings_get(&cfg);
    bsp_display_brightness_set(cfg.brightness);
    voice_set_volume(cfg.volume);
    net_reconnect();
    voice_reconnect();
    ui_theme_set_dark(cfg.dark_mode);
    s_keep_scroll = -1;
    ui_nav_refresh();
    ui_toast("Settings reset");
}

static void reset_cb(lv_event_t *e)
{
    ui_confirm("Reset settings?",
               "Wi-Fi, server and display settings go back to the values in secrets.h.",
               "Reset", true, do_reset, NULL);
}

static lv_obj_t *settings_build(void *arg)
{
    app_settings_t cfg;
    settings_get(&cfg);

    lv_obj_t *scr;
    lv_obj_t *list = ui_list_screen(&scr, "Settings", true);
    s_scr = scr;
    s_list = list;

    ui_section(list, "WI-FI");
    s_net_row = ui_row(list, ICON_WIFI, UI_C_BLUE, "Network", cfg.wifi_ssid, net_row_cb, NULL);
    char masked[48];
    ui_mask_secret(cfg.wifi_pass, masked, sizeof(masked), false);
    ui_row(list, ICON_KEY, UI_C_ORANGE, "Password", masked, pass_row_cb, NULL);
    s_status_row = ui_row(list, ICON_SIGNAL, UI_C_GREEN, "Status", "", NULL, NULL);

    ui_section(list, "DISPLAY");
    ui_row_switch(list, ICON_MOON, UI_C_INDIGO, "Dark mode", cfg.dark_mode, dark_cb, NULL);
    ui_row_slider(list, ICON_SUN, UI_C_ORANGE, "Brightness", 5, 100, cfg.brightness, bright_cb, NULL);

    ui_section(list, "SOUND");
    ui_row_slider(list, ICON_VOLUME, UI_C_RED, "Volume", 0, 100, cfg.volume, volume_cb, NULL);

    ui_section(list, "SYSTEM");
    char about[64];
    snprintf(about, sizeof(about), "v%s  -  %u KB free", APP_VERSION,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    ui_row(list, ICON_INFO, UI_C_GRAY, APP_NAME, about, NULL, NULL);
    ui_row(list, ICON_POWER, UI_C_TEAL, "Restart", NULL, restart_cb, NULL);
    ui_row(list, ICON_RESET, UI_C_RED, "Reset settings", NULL, reset_cb, NULL);

    refresh_rows();
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = lv_timer_create(tick_cb, 1000, NULL);
    lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);

    if (s_keep_scroll > 0) {
        lv_obj_update_layout(list);
        lv_obj_scroll_to_y(list, s_keep_scroll, LV_ANIM_OFF);
    }
    s_keep_scroll = -1;
    return scr;
}

static lv_obj_t *settings_app_build(void *arg) { return settings_build(arg); }

const ui_app_t APP_SETTINGS = {
    .name = "Settings",
    .icon = ICON_GEAR,
    .color1 = UI_C_SETTINGS_1,
    .color2 = UI_C_SETTINGS_2,
    .build = settings_app_build,
};
