/* Stocks app: a watchlist client for the FusionStockWatchListMCP server (Amplify Fusion MCP).
 *
 *   Watchlist      quotes for every symbol, refreshed every minute while the app is open
 *                  swipe left or + : Pick      long-press a row : remove it
 *                  swipe right or < : Home     gear : Stock server settings
 *   Pick           tabs of preset symbols; tap to add (green) or remove (gray)
 *   Stock server   URL, API key, status, refresh now, reset URL
 *
 * The network work happens in stocks_core.c; these screens only draw, polling the engine's
 * snapshot from LVGL timers, so they never freeze while a request is running. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_timer.h"

#include "apps.h"
#include "ui_widgets.h"
#include "stocks.h"
#include "settings_store.h"
#include "app_config.h"
#include "text_ascii.h"

#define C_STOCKS_1  0x30D158
#define C_STOCKS_2  0x0A7D6B

/* ---- preset symbols on the Pick screen: (tab name, up to 25 symbols) ----------------------
 * QQQ: a curated set of large Invesco QQQ (Nasdaq-100) holdings, mid-2026.
 * Blue Chip: large NYSE-listed companies that aren't in QQQ. Edit freely. */
#define GRID_COLS 5
#define GRID_ROWS 5
#define GRID_MAX  (GRID_COLS * GRID_ROWS)

typedef struct {
    const char *name;
    const char *syms[GRID_MAX + 1];
} sym_group_t;

static const sym_group_t GROUPS[] = {
    { "QQQ 1", { "NVDA", "AAPL", "MSFT", "GOOGL", "AMZN", "META", "AVGO", "TSLA", "MU", "AMD",
                 "WMT", "INTC", "NFLX", "COST", "PLTR", "CSCO", "ASML", "LRCX", "AMAT", "TMUS",
                 "PEP", "QCOM", "TXN", "ADI", "KLAC", NULL } },
    { "QQQ 2", { "GOOG", "ISRG", "LIN", "INTU", "AMGN", "ARM", "GILD", "SHOP", "APP", "BKNG",
                 "PANW", "CRWD", "HON", "ADBE", "ADP", "SBUX", "MELI", "CMCSA", "VRTX", "REGN",
                 "MRVL", "CDNS", "SNPS", "ABNB", "PYPL", NULL } },
    { "Blue Chip", { "JPM", "V", "MA", "BAC", "GS", "JNJ", "UNH", "LLY", "MRK", "ABBV",
                     "PG", "KO", "HD", "MCD", "NKE", "DIS", "IBM", "ORCL", "CRM", "VZ",
                     "XOM", "CVX", "CAT", "BA", "AXP", NULL } },
};
#define GROUP_COUNT ((int)(sizeof(GROUPS) / sizeof(GROUPS[0])))
#define OTHER_TAB   GROUP_COUNT          /* watchlist symbols that aren't in any group */

/* One snapshot shared by the screens (only one is open at a time). It's about 6 KB, so it is
 * malloc'd once (ESP-IDF puts allocations that big in PSRAM) instead of taking internal RAM. */
static stocks_snapshot_t *s_snap_p;
#define s_snap (*s_snap_p)
static uint32_t          s_toast_seen;   /* last add/remove result shown as a toast */

static void show_new_toast(void)
{
    if (s_snap.toast_ver != s_toast_seen) {
        s_toast_seen = s_snap.toast_ver;
        if (s_snap.toast[0]) ui_toast(s_snap.toast);
    }
}

static bool in_list(const char *sym)
{
    for (int i = 0; i < s_snap.nsyms; i++) if (!strcmp(s_snap.syms[i], sym)) return true;
    return false;
}

static bool pending(const char *sym)
{
    for (int i = 0; i < s_snap.npend; i++) if (!strcmp(s_snap.pend[i], sym)) return true;
    return false;
}

/* 1234.5 -> "1,234.50"; under 1 -> 4 decimals */
static void fmt_price(double v, char *out, size_t len)
{
    if (v < 1.0 && v > -1.0) { snprintf(out, len, "%.4f", v); return; }
    char raw[32];
    snprintf(raw, sizeof(raw), "%.2f", v < 0 ? -v : v);
    char *dot = strchr(raw, '.');
    int ilen = (int)(dot - raw);
    size_t n = 0;
    if (v < 0 && n + 1 < len) out[n++] = '-';
    for (int i = 0; i < ilen && n + 1 < len; i++) {
        out[n++] = raw[i];
        int left = ilen - i - 1;
        if (left > 0 && left % 3 == 0 && n + 1 < len) out[n++] = ',';
    }
    snprintf(out + n, len - n, "%s", dot);
}

