/* Quotes app: a second stocks app, for the Stock API (plain REST: GET /quote and GET /watchlist).
 * The Stocks app (app_stocks.c) is the MCP version; both stay on Home.
 *
 *   Quotes (menu)  Quote / Watchlist / Stock API
 *   Get a quote    tabs of symbols; tap one -> Quote screen (GET /quote?symbol=..)
 *   Pick symbols   the same tabs; tap to select / deselect (green), Watchlist button ->
 *   Watchlist      quotes for the picked symbols (GET /watchlist?symbols=..), tap a row -> Quote
 *   Stock API      base URL, API key, status, test, reset URL
 *
 * The picked symbols are saved on the device (settings qt_syms), not on the server.
 * The network work happens in quotes_core.c; these screens only draw, polling the engine's
 * snapshot from LVGL timers, so they never freeze while a request is running. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_timer.h"

#include "apps.h"
#include "ui_widgets.h"
#include "quotes.h"
#include "settings_store.h"
#include "app_config.h"
#include "text_ascii.h"

#define C_QUOTES_1  0x2F8CFF
#define C_QUOTES_2  0x5E5CE6

/* ---- symbols on the picker screens: (tab name, up to 16 symbols) ------------------------
 * A 4 x 4 grid of big buttons (easier to hit than the Stocks app's 5 x 5), so each tab has the
 * 16 best-known names from the Stocks app's presets. Plus a "Picked" tab with your selection.
 * Layout, top to bottom: header (big back button), tabs, grid, status, Watchlist button. */
#define GRID_COLS 4
#define GRID_ROWS 4
#define GRID_MAX  (GRID_COLS * GRID_ROWS)
#define SYM_W     UI_SZ(72, 86)      /* symbol button size */
#define SYM_H     UI_SZ(50, 58)
#define SYM_GAP   8                  /* between buttons, both ways */
#define HDR_Y     UI_SZ(34, 18)      /* header row (back button, title, clear) */
#define HDR_BTN   UI_SZ(52, 56)      /* back / clear button size */
#define TABS_Y    UI_SZ(100, 88)
#define TABS_H    UI_SZ(40, 46)
#define GRID_TOP  UI_SZ(154, 148)    /* grid is GRID_ROWS * (SYM_H + SYM_GAP) - SYM_GAP tall */
#define STATUS_Y  UI_SZ(382, 410)
#define GO_Y      UI_SZ(404, 436)    /* Watchlist button */
#define GO_W      UI_SZ(170, 220)
#define GO_H      UI_SZ(44, 52)

typedef struct {
    const char *name;
    const char *syms[GRID_MAX + 1];
} sym_group_t;

static const sym_group_t GROUPS[] = {
    { "QQQ 1", { "NVDA", "AAPL", "MSFT", "GOOGL", "AMZN", "META", "AVGO", "TSLA",
                 "AMD", "NFLX", "COST", "INTC", "CSCO", "QCOM", "TXN", "PEP", NULL } },
    { "QQQ 2", { "ADBE", "INTU", "AMAT", "MU", "LRCX", "ASML", "PLTR", "ARM",
                 "PANW", "CRWD", "SHOP", "ABNB", "PYPL", "SBUX", "BKNG", "ISRG", NULL } },
    { "Blue Chip", { "JPM", "V", "MA", "BAC", "GS", "JNJ", "UNH", "LLY",
                     "PG", "KO", "HD", "MCD", "DIS", "IBM", "XOM", "T", NULL } },
};
#define GROUP_COUNT ((int)(sizeof(GROUPS) / sizeof(GROUPS[0])))
#define PICKED_TAB  GROUP_COUNT          /* the symbols picked for the watchlist */

#define MODE_QUOTE  0                    /* picker: tap = quote */
#define MODE_WATCH  1                    /* picker: tap = select / deselect */

#define BULLET " \xE2\x80\xA2 "

/* ---- app state: one block, malloc'd once (over 1 KB, so ESP-IDF puts it in PSRAM, not in the
 * internal RAM the display needs). Screens are rebuilt every time they show, so it only holds
 * what has to outlive them. */
typedef struct {
    lv_obj_t *row, *sym, *name, *price, *pill, *chg;
} quote_row_t;

typedef struct {
    qt_snapshot_t snap;

    stocks_sym_t sel[QT_SEL_MAX];        /* picked for the watchlist, in the order picked */
    int          nsel;
    bool         sel_loaded, sel_dirty;
    stocks_sym_t quote_sym;              /* the symbol on the Quote screen */
    int          tab[2];                 /* last tab per picker mode */

    struct {                             /* menu */
        lv_obj_t   *scr;
        ui_row_t    watch, api;
        lv_timer_t *timer;
        uint32_t    ver;
    } m;

    struct {                             /* picker */
        lv_obj_t   *scr, *tabs[GROUP_COUNT + 1], *grid, *status, *go, *go_lbl, *clear;
        lv_obj_t   *btn[GRID_MAX];
        stocks_sym_t btn_sym[GRID_MAX];
        int         nbtn, mode;
    } p;

    struct {                             /* watchlist */
        lv_obj_t   *scr, *list, *status, *msg;
        lv_timer_t *timer;
        quote_row_t rows[QT_SEL_MAX];
        int         nrows, ticks;
        uint32_t    ver;
    } w;

    struct {                             /* quote */
        lv_obj_t   *scr, *name, *price, *pill, *chg, *open_v, *prev_v, *range, *bar, *fill, *dot,
                   *lo, *hi, *status;
        lv_timer_t *timer;
        uint32_t    ver;
        int         ticks;
    } q;

    struct {                             /* Stock API server screen */
        lv_obj_t   *scr;
        ui_row_t    url, key, status, ep;
        lv_timer_t *timer;
    } sv;
} quotes_app_t;

static quotes_app_t *A;
#define S (A->snap)

static lv_obj_t *picker_build(void *arg);
static lv_obj_t *watchlist_build(void *arg);
static lv_obj_t *quote_build(void *arg);
static lv_obj_t *server_build(void *arg);

/* ================================================================== helpers */
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

static void load_selection(void)
{
    if (A->sel_loaded) return;
    SETTINGS_COPY(cfg);
    A->nsel = quotes_split_symbols(cfg->qt_syms, A->sel, QT_SEL_MAX);
    A->sel_loaded = true;
    A->sel_dirty = false;
}

