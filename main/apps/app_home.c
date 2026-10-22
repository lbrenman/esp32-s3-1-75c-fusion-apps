/* Home screen: clock, app carousel, Wi-Fi status and a day/night toggle.
 *
 * The apps sit in a row you swipe left and right, one app at a time, like a watch: the app in the
 * middle is full size, its neighbors smaller and dimmer. Tap an app to open it. Home remembers
 * which app was in the middle (the one you last opened), so going back lands on it. */

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

/* Carousel geometry */
#define HOME_TILE_W      120      /* tile width (orb + name) */
#define HOME_ORB         104      /* orb size of the tile in the middle */
#define HOME_TILE_GAP    10
#define HOME_TILE_STEP   (HOME_TILE_W + HOME_TILE_GAP)
#define HOME_SIDE_ORB    80       /* orb size of the neighbors (the 56 px icon still fits)... */
#define HOME_SIDE_OPA    140      /* ...and how dim they are (255 = not at all) */
#define HOME_FAR_ORB     60       /* two away: smaller still, and faded out */
#define HOME_TILE_H      150      /* orb area (HOME_ORB) + name */

static int       s_center_app = 0;      /* the app in the middle of the carousel; kept across rebuilds */
static lv_obj_t  *s_carousel;
static lv_obj_t  *s_dots[8];

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
    s_carousel = NULL;
    memset(s_dots, 0, sizeof(s_dots));
}

static void tile_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    const ui_app_t *app = (const ui_app_t *)lv_event_get_user_data(e);
    for (int i = 0; i < g_app_count; i++) if (g_apps[i] == app) s_center_app = i;
    ui_nav_open_app(app);
}

/* Size and dim each tile's orb by its distance from the middle; light the matching page dot.
 * This runs on every scroll step, so it changes only plain properties (size, opacity). A transform
 * (scale) or a whole-object opacity would make LVGL draw each tile into an off-screen layer and
 * resample it on every frame, which is what made the first version jerky on the ESP32-S3. */
static void carousel_update(void)
{
    if (!s_carousel) return;
    lv_area_t c;
    lv_obj_get_coords(s_carousel, &c);
    int32_t mid = (c.x1 + c.x2) / 2;
    int32_t best = INT32_MAX;
    int center = s_center_app;
    for (int i = 0; i < g_app_count && i < (int)lv_obj_get_child_count(s_carousel); i++) {
        lv_obj_t *t = lv_obj_get_child(s_carousel, i);
        lv_area_t a;
        lv_obj_get_coords(t, &a);
        int32_t d = LV_ABS((a.x1 + a.x2) / 2 - mid);
        if (d < best) { best = d; center = i; }
        int32_t size, o;
        if (d <= HOME_TILE_STEP) {          /* middle -> neighbor: shrink and dim a little */
            size = HOME_ORB - d * (HOME_ORB - HOME_SIDE_ORB) / HOME_TILE_STEP;
            o = 255 - d * (255 - HOME_SIDE_OPA) / HOME_TILE_STEP;
        } else {                            /* neighbor -> two away: shrink more and fade out (off the edge) */
            int32_t e = d - HOME_TILE_STEP;
            if (e > HOME_TILE_STEP) e = HOME_TILE_STEP;
            size = HOME_SIDE_ORB - e * (HOME_SIDE_ORB - HOME_FAR_ORB) / HOME_TILE_STEP;
            o = HOME_SIDE_OPA - e * HOME_SIDE_OPA / HOME_TILE_STEP;
        }
        size &= ~1;                                                        /* even: stays centered */
        lv_opa_t opa = (lv_opa_t)o;
        lv_obj_t *orb = lv_obj_get_child(t, 0);
        lv_obj_t *icon = lv_obj_get_child(orb, 0);
        lv_obj_t *name = lv_obj_get_child(t, 1);
        if (lv_obj_get_width(orb) != size) {                               /* only redraw what changed */
            lv_obj_set_size(orb, size, size);
            lv_obj_set_style_transform_pivot_x(orb, size / 2, 0);
            lv_obj_set_style_transform_pivot_y(orb, size / 2, 0);
        }
        if (lv_obj_get_style_bg_opa(orb, 0) != opa) {
            lv_obj_set_style_bg_opa(orb, opa, 0);
            if (icon) lv_obj_set_style_text_opa(icon, opa, 0);
            lv_obj_set_style_text_opa(name, opa, 0);
        }
    }
    s_center_app = center;
    for (int i = 0; i < g_app_count && i < (int)(sizeof(s_dots) / sizeof(s_dots[0])); i++) {
        if (!s_dots[i]) continue;
        lv_obj_set_style_bg_color(s_dots[i], i == center ? ui_pal.text : ui_pal.text_dim, 0);
        lv_obj_set_style_bg_opa(s_dots[i], i == center ? LV_OPA_COVER : LV_OPA_50, 0);
        lv_obj_set_width(s_dots[i], i == center ? 18 : 7);
    }
}