static void fmt_age(int64_t us, char *out, size_t len)
{
    int s = (int)(us / 1000000);
    if (s < 60)        snprintf(out, len, "%ds ago", s < 0 ? 0 : s);
    else if (s < 3600) snprintf(out, len, "%dm ago", s / 60);
    else               snprintf(out, len, "%dh ago", s / 3600);
}

static void fmt_clock(time_t t, char *out, size_t len)
{
    struct tm tm;
    localtime_r(&t, &tm);
#if APP_CLOCK_24H
    snprintf(out, len, "%02d:%02d", tm.tm_hour, tm.tm_min);
#else
    int h = tm.tm_hour % 12;
    snprintf(out, len, "%d:%02d %s", h ? h : 12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
#endif
}

#define BULLET " \xE2\x80\xA2 "

/* Status line for the Watchlist screen. */
static lv_color_t status_text(char *buf, size_t len)
{
    char age[24] = "";
    if (s_snap.updated_us) fmt_age(esp_timer_get_time() - s_snap.updated_us, age, sizeof(age));

    switch (s_snap.state) {
    case STOCKS_OFF:
    case STOCKS_LOADING:
        snprintf(buf, len, "Loading...");
        return ui_pal.text_dim;
    case STOCKS_NOT_SET:
    case STOCKS_WAIT_WIFI:
        snprintf(buf, len, "%s", s_snap.note);
        return ui_pal.warn;
    case STOCKS_ERROR:
        if (s_snap.busy) { snprintf(buf, len, "Retrying..."); return ui_pal.text_dim; }
        if (age[0]) snprintf(buf, len, "%s" BULLET "%s", s_snap.note, age);
        else        snprintf(buf, len, "%s", s_snap.note);
        return ui_pal.err;
    case STOCKS_OK:
    default:
        if (s_snap.busy) { snprintf(buf, len, "Updating..."); return ui_pal.text_dim; }
        if (!s_snap.auto_refresh) {
            snprintf(buf, len, "Paused" BULLET "updated %s", age);
            return ui_pal.warn;
        }
        if (s_snap.updated_at) {
            char clk[16];
            fmt_clock(s_snap.updated_at, clk, sizeof(clk));
            snprintf(buf, len, "Updated %s", clk);
        } else {
            snprintf(buf, len, "Updated %s", age);
        }
        return ui_pal.text_dim;
    }
}

/* ============================================================== Watchlist screen */
static lv_obj_t *pick_build(void *arg);
static lv_obj_t *server_build(void *arg);

typedef struct {
    lv_obj_t *row, *sym, *name, *price, *pill, *chg;
} quote_row_t;

typedef struct {
    lv_obj_t   *scr, *list, *status, *auto_btn, *auto_icon, *msg;
    lv_timer_t *timer;
    quote_row_t rows[STOCKS_MAX];
    int         nrows;
    uint32_t    ver;
    stocks_sym_t shown[STOCKS_MAX];   /* symbols the rows were built for */
} wl_ui_t;

static wl_ui_t w;

static void do_remove(void *user)
{
    const char *sym = user;
    stocks_tap_t r = stocks_remove(sym);
    if (r == STOCKS_TAP_FULL) ui_toast("Busy - try again in a moment");
}

static void row_long_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= w.nrows) return;
    static stocks_sym_t sym;                 /* outlives the dialog */
    static char msg[80];
    strlcpy(sym, w.shown[i], sizeof(sym));
    if (pending(sym)) return;
    snprintf(msg, sizeof(msg), "Take %s off your watchlist?", sym);
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_wait_release(indev);
    ui_confirm("Remove symbol?", msg, "Remove", true, do_remove, sym);
}