static void save_selection(void)
{
    if (!A->sel_dirty) return;
    SETTINGS_COPY(cfg);
    cfg->qt_syms[0] = '\0';
    for (int i = 0; i < A->nsel; i++) {
        if (i) strlcat(cfg->qt_syms, ",", sizeof(cfg->qt_syms));
        strlcat(cfg->qt_syms, A->sel[i], sizeof(cfg->qt_syms));
    }
    settings_set(cfg);
    A->sel_dirty = false;
}

static int sel_index(const char *sym)
{
    for (int i = 0; i < A->nsel; i++) if (!strcmp(A->sel[i], sym)) return i;
    return -1;
}

/* "AAPL, INTC, TXN" (cut with "..." by the label) */
static void sel_summary(char *out, size_t len)
{
    out[0] = '\0';
    if (!A->nsel) { snprintf(out, len, "Nothing picked yet"); return; }
    for (int i = 0; i < A->nsel; i++) {
        if (i) strlcat(out, ", ", len);
        strlcat(out, A->sel[i], len);
    }
}

/* Header row: [<] title [right] (right may be NULL: an empty spacer keeps the title centered) */
static lv_obj_t *header(lv_obj_t *scr, const char *title, lv_event_cb_t back_cb, lv_obj_t **right_out,
                        const char *right_icon, lv_event_cb_t right_cb)
{
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_remove_style_all(hdr);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(hdr, LV_SIZE_CONTENT, HDR_BTN);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, HDR_Y);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 12, 0);
    lv_obj_t *back = ui_round_button(hdr, ICON_LEFT, UI_SZ(&ui_icons_20, &ui_icons_28), HDR_BTN, ui_pal.card, ui_pal.text);
    lv_obj_set_ext_click_area(back, 8);       /* a little extra margin around it */
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    ui_label(hdr, UI_F_TITLE, ui_pal.text, title);
    lv_obj_t *r;
    if (right_icon) {
        r = ui_round_button(hdr, right_icon, UI_SZ(&ui_icons_20, &ui_icons_28), HDR_BTN, ui_pal.card, ui_pal.text_dim);
        lv_obj_add_event_cb(r, right_cb, LV_EVENT_CLICKED, NULL);
    } else {
        r = lv_obj_create(hdr);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, HDR_BTN, HDR_BTN);
    }
    if (right_out) *right_out = r;
    return hdr;
}

static void pop_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    ui_nav_pop(LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
}

/* swipe right = back (screens built without ui_list_screen) */
static void back_gesture_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev || ui_nav_busy()) return;
    if (lv_indev_get_gesture_dir(indev) != LV_DIR_RIGHT) return;
    lv_indev_wait_release(indev);            /* the touch was a swipe, not a tap on a button */
    ui_nav_pop(LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
}

/* Status of the server connection for the menu and the Stock API screen. */
static uint32_t link_text(char *buf, size_t len)
{
    char age[24] = "";
    if (S.last_us) fmt_age(esp_timer_get_time() - S.last_us, age, sizeof(age));
    switch (S.link) {
    case QT_LINK_NOT_SET:   snprintf(buf, len, "Set the API URL"); return UI_C_RED;
    case QT_LINK_WAIT_WIFI: snprintf(buf, len, "Waiting for Wi-Fi..."); return UI_C_ORANGE;
    default: break;
    }
    if (S.busy)             { snprintf(buf, len, "Working..."); return UI_C_ORANGE; }
    if (!S.last_us)         { snprintf(buf, len, "Ready" BULLET "no requests yet"); return UI_C_GRAY; }
    if (S.last_ok)          { snprintf(buf, len, "OK" BULLET "HTTP %d" BULLET "%s", S.last_http, age); return UI_C_GREEN; }
    snprintf(buf, len, "%s", S.last_note);
    return UI_C_RED;
}

/* ================================================================== menu (the app's first screen) */
static void menu_quote_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    ui_nav_push(picker_build, (void *)(intptr_t)MODE_QUOTE, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

static void menu_watch_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    ui_nav_push(picker_build, (void *)(intptr_t)MODE_WATCH, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

static void menu_api_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    ui_nav_push(server_build, NULL, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

static void menu_update(void)
{
    quotes_get(&S);
    A->m.ver = quotes_version();
    char buf[140];
    uint32_t c = link_text(buf, sizeof(buf));
    lv_label_set_text(A->m.api.value, buf);
    lv_obj_set_style_text_color(A->m.api.value, c == UI_C_RED ? ui_pal.err : ui_pal.text_dim, 0);
}

static void menu_timer_cb(lv_timer_t *t)
{
    static int ticks;
    if (quotes_version() != A->m.ver || ++ticks % 8 == 0) menu_update();   /* "12s ago" keeps counting */
}

static void menu_delete_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != A->m.scr) return;
    if (A->m.timer) lv_timer_delete(A->m.timer);
    memset(&A->m, 0, sizeof(A->m));
}

static lv_obj_t *menu_build(void *arg)
{
    if (A->m.timer) lv_timer_delete(A->m.timer);   /* rebuilt in place (theme change): drop the old timer */
    memset(&A->m, 0, sizeof(A->m));
    load_selection();

    lv_obj_t *scr;
    lv_obj_t *list = ui_list_screen(&scr, "Quotes", true);
    A->m.scr = scr;

    ui_section(list, "STOCK API");
    ui_row(list, ICON_CHART, UI_C_GREEN, "Get a quote", "Tap a symbol to see its quote", menu_quote_cb, NULL);
    char buf[QT_SEL_MAX * (STOCKS_SYM_LEN + 2) + 8];
    sel_summary(buf, sizeof(buf));
    A->m.watch = ui_row(list, ICON_LIST, UI_C_BLUE, "Watchlist", buf, menu_watch_cb, NULL);
    lv_label_set_long_mode(A->m.watch.value, LV_LABEL_LONG_MODE_DOTS);

    ui_section(list, "SERVER");
    A->m.api = ui_row(list, ICON_SERVER, UI_C_ORANGE, "Stock API", "", menu_api_cb, NULL);
    lv_label_set_long_mode(A->m.api.value, LV_LABEL_LONG_MODE_DOTS);

    menu_update();
    A->m.timer = lv_timer_create(menu_timer_cb, 250, NULL);
    lv_obj_add_event_cb(scr, menu_delete_cb, LV_EVENT_DELETE, NULL);
    return scr;
}

/* ================================================================== picker (both modes) */
static void picker_status(void)
{
    char buf[QT_SEL_MAX * (STOCKS_SYM_LEN + 2) + 40];
    if (A->p.mode == MODE_QUOTE) {
        snprintf(buf, sizeof(buf), "Tap a symbol to see its quote");
    } else if (!A->nsel) {
        snprintf(buf, sizeof(buf), "Tap symbols to pick them (up to %d)", QT_SEL_MAX);
    } else {
        char list[QT_SEL_MAX * (STOCKS_SYM_LEN + 2) + 8];
        sel_summary(list, sizeof(list));
        snprintf(buf, sizeof(buf), "%d picked: %s", A->nsel, list);
    }
    lv_label_set_text(A->p.status, buf);

    if (A->p.go) {
        bool on = A->nsel > 0;
        snprintf(buf, sizeof(buf), on ? "Watchlist (%d)" : "Watchlist", A->nsel);
        lv_label_set_text(A->p.go_lbl, buf);
        lv_obj_set_style_bg_color(A->p.go, on ? ui_pal.accent : ui_pal.card, 0);
        lv_obj_set_style_text_color(A->p.go_lbl, on ? lv_color_white() : ui_pal.text_dim, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(A->p.go, 0), on ? lv_color_white() : ui_pal.text_dim, 0);
    }
    if (A->p.clear) lv_obj_set_style_opa(A->p.clear, A->nsel ? LV_OPA_COVER : LV_OPA_40, 0);
}

static void style_sym_button(int i)
{
    lv_obj_t *b = A->p.btn[i];
    if (!b) return;
    bool on = A->p.mode == MODE_WATCH && sel_index(A->p.btn_sym[i]) >= 0;
    bool last = A->p.mode == MODE_QUOTE && !strcmp(A->p.btn_sym[i], A->quote_sym);
    lv_obj_set_style_bg_color(b, on ? lv_color_hex(UI_C_GREEN) : ui_pal.card, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 0), on ? lv_color_white() : ui_pal.text, 0);
    /* in quote mode, a ring marks the symbol you looked at last */
    lv_obj_set_style_border_width(b, last ? 2 : 0, 0);
    lv_obj_set_style_border_color(b, ui_pal.accent, 0);
}