static void carousel_scroll_cb(lv_event_t *e)
{
    carousel_update();
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

static lv_obj_t *tile(lv_obj_t *parent, const ui_app_t *app, int w, int orb_size)
{
    /* fixed size, no flex: the carousel resizes the orb while scrolling, and that must not move
     * anything else (the orb stays centered on the same point, the name stays put) */
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, w, HOME_TILE_H);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(t, LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *orb = ui_orb(t, orb_size, app->color1, app->color2, app->icon, &ui_icons_56);
    lv_obj_remove_flag(orb, LV_OBJ_FLAG_CLICKABLE);
    /* press feedback on the orb while the whole tile takes the touch */
    lv_obj_add_flag(orb, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_align(orb, LV_ALIGN_CENTER, 0, orb_size / 2 - HOME_TILE_H / 2);   /* center at y = orb_size / 2 */

    lv_obj_t *name = ui_label(t, &lv_font_montserrat_16, ui_pal.text, app->name);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -4);
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

    /* app carousel: swipe left/right, one app per swipe, the one in the middle snaps to center */
    s_carousel = lv_obj_create(scr);
    lv_obj_t *car = s_carousel;
    lv_obj_remove_style_all(car);
    lv_obj_set_size(car, UI_SCREEN_SIZE, HOME_TILE_H + 10);
    lv_obj_align(car, LV_ALIGN_CENTER, 0, 6);
    lv_obj_set_flex_flow(car, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(car, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(car, HOME_TILE_GAP, 0);
    lv_obj_set_style_pad_hor(car, (UI_SCREEN_SIZE - HOME_TILE_W) / 2, 0);   /* first and last can center */
    lv_obj_set_scrollbar_mode(car, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(car, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(car, LV_SCROLL_SNAP_CENTER);
    lv_obj_add_flag(car, LV_OBJ_FLAG_SCROLL_ONE);          /* one app per swipe */
    lv_obj_remove_flag(car, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(car, LV_OBJ_FLAG_GESTURE_BUBBLE);
    for (int i = 0; i < g_app_count; i++) tile(car, g_apps[i], HOME_TILE_W, HOME_ORB);
    lv_obj_add_event_cb(car, carousel_scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(car, carousel_scroll_cb, LV_EVENT_SCROLL_END, NULL);

    /* page dots under the carousel */
    memset(s_dots, 0, sizeof(s_dots));
    lv_obj_t *dots = lv_obj_create(scr);
    lv_obj_remove_style_all(dots);
    lv_obj_remove_flag(dots, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, 8);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dots, 6, 0);
    lv_obj_align(dots, LV_ALIGN_BOTTOM_MID, 0, -142);
    for (int i = 0; i < g_app_count && i < (int)(sizeof(s_dots) / sizeof(s_dots[0])); i++) {
        lv_obj_t *d = lv_obj_create(dots);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 7, 7);
        lv_obj_set_style_radius(d, 4, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        s_dots[i] = d;
    }

    /* start on the app you were last in */
    if (s_center_app >= g_app_count) s_center_app = 0;
    lv_obj_update_layout(scr);
    lv_obj_scroll_to_x(car, s_center_app * HOME_TILE_STEP, LV_ANIM_OFF);
    carousel_update();

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
