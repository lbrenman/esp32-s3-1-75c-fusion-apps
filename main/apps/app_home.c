/* Home screen: clock, app launcher, Wi-Fi status and a day/night toggle. */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "apps.h"
#include "ui_widgets.h"
#include "net.h"
#include "settings_store.h"
#include "app_config.h"
#include "text_ascii.h"
#include "power.h"

static lv_obj_t   *s_scr;
static lv_obj_t   *s_time;
static lv_obj_t   *s_ampm;
static lv_obj_t   *s_date;
static lv_obj_t   *s_wifi_icon;
static lv_obj_t   *s_wifi_text;
static lv_obj_t   *s_batt;          /* top: [bolt/plug] [battery icon] 87% */
static lv_obj_t   *s_batt_state;
static lv_obj_t   *s_batt_icon;
static lv_obj_t   *s_batt_text;
static lv_timer_t *s_timer;

static void update_clock(void)
{
    if (!net_time_valid()) {
        lv_label_set_text(s_date, "");
        lv_label_set_text(s_time, APP_NAME);
        lv_obj_set_style_text_font(s_time, &lv_font_montserrat_28, 0);
        lv_label_set_text(s_ampm, "");
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    char buf[32];
    strftime(buf, sizeof(buf), "%a, %b %d", &tm);
    lv_label_set_text(s_date, buf);
#if APP_CLOCK_24H
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(s_ampm, "");
#else
    int h = tm.tm_hour % 12;
    snprintf(buf, sizeof(buf), "%d:%02d", h ? h : 12, tm.tm_min);
    lv_label_set_text(s_ampm, tm.tm_hour < 12 ? "AM" : "PM");
#endif
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_44, 0);
    lv_label_set_text(s_time, buf);
}

static void update_wifi(void)
{
    app_settings_t cfg;
    switch (net_state()) {
    case NET_UP:
        settings_get(&cfg);
        lv_obj_set_style_text_color(s_wifi_icon, ui_pal.ok, 0);
        {
            char name[3 * SET_SSID_MAX + 1];     /* "Leor's iPhone" uses ’, which the font lacks */
            text_to_ascii(cfg.wifi_ssid, name, sizeof(name));
            if (strcmp(lv_label_get_text(s_wifi_text), name) != 0) lv_label_set_text(s_wifi_text, name);
        }
        break;
    case NET_CONNECTING:
        lv_obj_set_style_text_color(s_wifi_icon, ui_pal.warn, 0);
        lv_label_set_text(s_wifi_text, "Connecting...");
        break;
    default:
        lv_obj_set_style_text_color(s_wifi_icon, ui_pal.err, 0);
        lv_label_set_text(s_wifi_text, "Wi-Fi not set");
        break;
    }
}

static const char *battery_icon(int pct)
{
    if (pct >= 88) return ICON_BATTERY_FULL;
    if (pct >= 63) return ICON_BATTERY_3Q;
    if (pct >= 38) return ICON_BATTERY_HALF;
    if (pct >= 13) return ICON_BATTERY_1Q;
    return ICON_BATTERY_EMPTY;
}

/* Battery at the top: green bolt while charging, plug when full or on USB without a battery,
 * red when 15 % or lower on battery. Hidden if the power chip didn't answer. */
static void update_battery(void)
{
    power_info_t p;
    power_get(&p);
    if (!p.available) {
        lv_obj_add_flag(s_batt, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_batt, LV_OBJ_FLAG_HIDDEN);

    char buf[16];
    if (!p.battery) {
        lv_label_set_text(s_batt_state, ICON_PLUG);
        lv_obj_set_style_text_color(s_batt_state, ui_pal.text_dim, 0);
        lv_obj_remove_flag(s_batt_state, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_batt_icon, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_batt_text, "USB");
        lv_obj_set_style_text_color(s_batt_text, ui_pal.text_dim, 0);
        return;
    }
    lv_obj_remove_flag(s_batt_icon, LV_OBJ_FLAG_HIDDEN);
    bool low = p.percent >= 0 && p.percent <= 15 && !p.usb;
    lv_color_t c = p.charging || p.full ? ui_pal.ok : (low ? ui_pal.err : ui_pal.text_dim);
    lv_label_set_text(s_batt_icon, battery_icon(p.percent < 0 ? 0 : p.percent));
    lv_obj_set_style_text_color(s_batt_icon, c, 0);

    if (p.charging)  lv_label_set_text(s_batt_state, ICON_BOLT);
    else if (p.usb)  lv_label_set_text(s_batt_state, ICON_PLUG);
    if (p.charging || p.usb) {
        lv_obj_set_style_text_color(s_batt_state, ui_pal.ok, 0);
        lv_obj_remove_flag(s_batt_state, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_batt_state, LV_OBJ_FLAG_HIDDEN);
    }

    if (p.percent >= 0) snprintf(buf, sizeof(buf), "%d%%", p.percent);
    else                snprintf(buf, sizeof(buf), "--%%");
    lv_label_set_text(s_batt_text, buf);
    lv_obj_set_style_text_color(s_batt_text, low ? ui_pal.err : ui_pal.text_dim, 0);
}

static void tick_cb(lv_timer_t *t)
{
    update_battery();
    update_clock();
    update_wifi();
}

static void delete_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_scr) return;   /* an older copy of this screen */
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    s_scr = NULL;
}