static void sym_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= A->p.nbtn) return;
    const char *sym = A->p.btn_sym[i];

    if (A->p.mode == MODE_QUOTE) {
        strlcpy(A->quote_sym, sym, sizeof(A->quote_sym));
        ui_nav_push(quote_build, NULL, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
        return;
    }
    int k = sel_index(sym);
    if (k >= 0) {
        memmove(&A->sel[k], &A->sel[k + 1], (size_t)(A->nsel - k - 1) * sizeof(stocks_sym_t));
        A->nsel--;
    } else if (A->nsel >= QT_SEL_MAX) {
        char msg[48];
        snprintf(msg, sizeof(msg), "Up to %d symbols", QT_SEL_MAX);
        ui_toast(msg);
        return;
    } else {
        strlcpy(A->sel[A->nsel++], sym, sizeof(stocks_sym_t));
    }
    A->sel_dirty = true;
    style_sym_button(i);
    picker_status();
}

static lv_obj_t *sym_button(const char *text, int i)
{
    lv_obj_t *b = lv_obj_create(A->p.grid);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, SYM_W, SYM_H);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, ui_pal.card, 0);
    lv_obj_set_style_transform_scale(b, 235, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(b, SYM_W / 2, 0);
    lv_obj_set_style_transform_pivot_y(b, SYM_H / 2, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = ui_label(b, strlen(text) > 4 ? UI_FONT(16, 20) : UI_FONT(20, 24), ui_pal.text, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, sym_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    return b;
}

static void build_grid(void)
{
    lv_obj_clean(A->p.grid);
    memset(A->p.btn, 0, sizeof(A->p.btn));
    A->p.nbtn = 0;
    int tab = A->tab[A->p.mode];
    int ntabs = A->p.mode == MODE_WATCH ? GROUP_COUNT + 1 : GROUP_COUNT;
    for (int t = 0; t < ntabs; t++) {
        bool on = t == tab;
        lv_obj_set_style_bg_color(A->p.tabs[t], on ? ui_pal.accent : ui_pal.card, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(A->p.tabs[t], 0), on ? lv_color_white() : ui_pal.text, 0);
    }

    if (tab < GROUP_COUNT) {
        for (int k = 0; GROUPS[tab].syms[k] && A->p.nbtn < GRID_MAX; k++) {
            strlcpy(A->p.btn_sym[A->p.nbtn], GROUPS[tab].syms[k], sizeof(stocks_sym_t));
            A->p.btn[A->p.nbtn] = sym_button(GROUPS[tab].syms[k], A->p.nbtn);
            A->p.nbtn++;
        }
    } else {
        /* Picked: your selection (tapping one deselects it; it stays until you switch tabs) */
        for (int k = 0; k < A->nsel && A->p.nbtn < GRID_MAX; k++) {
            strlcpy(A->p.btn_sym[A->p.nbtn], A->sel[k], sizeof(stocks_sym_t));
            A->p.btn[A->p.nbtn] = sym_button(A->sel[k], A->p.nbtn);
            A->p.nbtn++;
        }
        if (A->p.nbtn == 0) {
            lv_obj_t *l = ui_label(A->p.grid, UI_FONT(16, 20), ui_pal.text_dim,
                                   "Symbols you pick on the other tabs show up here.");
            lv_obj_set_width(l, UI_SZ(300, 340));
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_pad_top(l, 60, 0);
        }
    }
    for (int i = 0; i < A->p.nbtn; i++) style_sym_button(i);
}

static void tab_cb(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    if (t == A->tab[A->p.mode]) return;
    A->tab[A->p.mode] = t;
    build_grid();
}

static void go_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    if (!A->nsel) { ui_toast("Pick some symbols first"); return; }
    save_selection();
    ui_nav_push(watchlist_build, NULL, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

static void do_clear(void *user)
{
    if (!A->p.scr) return;
    A->nsel = 0;
    A->sel_dirty = true;
    build_grid();
    picker_status();
}

static void clear_cb(lv_event_t *e)
{
    if (ui_nav_busy() || !A->nsel) return;
    ui_confirm("Clear the list?", "Deselect all the symbols you picked?", "Clear", true, do_clear, NULL);
}

static void picker_delete_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != A->p.scr) return;
    save_selection();                         /* leaving the screen (back, Home, or Watchlist) */
    memset(&A->p, 0, sizeof(A->p));
}

static lv_obj_t *picker_build(void *arg)
{
    memset(&A->p, 0, sizeof(A->p));
    A->p.mode = (int)(intptr_t)arg;
    bool watch = A->p.mode == MODE_WATCH;
    load_selection();

    lv_obj_t *scr = ui_screen();
    A->p.scr = scr;
    lv_obj_add_event_cb(scr, back_gesture_cb, LV_EVENT_GESTURE, NULL);
    header(scr, watch ? "Pick symbols" : "Get a quote", pop_cb, watch ? &A->p.clear : NULL,
           watch ? ICON_TRASH : NULL, clear_cb);

    /* tabs (the watchlist picker has a fourth: Picked) */
    int ntabs = watch ? GROUP_COUNT + 1 : GROUP_COUNT;
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(bar, LV_SIZE_CONTENT, TABS_H);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, TABS_Y);
    for (int t = 0; t < ntabs; t++) {
        lv_obj_t *b = lv_obj_create(bar);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, t == 2 ? UI_SZ(96, 108) : UI_SZ(76, 86), TABS_H);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(b, 4);
        lv_obj_t *l = ui_label(b, UI_FONT(14, 16), ui_pal.text, t < GROUP_COUNT ? GROUPS[t].name : "Picked");
        lv_obj_center(l);
        lv_obj_add_event_cb(b, tab_cb, LV_EVENT_PRESSED, (void *)(intptr_t)t);
        A->p.tabs[t] = b;
    }
    if (A->tab[A->p.mode] >= ntabs) A->tab[A->p.mode] = 0;

    /* 5 x 5 symbol grid */
    A->p.grid = lv_obj_create(scr);
    lv_obj_remove_style_all(A->p.grid);
    lv_obj_remove_flag(A->p.grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(A->p.grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(A->p.grid, GRID_COLS * SYM_W + (GRID_COLS - 1) * SYM_GAP, GRID_ROWS * SYM_H + (GRID_ROWS - 1) * SYM_GAP);
    lv_obj_align(A->p.grid, LV_ALIGN_TOP_MID, 0, GRID_TOP);
    lv_obj_set_flex_flow(A->p.grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(A->p.grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(A->p.grid, SYM_GAP, 0);
    lv_obj_set_style_pad_row(A->p.grid, SYM_GAP, 0);

    A->p.status = ui_label(scr, UI_FONT(14, 16), ui_pal.text_dim, "");
    lv_obj_set_width(A->p.status, watch ? UI_SZ(290, UI_W - 60) : UI_SZ(280, UI_W - 80));
    lv_obj_set_style_text_align(A->p.status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(A->p.status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(A->p.status, LV_ALIGN_TOP_MID, 0, STATUS_Y);

    if (watch) {
        /* [list icon] Watchlist (N) */
        A->p.go = lv_obj_create(scr);
        lv_obj_remove_style_all(A->p.go);
        lv_obj_set_size(A->p.go, GO_W, GO_H);
        lv_obj_set_style_radius(A->p.go, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(A->p.go, LV_OPA_COVER, 0);
        lv_obj_set_style_transform_scale(A->p.go, 240, LV_STATE_PRESSED);
        lv_obj_set_style_transform_pivot_x(A->p.go, GO_W / 2, 0);
        lv_obj_set_style_transform_pivot_y(A->p.go, GO_H / 2, 0);
        lv_obj_add_flag(A->p.go, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(A->p.go, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(A->p.go, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(A->p.go, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(A->p.go, 8, 0);
        ui_icon(A->p.go, UI_SZ(&ui_icons_16, &ui_icons_20), lv_color_white(), ICON_LIST);
        A->p.go_lbl = ui_label(A->p.go, UI_FONT(16, 20), lv_color_white(), "Watchlist");
        lv_obj_align(A->p.go, LV_ALIGN_TOP_MID, 0, GO_Y);
        lv_obj_add_event_cb(A->p.go, go_cb, LV_EVENT_CLICKED, NULL);
    } else {
        lv_obj_t *hint = ui_label(scr, UI_FONT(12, 14), ui_pal.text_dim, "Swipe right to go back");
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, STATUS_Y + UI_SZ(24, 28));
    }

    build_grid();
    picker_status();
    lv_obj_add_event_cb(scr, picker_delete_cb, LV_EVENT_DELETE, NULL);
    return scr;
}

/* ================================================================== watchlist */
static quote_row_t make_row(int i);

static void row_click_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= A->w.nrows || i >= S.wl_n) return;
    strlcpy(A->quote_sym, S.wl_syms[i], sizeof(A->quote_sym));
    ui_nav_push(quote_build, NULL, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

static quote_row_t make_row(int i)
{
    quote_row_t r;
    r.row = lv_obj_create(A->w.list);
    lv_obj_remove_style_all(r.row);
    lv_obj_set_size(r.row, UI_LIST_WIDTH, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(r.row, UI_SZ(60, 72), 0);
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
    /* SHORT_CLICKED: not sent when the touch scrolled the list */
    lv_obj_add_event_cb(r.row, row_click_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);

    lv_obj_t *left = lv_obj_create(r.row);
    lv_obj_remove_style_all(left);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 2, 0);
    r.sym = ui_label(left, UI_FONT(20, 24), ui_pal.text, "");
    r.name = ui_label(left, UI_FONT(14, 16), ui_pal.text_dim, "");
    lv_obj_set_size(r.name, UI_SZ(150, 190), lv_font_get_line_height(UI_FONT(14, 16)));   /* one line, then "..." */
    lv_label_set_long_mode(r.name, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *right = lv_obj_create(r.row);
    lv_obj_remove_style_all(right);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(right, 4, 0);
    r.price = ui_label(right, UI_FONT(20, 24), ui_pal.text, "");
    r.pill = lv_obj_create(right);
    lv_obj_remove_style_all(r.pill);
    lv_obj_remove_flag(r.pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r.pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(r.pill, 8, 0);
    lv_obj_set_style_bg_opa(r.pill, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(r.pill, 8, 0);
    lv_obj_set_style_pad_ver(r.pill, 2, 0);
    r.chg = ui_label(r.pill, UI_FONT(14, 16), lv_color_white(), "");
    return r;
}

static void fill_row(quote_row_t *r, const stocks_quote_t *q, bool loaded)
{
    lv_label_set_text(r->sym, q->sym);
    char name[3 * STOCKS_NAME_LEN];
    text_to_ascii(q->name, name, sizeof(name));          /* "Nestlé" -> "Nestle": the font is ASCII */
    lv_label_set_text(r->name, name[0] ? name : (loaded && !q->has_price ? "Not found" : ""));
    char buf[48];
    if (q->has_price) {
        fmt_price(q->price, buf, sizeof(buf));
        lv_label_set_text(r->price, buf);
        lv_obj_set_style_text_color(r->price, ui_pal.text, 0);
        lv_obj_set_style_text_font(r->price, UI_FONT(20, 24), 0);
    } else {
        lv_label_set_text(r->price, loaded ? "no quote" : "...");
        lv_obj_set_style_text_color(r->price, ui_pal.text_dim, 0);
        lv_obj_set_style_text_font(r->price, UI_FONT(16, 20), 0);
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
}

static void wl_message(const char *title, const char *text)
{
    if (!title) {
        if (A->w.msg) { lv_obj_delete(A->w.msg); A->w.msg = NULL; }
        return;
    }
    if (!A->w.msg) {
        A->w.msg = lv_obj_create(A->w.list);
        lv_obj_remove_style_all(A->w.msg);
        lv_obj_remove_flag(A->w.msg, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(A->w.msg, UI_SZ(300, UI_W - 60), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(A->w.msg, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(A->w.msg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(A->w.msg, 8, 0);
        lv_obj_set_style_pad_top(A->w.msg, 30, 0);
        ui_icon(A->w.msg, &ui_icons_56, ui_pal.text_dim, ICON_LIST);
        ui_label(A->w.msg, UI_FONT(20, 24), ui_pal.text, "");
        lv_obj_t *m = ui_label(A->w.msg, UI_FONT(14, 16), ui_pal.text_dim, "");
        lv_obj_set_width(m, LV_PCT(100));
        lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_label_set_text(lv_obj_get_child(A->w.msg, 1), title);
    lv_label_set_text(lv_obj_get_child(A->w.msg, 2), text);
}

static void wl_update_status(void)
{
    char buf[140], age[24] = "";
    lv_color_t c = ui_pal.text_dim;
    if (S.wl_us) fmt_age(esp_timer_get_time() - S.wl_us, age, sizeof(age));
    if (S.link == QT_LINK_WAIT_WIFI)       { snprintf(buf, sizeof(buf), "Waiting for Wi-Fi..."); c = ui_pal.warn; }
    else if (S.wl_state == QT_LOADING)     snprintf(buf, sizeof(buf), S.wl_loaded ? "Updating..." : "Loading...");
    else if (S.wl_state == QT_ERROR) {
        if (age[0]) snprintf(buf, sizeof(buf), "%s" BULLET "%s", S.wl_note, age);
        else        snprintf(buf, sizeof(buf), "%s", S.wl_note);
        c = ui_pal.err;
    } else if (S.wl_state == QT_OK && S.wl_at) {
        char clk[16];
        fmt_clock(S.wl_at, clk, sizeof(clk));
        snprintf(buf, sizeof(buf), "Updated %s", clk);
    } else if (S.wl_state == QT_OK) {
        snprintf(buf, sizeof(buf), "Updated %s", age);
    } else {
        buf[0] = '\0';
    }
    lv_label_set_text(A->w.status, buf);
    lv_obj_set_style_text_color(A->w.status, c, 0);
}

static void wl_update(void)
{
    quotes_get(&S);
    A->w.ver = quotes_version();
    wl_update_status();

    /* rows for the asked symbols; built once (the list only changes on the picker) */
    if (A->w.nrows != S.wl_n) {
        for (int i = 0; i < A->w.nrows; i++) lv_obj_delete(A->w.rows[i].row);
        A->w.nrows = 0;
        for (int i = 0; i < S.wl_n; i++) A->w.rows[A->w.nrows++] = make_row(i);
    }
    for (int i = 0; i < A->w.nrows; i++) fill_row(&A->w.rows[i], &S.wl_rows[i], S.wl_loaded);

    if (S.link == QT_LINK_NOT_SET)
        wl_message("API not set", "Go back and open Stock API to enter the base URL and API key.");
    else if (S.wl_state == QT_ERROR && !S.wl_loaded)
        wl_message("Can't load quotes", S.wl_note);
    else
        wl_message(NULL, NULL);
    if (A->w.msg) lv_obj_move_to_index(A->w.msg, 0);
    lv_obj_update_layout(A->w.list);
    lv_obj_send_event(A->w.list, LV_EVENT_SCROLL, NULL);     /* re-apply the edge fade */
}

static void wl_timer_cb(lv_timer_t *t)
{
    if (quotes_version() != A->w.ver) wl_update();
    else if (++A->w.ticks % 4 == 0) wl_update_status();    /* "12s ago" keeps counting */
    /* refresh every QT_REFRESH_MS while open; after a failure, retry every QT_RETRY_MS */
    if (A->w.ticks % 4 || S.wl_state == QT_LOADING) return;
    int64_t now = esp_timer_get_time();
    bool due = S.wl_state == QT_OK && S.wl_us && now - S.wl_us > (int64_t)QT_REFRESH_MS * 1000;
    bool retry = S.wl_state == QT_ERROR && S.link == QT_LINK_READY && S.last_us &&
                 now - S.last_us > (int64_t)QT_RETRY_MS * 1000;
    if (due || retry) quotes_request_watchlist(A->sel, A->nsel);
}

static void wl_delete_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != A->w.scr) return;
    if (A->w.timer) lv_timer_delete(A->w.timer);
    memset(&A->w, 0, sizeof(A->w));
}

static void wl_refresh_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    quotes_request_watchlist(A->sel, A->nsel);
    wl_update();
}

static lv_obj_t *watchlist_build(void *arg)
{
    if (A->w.timer) lv_timer_delete(A->w.timer);
    memset(&A->w, 0, sizeof(A->w));
    lv_obj_t *scr;
    A->w.list = ui_list_screen(&scr, "Watchlist", true);
    A->w.scr = scr;
    lv_obj_set_style_pad_top(A->w.list, UI_SZ(112, 104), 0);

    /* a taller top fade, so rows scrolling up don't show through the status line */
    lv_obj_t *fade = lv_obj_create(scr);
    lv_obj_remove_style_all(fade);
    lv_obj_remove_flag(fade, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(fade, UI_W, UI_SZ(126, 116));
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

    if (!UI_ROUND) {                          /* 2.06: rows would show through the button */
        lv_obj_t *bfade = lv_obj_create(scr);
        lv_obj_remove_style_all(bfade);
        lv_obj_remove_flag(bfade, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(bfade, UI_W, 110);
        lv_obj_align(bfade, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_opa(bfade, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bfade, ui_pal.bg, 0);
        lv_obj_set_style_bg_grad_color(bfade, ui_pal.bg, 0);
        lv_obj_set_style_bg_grad_dir(bfade, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_main_opa(bfade, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_grad_opa(bfade, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_main_stop(bfade, 0, 0);
        lv_obj_set_style_bg_grad_stop(bfade, 90, 0);
        lv_obj_move_to_index(bfade, 2);
    }

    A->w.status = ui_label(scr, UI_FONT(14, 16), ui_pal.text_dim, "");
    lv_obj_set_size(A->w.status, UI_SZ(270, UI_W - 60), lv_font_get_line_height(UI_FONT(14, 16)));   /* one line, then "..." */
    lv_obj_set_style_text_align(A->w.status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(A->w.status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(A->w.status, LV_ALIGN_TOP_MID, 0, UI_SZ(84, 76));

    lv_obj_t *rb = ui_round_button(scr, ICON_ROTATE, UI_SZ(&ui_icons_20, &ui_icons_28), UI_SZ(52, 60), ui_pal.card, ui_pal.text);
    lv_obj_align(rb, LV_ALIGN_BOTTOM_MID, 0, UI_SZ(-30, -24));
    lv_obj_add_event_cb(rb, wl_refresh_cb, LV_EVENT_CLICKED, NULL);

    quotes_request_watchlist(A->sel, A->nsel);   /* same list as last time: rows show the old numbers meanwhile */
    wl_update();
    A->w.timer = lv_timer_create(wl_timer_cb, 250, NULL);
    lv_obj_add_event_cb(scr, wl_delete_cb, LV_EVENT_DELETE, NULL);
    return scr;
}

/* ================================================================== Quote screen
 * A detailed quote from GET /quote: price, change, open, previous close and the day's range.
 * Shows the watchlist's numbers right away (when it came from there) and fills in the rest. */
#define BAR_W UI_SZ(240, 290)

static const stocks_quote_t *wl_row_for(const char *sym)
{
    if (!S.wl_loaded) return NULL;
    for (int i = 0; i < S.wl_n; i++)
        if (!strcmp(S.wl_rows[i].sym, sym) && S.wl_rows[i].has_price) return &S.wl_rows[i];
    return NULL;
}

static void set_value(lv_obj_t *l, bool has, double v, bool failed)
{
    char buf[32];
    if (has) fmt_price(v, buf, sizeof(buf));
    else snprintf(buf, sizeof(buf), failed ? "--" : "...");
    lv_label_set_text(l, buf);
}

static void quote_update(void)
{
    quotes_get(&S);
    A->q.ver = quotes_version();
    bool have = S.q_state == QT_OK && !strcmp(S.q_sym, A->quote_sym);
    bool mine = !strcmp(S.q_sym, A->quote_sym);
    const stocks_detail_t *d = have ? &S.q : NULL;
    const stocks_quote_t *q = d ? &d->q : wl_row_for(A->quote_sym);

    char buf[128];
    text_to_ascii(q && q->name[0] ? q->name : "", buf, sizeof(buf));
    lv_label_set_text(A->q.name, buf);
    if (q && q->has_price) {
        fmt_price(q->price, buf, sizeof(buf));
        lv_label_set_text(A->q.price, buf);
    } else {
        lv_label_set_text(A->q.price, mine && S.q_state == QT_ERROR ? "--" : "...");
    }
    if (q && q->has_price && (q->has_change || q->has_pct)) {
        double v = q->has_change ? q->change : q->pct;
        lv_color_t c = v > 0 ? ui_pal.ok : (v < 0 ? ui_pal.err : lv_color_hex(UI_C_GRAY));
        if (q->has_change && q->has_pct) snprintf(buf, sizeof(buf), "%+.2f  (%+.2f%%)", q->change, q->pct);
        else if (q->has_change)          snprintf(buf, sizeof(buf), "%+.2f", q->change);
        else                             snprintf(buf, sizeof(buf), "%+.2f%%", q->pct);
        lv_label_set_text(A->q.chg, buf);
        lv_obj_set_style_bg_color(A->q.pill, c, 0);
        lv_obj_remove_flag(A->q.pill, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(A->q.pill, LV_OBJ_FLAG_HIDDEN);
    }

    bool failed = mine && S.q_state == QT_ERROR;
    set_value(A->q.open_v, d && d->has_open, d ? d->open : 0, failed);
    set_value(A->q.prev_v, d && d->has_prev_close, d ? d->prev_close : 0, failed);

    /* day range: low ----o---- high */
    if (d && d->has_high && d->has_low && d->high > d->low) {
        lv_obj_remove_flag(A->q.range, LV_OBJ_FLAG_HIDDEN);
        fmt_price(d->low, buf, sizeof(buf));
        lv_label_set_text(A->q.lo, buf);
        fmt_price(d->high, buf, sizeof(buf));
        lv_label_set_text(A->q.hi, buf);
        double f = (d->q.price - d->low) / (d->high - d->low);
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        int x = (int)(f * BAR_W);
        lv_obj_set_width(A->q.fill, x < 4 ? 4 : x);
        lv_obj_set_x(A->q.dot, 20 + x - 8);            /* the bar starts 20 px into the range box */
    } else {
        lv_obj_add_flag(A->q.range, LV_OBJ_FLAG_HIDDEN);
    }

    lv_color_t c = ui_pal.text_dim;
    if (S.link == QT_LINK_NOT_SET) {
        snprintf(buf, sizeof(buf), "Set the API URL (Stock API screen)");
        c = ui_pal.err;
    } else if (mine && S.q_state == QT_ERROR) {
        snprintf(buf, sizeof(buf), "%s", S.q_note);
        c = ui_pal.err;
    } else if (mine && S.q_state == QT_LOADING) {
        snprintf(buf, sizeof(buf), "%s", S.link == QT_LINK_WAIT_WIFI ? "Waiting for Wi-Fi..." :
                                         have ? "Updating..." : "Loading quote...");
    } else if (have && S.q_at) {
        char clk[16];
        fmt_clock(S.q_at, clk, sizeof(clk));
        snprintf(buf, sizeof(buf), "Quote at %s", clk);
    } else if (have) {
        char age[24];
        fmt_age(esp_timer_get_time() - S.q_us, age, sizeof(age));
        snprintf(buf, sizeof(buf), "Quote from %s", age);
    } else {
        buf[0] = '\0';
    }
    lv_label_set_text(A->q.status, buf);
    lv_obj_set_style_text_color(A->q.status, c, 0);
}

static void quote_timer_cb(lv_timer_t *t)
{
    if (quotes_version() != A->q.ver) quote_update();
    if (++A->q.ticks % 4) return;
    if (S.q_state == QT_OK && S.q_us && esp_timer_get_time() - S.q_us > (int64_t)QT_REFRESH_MS * 1000) {
        quotes_request_quote(A->quote_sym);   /* while it's open, refresh it every minute */
    }
}

static void quote_delete_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != A->q.scr) return;
    if (A->q.timer) lv_timer_delete(A->q.timer);
    memset(&A->q, 0, sizeof(A->q));
}

static void quote_refresh_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    quotes_request_quote(A->quote_sym);
    quote_update();
}

static lv_obj_t *stat_card(lv_obj_t *parent, const char *title, lv_obj_t **value)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(c, UI_SZ(146, 172), UI_SZ(60, 72));
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_bg_color(c, ui_pal.card, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 2, 0);
    ui_label(c, UI_FONT(14, 16), ui_pal.text_dim, title);
    *value = ui_label(c, UI_FONT(20, 24), ui_pal.text, "...");
    return c;
}

static lv_obj_t *quote_build(void *arg)
{
    if (A->q.timer) lv_timer_delete(A->q.timer);
    memset(&A->q, 0, sizeof(A->q));
    lv_obj_t *scr = ui_screen();
    A->q.scr = scr;
    lv_obj_add_event_cb(scr, back_gesture_cb, LV_EVENT_GESTURE, NULL);

    header(scr, A->quote_sym, pop_cb, NULL, NULL, NULL);

    A->q.name = ui_label(scr, UI_FONT(16, 20), ui_pal.text_dim, "");
    lv_obj_set_size(A->q.name, UI_SZ(290, UI_W - 60), lv_font_get_line_height(UI_FONT(16, 20)));
    lv_label_set_long_mode(A->q.name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(A->q.name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(A->q.name, LV_ALIGN_TOP_MID, 0, UI_SZ(90, 80));   /* under the bigger header */

    A->q.price = ui_label(scr, &lv_font_montserrat_44, ui_pal.text, "...");
    lv_obj_align(A->q.price, LV_ALIGN_TOP_MID, 0, UI_SZ(110, 110));

    A->q.pill = lv_obj_create(scr);
    lv_obj_remove_style_all(A->q.pill);
    lv_obj_remove_flag(A->q.pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(A->q.pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(A->q.pill, 10, 0);
    lv_obj_set_style_bg_opa(A->q.pill, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(A->q.pill, 12, 0);
    lv_obj_set_style_pad_ver(A->q.pill, 3, 0);
    A->q.chg = ui_label(A->q.pill, UI_FONT(16, 20), lv_color_white(), "");
    lv_obj_align(A->q.pill, LV_ALIGN_TOP_MID, 0, UI_SZ(164, 170));

    /* open | previous close */
    lv_obj_t *cards = lv_obj_create(scr);
    lv_obj_remove_style_all(cards);
    lv_obj_remove_flag(cards, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(cards, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cards, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cards, 10, 0);
    lv_obj_align(cards, LV_ALIGN_TOP_MID, 0, UI_SZ(202, 216));
    stat_card(cards, "Open", &A->q.open_v);
    stat_card(cards, "Prev close", &A->q.prev_v);

    /* day range */
    A->q.range = lv_obj_create(scr);
    lv_obj_remove_style_all(A->q.range);
    lv_obj_remove_flag(A->q.range, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(A->q.range, BAR_W + 40, UI_SZ(70, 76));
    lv_obj_align(A->q.range, LV_ALIGN_TOP_MID, 0, UI_SZ(274, 304));
    lv_obj_t *rt = ui_label(A->q.range, UI_FONT(14, 16), ui_pal.text_dim, "Day range");
    lv_obj_align(rt, LV_ALIGN_TOP_MID, 0, 0);
    A->q.bar = lv_obj_create(A->q.range);
    lv_obj_remove_style_all(A->q.bar);
    lv_obj_remove_flag(A->q.bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(A->q.bar, BAR_W, 8);
    lv_obj_set_style_radius(A->q.bar, 4, 0);
    lv_obj_set_style_bg_color(A->q.bar, ui_pal.card_press, 0);
    lv_obj_set_style_bg_opa(A->q.bar, LV_OPA_COVER, 0);
    lv_obj_align(A->q.bar, LV_ALIGN_TOP_MID, 0, UI_SZ(28, 30));
    A->q.fill = lv_obj_create(A->q.bar);
    lv_obj_remove_style_all(A->q.fill);
    lv_obj_remove_flag(A->q.fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(A->q.fill, 4, 8);
    lv_obj_set_style_radius(A->q.fill, 4, 0);
    lv_obj_set_style_bg_color(A->q.fill, ui_pal.accent, 0);
    lv_obj_set_style_bg_opa(A->q.fill, LV_OPA_COVER, 0);
    A->q.dot = lv_obj_create(A->q.range);             /* a sibling of the bar, so it isn't clipped */
    lv_obj_remove_style_all(A->q.dot);
    lv_obj_remove_flag(A->q.dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(A->q.dot, 16, 16);
    lv_obj_set_style_radius(A->q.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(A->q.dot, ui_pal.text, 0);
    lv_obj_set_style_bg_opa(A->q.dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(A->q.dot, 20 - 8, UI_SZ(28, 30) - 4);
    A->q.lo = ui_label(A->q.range, UI_FONT(14, 16), ui_pal.text_dim, "");
    lv_obj_align(A->q.lo, LV_ALIGN_TOP_LEFT, 20, UI_SZ(44, 48));
    A->q.hi = ui_label(A->q.range, UI_FONT(14, 16), ui_pal.text_dim, "");
    lv_obj_align(A->q.hi, LV_ALIGN_TOP_RIGHT, -20, UI_SZ(44, 48));
    lv_obj_add_flag(A->q.range, LV_OBJ_FLAG_HIDDEN);

    A->q.status = ui_label(scr, UI_FONT(14, 16), ui_pal.text_dim, "");
    lv_obj_set_width(A->q.status, UI_SZ(300, UI_W - 60));
    lv_obj_set_style_text_align(A->q.status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(A->q.status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(A->q.status, LV_ALIGN_TOP_MID, 0, UI_SZ(352, 390));

    lv_obj_t *rb = ui_round_button(scr, ICON_ROTATE, UI_SZ(&ui_icons_20, &ui_icons_28), UI_SZ(52, 60), ui_pal.card, ui_pal.text);
    lv_obj_align(rb, LV_ALIGN_BOTTOM_MID, 0, UI_SZ(-30, -24));
    lv_obj_add_event_cb(rb, quote_refresh_cb, LV_EVENT_CLICKED, NULL);

    quotes_request_quote(A->quote_sym);
    quote_update();
    A->q.timer = lv_timer_create(quote_timer_cb, 250, NULL);
    lv_obj_add_event_cb(scr, quote_delete_cb, LV_EVENT_DELETE, NULL);
    return scr;
}

/* ================================================================== Stock API screen */
static void sv_refresh(void)
{
    SETTINGS_COPY(cfg);
    lv_label_set_text(A->sv.url.value, cfg->qt_url[0] ? cfg->qt_url : "Not set");
    char masked[48];
    ui_mask_secret(cfg->qt_api_key, masked, sizeof(masked), true);
    lv_label_set_text(A->sv.key.value, cfg->qt_api_key[0] ? masked : "Not set");
    char q[QT_ENDPOINT_MAX], w[QT_ENDPOINT_MAX], buf[2 * QT_ENDPOINT_MAX + 20];
    quotes_endpoint(cfg->qt_url, QT_PATH_QUOTE, q, sizeof(q));
    quotes_endpoint(cfg->qt_url, QT_PATH_WATCHLIST, w, sizeof(w));
    snprintf(buf, sizeof(buf), "GET %s\nGET %s", q, w);
    lv_label_set_text(A->sv.ep.value, buf);

    quotes_get(&S);
    uint32_t c = link_text(buf, sizeof(buf));
    lv_label_set_text(A->sv.status.value, buf);
    lv_obj_set_style_bg_color(lv_obj_get_child(A->sv.status.row, 0), lv_color_hex(c), 0);
}

static void sv_timer_cb(lv_timer_t *t) { sv_refresh(); }

static void sv_delete_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != A->sv.scr) return;
    if (A->sv.timer) lv_timer_delete(A->sv.timer);
    memset(&A->sv, 0, sizeof(A->sv));
}

static bool sv_url_saved(const char *text, void *user)
{
    if (strncmp(text, "https://", 8) != 0 && strncmp(text, "http://", 7) != 0) {
        ui_toast("Start the URL with https://");
        return true;                       /* stay in the editor */
    }
    SETTINGS_COPY(cfg);
    strlcpy(cfg->qt_url, text, sizeof(cfg->qt_url));
    settings_set(cfg);
    quotes_config_changed();
    ui_toast("Saved");
    return false;
}

static bool sv_key_saved(const char *text, void *user)
{
    SETTINGS_COPY(cfg);
    strlcpy(cfg->qt_api_key, text, sizeof(cfg->qt_api_key));
    settings_set(cfg);
    quotes_config_changed();
    ui_toast("Saved");
    return false;
}

static void sv_url_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    SETTINGS_COPY(cfg);
    ui_editor_cfg_t ed = {
        .title = "Stock API URL", .icon = ICON_LINK, .initial = cfg->qt_url,
        .placeholder = "https://host:4443/stock2api", .max_len = SET_URL_MAX,
        .on_save = sv_url_saved,
    };
    ui_editor_open(&ed, false);
}

static void sv_key_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    SETTINGS_COPY(cfg);
    ui_editor_cfg_t ed = {
        .title = "API key", .icon = ICON_KEY, .initial = cfg->qt_api_key,
        .placeholder = "x-api-key", .max_len = SET_KEY_MAX, .password = true, .on_save = sv_key_saved,
    };
    ui_editor_open(&ed, false);
}

static void sv_test_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    load_selection();
    const char *sym = A->nsel ? A->sel[0] : "AAPL";
    quotes_request_quote(sym);
    char msg[40];
    snprintf(msg, sizeof(msg), "Asking for a %s quote", sym);
    ui_toast(msg);
}

static void sv_reset_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    SETTINGS_COPY(cfg);
    strlcpy(cfg->qt_url, settings_default_qt_url(), sizeof(cfg->qt_url));
    settings_set(cfg);
    quotes_config_changed();
    sv_refresh();
    ui_toast("URL reset to secrets.h");
}

static lv_obj_t *server_build(void *arg)
{
    if (A->sv.timer) lv_timer_delete(A->sv.timer);
    memset(&A->sv, 0, sizeof(A->sv));
    lv_obj_t *scr;
    lv_obj_t *list = ui_list_screen(&scr, "Stock API", true);
    A->sv.scr = scr;

    ui_section(list, "SERVER");
    A->sv.url = ui_row(list, ICON_LINK, UI_C_BLUE, "Base URL", "", sv_url_cb, NULL);
    lv_obj_set_height(A->sv.url.value, 2 * lv_font_get_line_height(UI_F_VALUE));
    A->sv.key = ui_row(list, ICON_KEY, UI_C_ORANGE, "API key", "", sv_key_cb, NULL);
    A->sv.status = ui_row(list, ICON_PLUG, UI_C_ORANGE, "Status", "", NULL, NULL);
    lv_label_set_long_mode(A->sv.status.value, LV_LABEL_LONG_MODE_WRAP);
    A->sv.ep = ui_row(list, ICON_SERVER, UI_C_GRAY, "Endpoints", "", NULL, NULL);
    lv_label_set_long_mode(A->sv.ep.value, LV_LABEL_LONG_MODE_WRAP);

    ui_section(list, "ACTIONS");
    ui_row(list, ICON_CHART, UI_C_TEAL, "Test", "Ask for one quote", sv_test_cb, NULL);
    ui_row(list, ICON_RESET, UI_C_GRAY, "Reset URL", "Back to secrets.h", sv_reset_cb, NULL);

    sv_refresh();
    A->sv.timer = lv_timer_create(sv_timer_cb, 300, NULL);
    lv_obj_add_event_cb(scr, sv_delete_cb, LV_EVENT_DELETE, NULL);
    return scr;
}

/* ================================================================== app descriptor */
static lv_obj_t *quotes_build(void *arg)
{
    if (!A) {                                /* built before on_enter (shouldn't happen): be safe */
        A = calloc(1, sizeof(*A));
        if (!A) return ui_screen();
    }
    return menu_build(arg);
}

static void quotes_enter(void)
{
    if (!A) A = calloc(1, sizeof(*A));       /* about 6 KB: ESP-IDF puts it in PSRAM */
    if (A) A->sel_loaded = false;            /* re-read: Reset settings or the phone may have changed it */
    quotes_set_active(true);
}

static void quotes_leave(void) { quotes_set_active(false); }

const ui_app_t APP_QUOTES = {
    .name = "Quotes",
    .icon = ICON_LIST,
    .color1 = C_QUOTES_1,
    .color2 = C_QUOTES_2,
    .build = quotes_build,
    .on_enter = quotes_enter,
    .on_leave = quotes_leave,
};