static quote_row_t make_row(int i)
{
    quote_row_t r;
    r.row = lv_obj_create(w.list);
    lv_obj_remove_style_all(r.row);
    lv_obj_set_size(r.row, UI_LIST_WIDTH, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(r.row, 60, 0);
    lv_obj_set_style_radius(r.row, 20, 0);
    lv_obj_set_style_bg_color(r.row, ui_pal.card, 0);
    lv_obj_set_style_bg_opa(r.row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r.row, ui_pal.card_press, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(r.row, 16, 0);
    lv_obj_set_style_pad_ver(r.row, 8, 0);
    lv_obj_set_style_pad_column(r.row, 10, 0);
    lv_obj_set_flex_flow(r.row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r.row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(r.row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r.row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r.row, row_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

    lv_obj_t *left = lv_obj_create(r.row);
    lv_obj_remove_style_all(left);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 2, 0);
    r.sym = ui_label(left, &lv_font_montserrat_20, ui_pal.text, "");
    r.name = ui_label(left, &lv_font_montserrat_14, ui_pal.text_dim, "");
    lv_obj_set_size(r.name, 150, lv_font_get_line_height(&lv_font_montserrat_14));   /* one line, then "..." */
    lv_label_set_long_mode(r.name, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *right = lv_obj_create(r.row);
    lv_obj_remove_style_all(right);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(right, 4, 0);
    r.price = ui_label(right, &lv_font_montserrat_20, ui_pal.text, "");
    r.pill = lv_obj_create(right);
    lv_obj_remove_style_all(r.pill);
    lv_obj_remove_flag(r.pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r.pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(r.pill, 8, 0);
    lv_obj_set_style_bg_opa(r.pill, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(r.pill, 8, 0);
    lv_obj_set_style_pad_ver(r.pill, 2, 0);
    r.chg = ui_label(r.pill, &lv_font_montserrat_14, lv_color_white(), "");
    return r;
}

static void fill_row(quote_row_t *r, const stocks_quote_t *q)
{
    lv_label_set_text(r->sym, q->sym);
    char name[3 * STOCKS_NAME_LEN];
    text_to_ascii(q->name, name, sizeof(name));          /* "Nestlé" -> "Nestle": the font is ASCII */
    lv_label_set_text(r->name, name[0] ? name : (q->has_price ? "" : "Unknown symbol?"));
    char buf[48];
    if (q->has_price) {
        fmt_price(q->price, buf, sizeof(buf));
        lv_label_set_text(r->price, buf);
        lv_obj_set_style_text_color(r->price, ui_pal.text, 0);
        lv_obj_set_style_text_font(r->price, &lv_font_montserrat_20, 0);
    } else {
        lv_label_set_text(r->price, "no quote");
        lv_obj_set_style_text_color(r->price, ui_pal.text_dim, 0);
        lv_obj_set_style_text_font(r->price, &lv_font_montserrat_16, 0);
    }
    if (q->has_price && (q->has_change || q->has_pct)) {
        double v = q->has_change ? q->change : q->pct;
        lv_color_t c = v > 0 ? ui_pal.ok : (v < 0 ? ui_pal.err : lv_color_hex(UI_C_GRAY));
        if (q->has_change && q->has_pct) snprintf(buf, sizeof(buf), "%+.2f  %+.2f%%", q->change, q->pct);
        else if (q->has_change)          snprintf(buf, sizeof(buf), "%+.2f", q->change);
        else                             snprintf(buf, sizeof(buf), "%+.2f%%", q->pct);
        lv_label_set_text(r->chg, buf);
        lv_obj_set_style_bg_color(r->pill, c, 0);
        lv_obj_remove_flag(r->pill, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(r->pill, LV_OBJ_FLAG_HIDDEN);
    }
    /* a row whose removal is in progress fades */
    lv_obj_set_style_bg_opa(r->row, pending(q->sym) ? LV_OPA_40 : LV_OPA_COVER, 0);
}

static void set_message(const char *title, const char *text)
{
    if (!title) {
        if (w.msg) { lv_obj_delete(w.msg); w.msg = NULL; }
        return;
    }
    if (!w.msg) {
        w.msg = lv_obj_create(w.list);
        lv_obj_remove_style_all(w.msg);
        lv_obj_remove_flag(w.msg, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(w.msg, 300, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(w.msg, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(w.msg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(w.msg, 8, 0);
        lv_obj_set_style_pad_top(w.msg, 46, 0);
        ui_icon(w.msg, &ui_icons_56, ui_pal.text_dim, ICON_CHART);
        lv_obj_t *t = ui_label(w.msg, &lv_font_montserrat_20, ui_pal.text, "");
        lv_obj_t *m = ui_label(w.msg, &lv_font_montserrat_14, ui_pal.text_dim, "");
        lv_obj_set_width(m, LV_PCT(100));
        lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
        (void)t;
    }
    lv_label_set_text(lv_obj_get_child(w.msg, 1), title);
    lv_label_set_text(lv_obj_get_child(w.msg, 2), text);
}

static void style_auto_button(void)
{
    bool on = s_snap.auto_refresh;
    lv_obj_set_style_bg_color(w.auto_btn, on ? lv_color_hex(UI_C_GREEN) : ui_pal.card, 0);
    lv_obj_set_style_text_color(w.auto_icon, on ? lv_color_white() : ui_pal.warn, 0);
    lv_label_set_text(w.auto_icon, on ? ICON_ROTATE : ICON_PAUSE);
}

static void wl_update_status(void)
{
    char buf[140];
    lv_color_t c = status_text(buf, sizeof(buf));
    lv_label_set_text(w.status, buf);
    lv_obj_set_style_text_color(w.status, c, 0);
}

static void wl_update(void)
{
    stocks_get(&s_snap);
    w.ver = stocks_version();
    style_auto_button();
    wl_update_status();
    show_new_toast();

    /* rebuild the rows only when the symbols changed; otherwise just update the text */
    bool same = s_snap.nrows == w.nrows;
    for (int i = 0; same && i < w.nrows; i++) same = !strcmp(w.shown[i], s_snap.rows[i].sym);
    if (!same) {
        int32_t y = lv_obj_get_scroll_y(w.list);
        for (int i = 0; i < w.nrows; i++) lv_obj_delete(w.rows[i].row);
        w.nrows = 0;
        for (int i = 0; i < s_snap.nrows; i++) {
            w.rows[i] = make_row(i);
            strlcpy(w.shown[i], s_snap.rows[i].sym, sizeof(stocks_sym_t));
            w.nrows++;
        }
        lv_obj_update_layout(w.list);
        lv_obj_scroll_to_y(w.list, y, LV_ANIM_OFF);
    }
    for (int i = 0; i < w.nrows; i++) fill_row(&w.rows[i], &s_snap.rows[i]);

    if (w.nrows) set_message(NULL, NULL);
    else if (s_snap.state == STOCKS_NOT_SET)
        set_message("Server not set", "Tap the gear to enter your FusionStockWatchListMCP URL and API key.");
    else if (s_snap.state == STOCKS_ERROR && !s_snap.quotes_loaded)
        set_message("Can't load quotes", s_snap.note);
    else if (s_snap.quotes_loaded)
        set_message("Your watchlist is empty", "Tap + or swipe left to pick symbols.");
    else
        set_message("Loading watchlist", s_snap.state == STOCKS_WAIT_WIFI ? "Waiting for Wi-Fi..." : "");
    lv_obj_update_layout(w.list);
    lv_obj_send_event(w.list, LV_EVENT_SCROLL, NULL);     /* re-apply the edge fade */
}

static void wl_timer_cb(lv_timer_t *t)
{
    static int ticks;
    if (stocks_version() != w.ver) wl_update();
    else if (++ticks % 4 == 0) wl_update_status();      /* "updated 12s ago" keeps counting */
}

static void wl_delete_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != w.scr) return;
    if (w.timer) lv_timer_delete(w.timer);
    memset(&w, 0, sizeof(w));
}

static void auto_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    bool on = !stocks_auto();
    app_settings_t cfg;
    settings_get(&cfg);
    cfg.stocks_auto = on;
    settings_set(&cfg);
    stocks_set_auto(on);              /* on = refresh now, then every minute */
    ui_toast(on ? "Auto refresh on" : "Auto refresh paused");
    wl_update();
}

static void pick_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    ui_nav_push(pick_build, NULL, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

static void gear_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    ui_nav_push(server_build, NULL, LV_SCREEN_LOAD_ANIM_MOVE_TOP);
}

static void wl_gesture_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev || ui_nav_busy()) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT) {
        lv_indev_wait_release(indev);
        ui_nav_push(pick_build, NULL, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
    } else if (dir == LV_DIR_RIGHT) {
        lv_indev_wait_release(indev);
        ui_nav_home(LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
    }
}

static lv_obj_t *bottom_button(lv_obj_t *bar, const char *icon, lv_color_t bg, lv_color_t fg, lv_event_cb_t cb)
{
    lv_obj_t *b = ui_round_button(bar, icon, &ui_icons_20, 52, bg, fg);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static lv_obj_t *watchlist_build(void *arg)
{
    if (w.timer) lv_timer_delete(w.timer);   /* rebuilt in place (theme change): drop the old timer */
    memset(&w, 0, sizeof(w));
    lv_obj_t *scr;
    w.list = ui_list_screen(&scr, "Stocks", true);
    w.scr = scr;
    lv_obj_set_user_data(scr, UI_CUSTOM_GESTURES);
    lv_obj_add_event_cb(scr, wl_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_set_style_pad_top(w.list, 112, 0);

    /* a taller top fade, so rows scrolling up don't show through the status line */
    lv_obj_t *fade = lv_obj_create(scr);
    lv_obj_remove_style_all(fade);
    lv_obj_remove_flag(fade, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(fade, UI_SCREEN_SIZE, 126);
    lv_obj_align(fade, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(fade, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(fade, ui_pal.bg, 0);
    lv_obj_set_style_bg_grad_color(fade, ui_pal.bg, 0);
    lv_obj_set_style_bg_grad_dir(fade, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_opa(fade, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_opa(fade, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_main_stop(fade, 205, 0);
    lv_obj_set_style_bg_grad_stop(fade, 255, 0);
    lv_obj_move_to_index(fade, 1);            /* just above the list, below the header */

    w.status = ui_label(scr, &lv_font_montserrat_14, ui_pal.text_dim, "");
    lv_obj_set_width(w.status, 270);
    lv_obj_set_style_text_align(w.status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(w.status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(w.status, LV_ALIGN_TOP_MID, 0, 84);

    /* bottom: [auto refresh] [+] [gear] */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 18, 0);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -30);
    w.auto_btn = bottom_button(bar, ICON_ROTATE, ui_pal.card, ui_pal.text, auto_cb);
    w.auto_icon = lv_obj_get_child(w.auto_btn, 0);
    bottom_button(bar, ICON_PLUS, ui_pal.accent, lv_color_white(), pick_cb);
    bottom_button(bar, ICON_GEAR, ui_pal.card, ui_pal.text_dim, gear_cb);

    w.ver = 0;
    wl_update();
    w.timer = lv_timer_create(wl_timer_cb, 250, NULL);
    lv_obj_add_event_cb(scr, wl_delete_cb, LV_EVENT_DELETE, NULL);
    return scr;
}

/* ============================================================== Pick screen */
typedef struct {
    lv_obj_t   *scr, *tabs[GROUP_COUNT + 1], *grid, *status;
    lv_obj_t   *btn[GRID_MAX];
    const char *btn_sym[GRID_MAX];
    int         nbtn;
    lv_timer_t *timer;
    uint32_t    ver;
    char        other_key[STOCKS_MAX * (STOCKS_SYM_LEN + 1) + 1];   /* what the Other tab shows */
    stocks_sym_t other[GRID_MAX];
} pick_ui_t;

static pick_ui_t p;
static int       s_tab;                  /* remembered while the app is open */

static void style_sym_button(int i)
{
    lv_obj_t *b = p.btn[i];
    const char *sym = p.btn_sym[i];
    if (!sym) return;
    bool pend = pending(sym), on = in_list(sym);
    lv_color_t bg = pend ? lv_color_hex(UI_C_ORANGE) : (on ? lv_color_hex(UI_C_GREEN) : ui_pal.card);
    lv_color_t fg = (pend || on) ? lv_color_white() : ui_pal.text;
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 0), fg, 0);
}

static void sym_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= p.nbtn || !p.btn_sym[i]) return;
    switch (stocks_toggle(p.btn_sym[i])) {
    case STOCKS_TAP_NOT_LOADED: ui_toast("Still loading the watchlist..."); break;
    case STOCKS_TAP_FULL:       ui_toast("Busy - try again in a moment"); break;
    default: break;
    }
    stocks_get(&s_snap);
    style_sym_button(i);
}

static lv_obj_t *sym_button(const char *text, int i)
{
    lv_obj_t *b = lv_obj_create(p.grid);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 62, 40);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, ui_pal.card, 0);
    lv_obj_set_style_transform_scale(b, 235, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(b, 31, 0);
    lv_obj_set_style_transform_pivot_y(b, 20, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = ui_label(b, strlen(text) > 4 ? &lv_font_montserrat_14 : &lv_font_montserrat_16, ui_pal.text, text);
    lv_obj_center(l);
    if (i >= 0) lv_obj_add_event_cb(b, sym_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    return b;
}

/* Watchlist symbols that aren't on any preset tab ("+N" if more than fit). */
static int other_symbols(stocks_sym_t *out, int max, int *extra, char *key, size_t key_len)
{
    int n = 0, total = 0;
    key[0] = '\0';
    for (int i = 0; i < s_snap.nsyms; i++) {
        const char *sym = s_snap.syms[i];
        bool preset = false;
        for (int g = 0; g < GROUP_COUNT && !preset; g++)
            for (int k = 0; GROUPS[g].syms[k] && !preset; k++) preset = !strcmp(GROUPS[g].syms[k], sym);
        bool dup = false;
        for (int j = 0; j < n; j++) if (!strcmp(out[j], sym)) dup = true;
        if (preset || dup) continue;
        total++;
        if (n < max) strlcpy(out[n++], sym, sizeof(stocks_sym_t));
        strlcat(key, sym, key_len);
        strlcat(key, ",", key_len);
    }
    *extra = total - n;
    return n;
}

static void build_grid(void)
{
    lv_obj_clean(p.grid);
    memset(p.btn, 0, sizeof(p.btn));
    memset(p.btn_sym, 0, sizeof(p.btn_sym));
    p.nbtn = 0;
    for (int t = 0; t <= GROUP_COUNT; t++) {
        bool on = t == s_tab;
        lv_obj_set_style_bg_color(p.tabs[t], on ? ui_pal.accent : ui_pal.card, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(p.tabs[t], 0), on ? lv_color_white() : ui_pal.text, 0);
    }

    if (s_tab < GROUP_COUNT) {
        for (int k = 0; GROUPS[s_tab].syms[k] && p.nbtn < GRID_MAX; k++) {
            p.btn_sym[p.nbtn] = GROUPS[s_tab].syms[k];
            p.btn[p.nbtn] = sym_button(GROUPS[s_tab].syms[k], p.nbtn);
            p.nbtn++;
        }
    } else {
        int extra;
        int n = other_symbols(p.other, GRID_MAX, &extra, p.other_key, sizeof(p.other_key));
        if (extra > 0) n = GRID_MAX - 1;              /* last slot says "+N more" */
        for (int i = 0; i < n; i++) {
            p.btn_sym[p.nbtn] = p.other[i];
            p.btn[p.nbtn] = sym_button(p.other[i], p.nbtn);
            p.nbtn++;
        }
        if (extra > 0) {
            char more[16];
            snprintf(more, sizeof(more), "+%d", extra + 1);
            lv_obj_t *b = sym_button(more, -1);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
        }
        if (n == 0) {
            lv_obj_t *l = ui_label(p.grid, &lv_font_montserrat_16, ui_pal.text_dim,
                                   s_snap.list_loaded ? "Symbols added elsewhere (for example from Claude) "
                                                        "that aren't on the other tabs show up here."
                                                      : "Loading...");
            lv_obj_set_width(l, 300);
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_pad_top(l, 60, 0);
        }
    }
    for (int i = 0; i < p.nbtn; i++) style_sym_button(i);
}

static void pick_status(void)
{
    char buf[120];
    lv_color_t c = ui_pal.text_dim;
    if (s_snap.npend)                                        snprintf(buf, sizeof(buf), "Saving %d...", s_snap.npend);
    else if (!s_snap.list_loaded && s_snap.state == STOCKS_ERROR) { snprintf(buf, sizeof(buf), "%s", s_snap.note); c = ui_pal.err; }
    else if (!s_snap.list_loaded)                            snprintf(buf, sizeof(buf), "Loading watchlist...");
    else snprintf(buf, sizeof(buf), "%d symbol%s in your watchlist", s_snap.nsyms, s_snap.nsyms == 1 ? "" : "s");
    lv_label_set_text(p.status, buf);
    lv_obj_set_style_text_color(p.status, c, 0);
}

static void pick_update(void)
{
    stocks_get(&s_snap);
    p.ver = stocks_version();
    show_new_toast();
    if (s_tab == OTHER_TAB) {
        /* rebuild from the timer (never from a button's own event) when its contents change */
        stocks_sym_t tmp[GRID_MAX];
        char key[sizeof(p.other_key)];
        int extra;
        other_symbols(tmp, GRID_MAX, &extra, key, sizeof(key));
        if (strcmp(key, p.other_key) != 0 || (p.nbtn == 0 && s_snap.list_loaded)) build_grid();
    }
    for (int i = 0; i < p.nbtn; i++) style_sym_button(i);
    pick_status();
}

static void pick_timer_cb(lv_timer_t *t)
{
    if (stocks_version() != p.ver) pick_update();
}

static void pick_delete_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != p.scr) return;
    if (p.timer) lv_timer_delete(p.timer);
    memset(&p, 0, sizeof(p));
}

static void tab_cb(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    if (t == s_tab) return;
    s_tab = t;
    stocks_get(&s_snap);
    build_grid();
}

static void pick_back_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    ui_nav_pop(LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
}

static void pick_gesture_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev || ui_nav_busy()) return;
    if (lv_indev_get_gesture_dir(indev) != LV_DIR_RIGHT) return;
    lv_indev_wait_release(indev);            /* the touch was a swipe, not a tap on a symbol */
    ui_nav_pop(LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
}

static lv_obj_t *pick_build(void *arg)
{
    if (p.timer) lv_timer_delete(p.timer);
    memset(&p, 0, sizeof(p));
    stocks_get(&s_snap);
    lv_obj_t *scr = ui_screen();
    p.scr = scr;
    lv_obj_add_event_cb(scr, pick_gesture_cb, LV_EVENT_GESTURE, NULL);

    /* header: [<] Watchlist */
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_remove_style_all(hdr);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(hdr, LV_SIZE_CONTENT, 44);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 10, 0);
    lv_obj_t *back = ui_round_button(hdr, ICON_LEFT, &ui_icons_16, 40, ui_pal.card, ui_pal.text);
    lv_obj_add_event_cb(back, pick_back_cb, LV_EVENT_CLICKED, NULL);
    ui_label(hdr, &lv_font_montserrat_24, ui_pal.text, "Pick symbols");
    lv_obj_t *sp = lv_obj_create(hdr);
    lv_obj_remove_style_all(sp);
    lv_obj_set_size(sp, 40, 40);

    /* tabs */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(bar, LV_SIZE_CONTENT, 36);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 90);
    for (int t = 0; t <= GROUP_COUNT; t++) {
        lv_obj_t *b = lv_obj_create(bar);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, t == 2 ? 92 : 72, 36);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(b, 4);
        lv_obj_t *l = ui_label(b, &lv_font_montserrat_14, ui_pal.text, t < GROUP_COUNT ? GROUPS[t].name : "Other");
        lv_obj_center(l);
        lv_obj_add_event_cb(b, tab_cb, LV_EVENT_PRESSED, (void *)(intptr_t)t);
        p.tabs[t] = b;
    }

    /* 5 x 5 symbol grid */
    p.grid = lv_obj_create(scr);
    lv_obj_remove_style_all(p.grid);
    lv_obj_remove_flag(p.grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(p.grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(p.grid, GRID_COLS * 62 + (GRID_COLS - 1) * 4, GRID_ROWS * 40 + (GRID_ROWS - 1) * 6);
    lv_obj_align(p.grid, LV_ALIGN_TOP_MID, 0, 138);
    lv_obj_set_flex_flow(p.grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(p.grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(p.grid, 4, 0);
    lv_obj_set_style_pad_row(p.grid, 6, 0);

    p.status = ui_label(scr, &lv_font_montserrat_14, ui_pal.text_dim, "");
    lv_obj_set_width(p.status, 280);
    lv_obj_set_style_text_align(p.status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(p.status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(p.status, LV_ALIGN_TOP_MID, 0, 374);

    lv_obj_t *hint = ui_label(scr, &lv_font_montserrat_12, ui_pal.text_dim, "Tap to add or remove");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 398);

    if (s_tab > GROUP_COUNT) s_tab = 0;
    build_grid();
    pick_status();
    p.ver = stocks_version();
    p.timer = lv_timer_create(pick_timer_cb, 200, NULL);
    lv_obj_add_event_cb(scr, pick_delete_cb, LV_EVENT_DELETE, NULL);
    return scr;
}

/* ============================================================== Stock server screen */
static lv_obj_t   *s_sv_scr;
static ui_row_t    s_sv_url, s_sv_key, s_sv_status;
static lv_timer_t *s_sv_timer;

static void sv_refresh(void)
{
    app_settings_t cfg;
    settings_get(&cfg);
    lv_label_set_text(s_sv_url.value, cfg.mcp_url[0] ? cfg.mcp_url : "Not set");
    char masked[48];
    ui_mask_secret(cfg.mcp_api_key, masked, sizeof(masked), true);
    lv_label_set_text(s_sv_key.value, masked);

    stocks_get(&s_snap);
    show_new_toast();
    char buf[140];
    uint32_t c;
    if (s_snap.busy)                       { snprintf(buf, sizeof(buf), "Working..."); c = UI_C_ORANGE; }
    else if (s_snap.state == STOCKS_OK)    { snprintf(buf, sizeof(buf), "Connected" BULLET "%d symbols", s_snap.nsyms); c = UI_C_GREEN; }
    else if (s_snap.state == STOCKS_ERROR || s_snap.state == STOCKS_NOT_SET) { snprintf(buf, sizeof(buf), "%s", s_snap.note); c = UI_C_RED; }
    else if (s_snap.state == STOCKS_WAIT_WIFI) { snprintf(buf, sizeof(buf), "Waiting for Wi-Fi..."); c = UI_C_ORANGE; }
    else                                   { snprintf(buf, sizeof(buf), "Connecting..."); c = UI_C_ORANGE; }
    lv_label_set_text(s_sv_status.value, buf);
    lv_obj_set_style_bg_color(lv_obj_get_child(s_sv_status.row, 0), lv_color_hex(c), 0);
}

static void sv_timer_cb(lv_timer_t *t) { sv_refresh(); }

static void sv_delete_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_sv_scr) return;
    if (s_sv_timer) lv_timer_delete(s_sv_timer);
    s_sv_timer = NULL;
    s_sv_scr = NULL;
}

static bool sv_url_saved(const char *text, void *user)
{
    if (strncmp(text, "https://", 8) != 0 && strncmp(text, "http://", 7) != 0) {
        ui_toast("Start the URL with https://");
        return true;                       /* stay in the editor */
    }
    app_settings_t cfg;
    settings_get(&cfg);
    strlcpy(cfg.mcp_url, text, sizeof(cfg.mcp_url));
    settings_set(&cfg);
    stocks_config_changed();
    ui_toast("Saved - reloading");
    return false;
}

static bool sv_key_saved(const char *text, void *user)
{
    app_settings_t cfg;
    settings_get(&cfg);
    strlcpy(cfg.mcp_api_key, text, sizeof(cfg.mcp_api_key));
    settings_set(&cfg);
    stocks_config_changed();
    ui_toast("Saved - reloading");
    return false;
}

static void sv_url_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    app_settings_t cfg;
    settings_get(&cfg);
    ui_editor_cfg_t ed = {
        .title = "MCP server URL", .icon = ICON_LINK, .initial = cfg.mcp_url,
        .placeholder = "https://host:4443/FusionStockWatchListMCP", .max_len = SET_URL_MAX,
        .on_save = sv_url_saved,
    };
    ui_editor_open(&ed, false);
}

static void sv_key_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    app_settings_t cfg;
    settings_get(&cfg);
    ui_editor_cfg_t ed = {
        .title = "API key", .icon = ICON_KEY, .initial = cfg.mcp_api_key,
        .placeholder = "x-api-key", .max_len = SET_KEY_MAX, .password = true, .on_save = sv_key_saved,
    };
    ui_editor_open(&ed, false);
}

static void sv_refresh_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    stocks_refresh_now();
    ui_toast("Refreshing");
}

static void sv_reset_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    app_settings_t cfg;
    settings_get(&cfg);
    strlcpy(cfg.mcp_url, settings_default_mcp_url(), sizeof(cfg.mcp_url));
    settings_set(&cfg);
    stocks_config_changed();
    sv_refresh();
    ui_toast("URL reset to secrets.h");
}

static lv_obj_t *server_build(void *arg)
{
    lv_obj_t *scr;
    lv_obj_t *list = ui_list_screen(&scr, "Stock server", true);
    s_sv_scr = scr;

    ui_section(list, "FUSION MCP SERVER");
    s_sv_url = ui_row(list, ICON_LINK, UI_C_BLUE, "Server URL", "", sv_url_cb, NULL);
    lv_obj_set_height(s_sv_url.value, 2 * lv_font_get_line_height(&lv_font_montserrat_14));
    s_sv_key = ui_row(list, ICON_KEY, UI_C_ORANGE, "API key", "", sv_key_cb, NULL);
    s_sv_status = ui_row(list, ICON_PLUG, UI_C_ORANGE, "Status", "", NULL, NULL);
    lv_label_set_long_mode(s_sv_status.value, LV_LABEL_LONG_MODE_WRAP);

    ui_section(list, "ACTIONS");
    ui_row(list, ICON_ROTATE, UI_C_TEAL, "Refresh now", NULL, sv_refresh_cb, NULL);
    ui_row(list, ICON_RESET, UI_C_GRAY, "Reset URL", "Back to secrets.h", sv_reset_cb, NULL);

    sv_refresh();
    if (s_sv_timer) lv_timer_delete(s_sv_timer);
    s_sv_timer = lv_timer_create(sv_timer_cb, 300, NULL);
    lv_obj_add_event_cb(scr, sv_delete_cb, LV_EVENT_DELETE, NULL);
    return scr;
}

/* ============================================================== app descriptor */
static void stocks_enter(void)
{
    if (!s_snap_p) s_snap_p = calloc(1, sizeof(*s_snap_p));
    stocks_set_active(true);
    stocks_get(&s_snap);
    s_toast_seen = s_snap.toast_ver;     /* don't replay a result from the last visit */
}

static void stocks_leave(void) { stocks_set_active(false); }

const ui_app_t APP_STOCKS = {
    .name = "Stocks",
    .icon = ICON_CHART,
    .color1 = C_STOCKS_1,
    .color2 = C_STOCKS_2,
    .build = watchlist_build,
    .on_enter = stocks_enter,
    .on_leave = stocks_leave,
};