static void tile_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    ui_nav_open_app((const ui_app_t *)lv_event_get_user_data(e));
}

static void theme_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    app_settings_t cfg;
    settings_get(&cfg);
    cfg.dark_mode = !cfg.dark_mode;
    settings_set(&cfg);
    ui_theme_set_dark(cfg.dark_mode);
    ui_nav_refresh();
}

static lv_obj_t *tile(lv_obj_t *parent, const ui_app_t *app)
{
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, 110, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(t, 10, 0);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *orb = ui_orb(t, 96, app->color1, app->color2, app->icon, &ui_icons_56);
    lv_obj_remove_flag(orb, LV_OBJ_FLAG_CLICKABLE);
    /* press feedback on the orb while the whole tile takes the touch */
    lv_obj_add_flag(orb, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_transform_scale(t, 240, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(t, 55, 0);
    lv_obj_set_style_transform_pivot_y(t, 58, 0);

    ui_label(t, &lv_font_montserrat_16, ui_pal.text, app->name);
    lv_obj_add_event_cb(t, tile_cb, LV_EVENT_CLICKED, (void *)app);
    return t;
}

lv_obj_t *home_build(void *arg)
{
    lv_obj_t *scr = ui_screen();
    s_scr = scr;

    /* battery, at the very top */
    s_batt = lv_obj_create(scr);
    lv_obj_remove_style_all(s_batt);
    lv_obj_remove_flag(s_batt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_batt, LV_SIZE_CONTENT, 22);
    lv_obj_set_flex_flow(s_batt, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_batt, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_batt, 5, 0);
    lv_obj_align(s_batt, LV_ALIGN_TOP_MID, 0, 22);
    s_batt_state = ui_icon(s_batt, &ui_icons_16, ui_pal.ok, ICON_BOLT);
    s_batt_icon = ui_icon(s_batt, &ui_icons_20, ui_pal.text_dim, ICON_BATTERY_FULL);
    s_batt_text = ui_label(s_batt, &lv_font_montserrat_14, ui_pal.text_dim, "");

    /* date + time */
    s_date = ui_label(scr, &lv_font_montserrat_16, ui_pal.text_dim, "");
    lv_obj_align(s_date, LV_ALIGN_TOP_MID, 0, 50);

    lv_obj_t *clock = lv_obj_create(scr);
    lv_obj_remove_style_all(clock);
    lv_obj_remove_flag(clock, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(clock, LV_SIZE_CONTENT, 56);
    lv_obj_align(clock, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_set_flex_flow(clock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clock, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(clock, 6, 0);
    s_time = ui_label(clock, &lv_font_montserrat_44, ui_pal.text, "");
    s_ampm = ui_label(clock, &lv_font_montserrat_16, ui_pal.text_dim, "");
    lv_obj_set_style_pad_bottom(s_ampm, 8, 0);

    /* app grid: three tiles per row; wraps to more rows as apps are added and scrolls if it overflows */
    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, 380, 170);
    lv_obj_align(grid, LV_ALIGN_CENTER, 0, 18);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grid, 14, 0);
    lv_obj_set_style_pad_row(grid, 16, 0);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    for (int i = 0; i < g_app_count; i++) tile(grid, g_apps[i]);

    /* Wi-Fi status pill */
    lv_obj_t *pill = lv_obj_create(scr);
    lv_obj_remove_style_all(pill);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, 34);
    lv_obj_set_style_max_width(pill, 300, 0);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pill, ui_pal.card, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(pill, 14, 0);
    lv_obj_set_style_pad_column(pill, 8, 0);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(pill, LV_ALIGN_BOTTOM_MID, 0, -96);
    s_wifi_icon = ui_icon(pill, &ui_icons_16, ui_pal.text_dim, ICON_WIFI);
    s_wifi_text = ui_label(pill, &lv_font_montserrat_14, ui_pal.text, "");
    /* one line, "..." if the network name is too long (without a fixed height it would wrap
     * to a second line and spill out of the pill) */
    lv_obj_set_style_max_width(s_wifi_text, 240, 0);
    lv_obj_set_height(s_wifi_text, lv_font_get_line_height(&lv_font_montserrat_14));
    lv_label_set_long_mode(s_wifi_text, LV_LABEL_LONG_MODE_DOTS);

    /* day/night toggle: shows the mode it switches to */
    lv_obj_t *theme = ui_round_button(scr, ui_theme_is_dark() ? ICON_SUN : ICON_MOON, &ui_icons_20, 50,
                                      ui_pal.card, ui_theme_is_dark() ? lv_color_hex(0xFFC940) : ui_pal.accent);
    lv_obj_align(theme, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_add_event_cb(theme, theme_cb, LV_EVENT_CLICKED, NULL);

    update_clock();
    update_wifi();
    update_battery();
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = lv_timer_create(tick_cb, 1000, NULL);
    lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);
    return scr;
}
