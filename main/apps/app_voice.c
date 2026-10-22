/* Voice chat app: the push-to-talk screen and its server settings screen.
 *
 *   "Jarvis" ...       -> talk hands-free, if the wake word is on (Settings > Voice chat)
 *   quick tap          -> talk hands-free, if the wake word is on
 *   touch and hold     -> talk (release to send)
 *   swipe up / down    -> voice settings (server URL, API key)
 *   swipe left / right -> back to Home
 *
 * Recording starts the moment the finger lands, so the first word isn't lost; if the finger
 * then moves UI_SWIPE_MIN_PX within UI_SWIPE_MAX_MS, the touch was a swipe and the recording is
 * discarded. The audio/WebSocket work happens in voice_core.c; this file only draws, polling the
 * engine's state from an LVGL timer. */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_timer.h"

#include "apps.h"
#include "ui_widgets.h"
#include "voice.h"
#include "settings_store.h"
#include "app_config.h"

#define REPLY_BIG_FONT_CHARS 90             // longer replies switch to the 16 px font
#define ORB_SIZE             132

/* ---- "thinking" animation: three dots bouncing in a wave -------------------------------- */
#define DOT_COUNT     3
#define DOT_SIZE      18
#define DOT_GAP       16
#define DOT_BOUNCE    22     // px
#define DOT_PERIOD_MS 1000   // one full wave
#define DOT_STAGGER   140    // ms between dots

typedef struct {
    lv_obj_t *scr, *ring, *status, *orb, *halo, *col, *heard, *reply, *note, *dots, *hint;
    lv_obj_t *dot[DOT_COUNT];
    lv_timer_t *timer;
    voice_state_t last_state;
    uint32_t last_ver;
    bool last_wake;             /* the wake word engine was listening */
    int last_mode;              /* what the center shows: 0 orb, 1 text */
    bool dots_on, halo_on;
} voice_ui_t;

static voice_ui_t s_ui;

/* v runs 0..1000 over one period; the dot hops during the first half, rests the second half */
static void dot_anim_cb(void *var, int32_t v)
{
    float phase = v / 1000.0f;
    float hop = phase < 0.5f ? sinf(phase * 2.0f * (float)M_PI) : 0.0f;
    int32_t h = (phase > 0.02f && hop < 0.12f) ? DOT_SIZE - 4 : DOT_SIZE;
    lv_obj_set_height((lv_obj_t *)var, h);
    lv_obj_set_y((lv_obj_t *)var, DOT_BOUNCE - (int32_t)(hop * DOT_BOUNCE) + (DOT_SIZE - h));
}

static void dots_build(lv_obj_t *scr)
{
    static const uint32_t colors[DOT_COUNT] = { 0x2F8CFF, 0x8E5CFF, 0x14B8C8 };
    const int32_t width = DOT_COUNT * DOT_SIZE + (DOT_COUNT - 1) * DOT_GAP + 4;

    s_ui.dots = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ui.dots);
    lv_obj_set_size(s_ui.dots, width, DOT_SIZE + DOT_BOUNCE + 4);
    lv_obj_align(s_ui.dots, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(s_ui.dots, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_ui.dots, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.dots, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < DOT_COUNT; i++) {
        lv_obj_t *d = lv_obj_create(s_ui.dots);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, DOT_SIZE, DOT_SIZE);
        lv_obj_set_pos(d, i * (DOT_SIZE + DOT_GAP), DOT_BOUNCE);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
        s_ui.dot[i] = d;
    }
}

static void dots_set(bool on, bool below_text)
{
    lv_obj_align(s_ui.dots, below_text ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_CENTER, 0, below_text ? -96 : 0);
    if (on == s_ui.dots_on) return;
    s_ui.dots_on = on;
    for (int i = 0; i < DOT_COUNT; i++) {
        lv_anim_delete(s_ui.dot[i], dot_anim_cb);
        lv_obj_set_pos(s_ui.dot[i], i * (DOT_SIZE + DOT_GAP), DOT_BOUNCE);
        lv_obj_set_size(s_ui.dot[i], DOT_SIZE, DOT_SIZE);
        if (!on) continue;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_ui.dot[i]);
        lv_anim_set_exec_cb(&a, dot_anim_cb);
        lv_anim_set_values(&a, 0, 1000);
        lv_anim_set_duration(&a, DOT_PERIOD_MS);
        lv_anim_set_delay(&a, i * DOT_STAGGER);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
    if (on) lv_obj_remove_flag(s_ui.dots, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_ui.dots, LV_OBJ_FLAG_HIDDEN);
}

/* ---- listening halo: a ring that grows and fades behind the orb ------------------------- */
static void halo_anim_cb(void *var, int32_t v)
{
    lv_obj_t *h = var;
    int32_t size = ORB_SIZE + v * 70 / 1000;
    lv_obj_set_size(h, size, size);
    lv_obj_set_style_opa(h, (lv_opa_t)(200 - v * 200 / 1000), 0);
}

static void halo_set(bool on)
{
    if (on == s_ui.halo_on) return;
    s_ui.halo_on = on;
    lv_anim_delete(s_ui.halo, halo_anim_cb);
    if (!on) {
        lv_obj_add_flag(s_ui.halo, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_ui.halo, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_ui.halo);
    lv_anim_set_exec_cb(&a, halo_anim_cb);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, 1100);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

/* ---- state -> screen -------------------------------------------------------------------- */
static uint32_t state_color(voice_state_t st)
{
    switch (st) {
    case VOICE_WAIT_WIFI:
    case VOICE_CONNECTING: return 0xF5A623;
    case VOICE_READY:      return 0x2ECC71;
    case VOICE_LISTENING:  return 0xE74C3C;
    case VOICE_THINKING:   return 0x3498DB;
    case VOICE_SPEAKING:   return 0x9B59B6;
    case VOICE_ERROR:      return 0xE74C3C;
    default:               return 0x555555;
    }
}

static void orb_colors(voice_state_t st)
{
    uint32_t c1 = UI_C_VOICE_1, c2 = UI_C_VOICE_2;
    if (st == VOICE_LISTENING) { c1 = 0xFF5A4F; c2 = 0xE0306A; }
    else if (st != VOICE_READY) { c1 = 0x6B7079; c2 = 0x3E434C; }
    lv_obj_set_style_bg_color(s_ui.orb, lv_color_hex(c1), 0);
    lv_obj_set_style_bg_grad_color(s_ui.orb, lv_color_hex(c2), 0);
}

static void ui_update(bool force)
{
    voice_state_t st = voice_state();
    uint32_t ver = voice_text_version();
    bool wake = voice_wake_listening();
    if (!force && st == s_ui.last_state && ver == s_ui.last_ver && wake == s_ui.last_wake) return;
    s_ui.last_state = st;
    s_ui.last_ver = ver;
    s_ui.last_wake = wake;

    static char heard[220], reply[340], note[100];
    voice_text_get(heard, sizeof(heard), reply, sizeof(reply), note, sizeof(note));
    bool has_text = heard[0] || reply[0];

    lv_label_set_text(s_ui.status, voice_state_text(st));
    lv_obj_set_style_border_color(s_ui.ring, lv_color_hex(state_color(st)), 0);

    /* center: orb (idle / listening / connecting) or the conversation text */
    bool show_text = has_text;
    bool thinking = (st == VOICE_THINKING && !reply[0]);
    if (show_text) {
        lv_obj_add_flag(s_ui.orb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.col, LV_OBJ_FLAG_HIDDEN);
    } else if (thinking) {
        lv_obj_add_flag(s_ui.orb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.col, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_ui.orb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.col, LV_OBJ_FLAG_HIDDEN);
        orb_colors(st);
    }
    halo_set(st == VOICE_LISTENING && !show_text);
    dots_set(thinking, show_text);

    lv_label_set_text(s_ui.heard, heard);
    if (reply[0]) {
        lv_obj_set_style_text_color(s_ui.reply, ui_pal.text, 0);
        lv_obj_set_style_text_font(s_ui.reply, strlen(reply) > REPLY_BIG_FONT_CHARS ?
                                   &lv_font_montserrat_16 : &lv_font_montserrat_20, 0);
        lv_label_set_text(s_ui.reply, reply);
    } else {
        /* a note with part of a conversation on screen goes where the reply would be */
        lv_obj_set_style_text_color(s_ui.reply, ui_pal.warn, 0);
        lv_obj_set_style_text_font(s_ui.reply, &lv_font_montserrat_16, 0);
        lv_label_set_text(s_ui.reply, has_text ? note : "");
    }
    /* otherwise the note sits under the orb */
    lv_obj_set_style_text_color(s_ui.note, ui_pal.warn, 0);
    if (st == VOICE_DISCONNECTED && !has_text) {
        char buf[140];
        snprintf(buf, sizeof(buf), "%s%sTap to connect", note, note[0] ? "\n" : "");
        lv_label_set_text(s_ui.note, buf);
    } else if (st == VOICE_READY && wake && !has_text && !note[0]) {
        /* the status line says 'Say "Jarvis"'; touch still works */
        lv_obj_set_style_text_color(s_ui.note, ui_pal.text_dim, 0);
        lv_label_set_text(s_ui.note, "or tap to talk");
    } else {
        lv_label_set_text(s_ui.note, (!has_text && st != VOICE_LISTENING) ? note : "");
    }
    if (heard[0]) lv_obj_remove_flag(s_ui.heard, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_ui.heard, LV_OBJ_FLAG_HIDDEN);

    bool hint = !has_text && st != VOICE_LISTENING && st != VOICE_THINKING && st != VOICE_SPEAKING;
    if (hint) lv_obj_remove_flag(s_ui.hint, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_ui.hint, LV_OBJ_FLAG_HIDDEN);
}

static void timer_cb(lv_timer_t *t)
{
    ui_update(false);
}

static void delete_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_ui.scr) return;
    voice_set_foreground(false);             /* stop listening for the wake word */
    if (s_ui.timer) lv_timer_delete(s_ui.timer);
    for (int i = 0; i < DOT_COUNT; i++) lv_anim_delete(s_ui.dot[i], dot_anim_cb);
    lv_anim_delete(s_ui.halo, halo_anim_cb);
    memset(&s_ui, 0, sizeof(s_ui));
}

/* ---- touch: hold to talk, swipe to navigate ---------------------------------------------- */
static lv_obj_t *voice_settings_build(void *arg);

static lv_point_t s_touch_start;
static int64_t    s_touch_start_us;
static bool       s_swiped;

static void touch_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();

    if (code == LV_EVENT_PRESSED) {
        if (indev) lv_indev_get_point(indev, &s_touch_start);
        s_touch_start_us = esp_timer_get_time();
        s_swiped = ui_nav_busy();            /* ignore touches during a transition */
        if (!s_swiped) voice_ptt_press();
    } else if (code == LV_EVENT_PRESSING) {
        if (s_swiped || !indev) return;
        if (esp_timer_get_time() - s_touch_start_us > (int64_t)UI_SWIPE_MAX_MS * 1000) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int dx = p.x - s_touch_start.x;
        int dy = p.y - s_touch_start.y;
        bool vertical = LV_ABS(dy) >= UI_SWIPE_MIN_PX && LV_ABS(dy) >= 2 * LV_ABS(dx);
        bool horizontal = LV_ABS(dx) >= UI_SWIPE_MIN_PX && LV_ABS(dx) >= 2 * LV_ABS(dy);
        if (!vertical && !horizontal) return;

        s_swiped = true;
        voice_ptt_cancel();                  /* the touch started a recording: throw it away */
        if (vertical) {
            /* screen y grows downward: negative = swipe up, settings slide in from below */
            ui_nav_push(voice_settings_build, NULL,
                        dy < 0 ? LV_SCREEN_LOAD_ANIM_MOVE_TOP : LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM);
        } else {
            ui_nav_home(dx < 0 ? LV_SCREEN_LOAD_ANIM_MOVE_LEFT : LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s_swiped) voice_ptt_release();
    }
}

static lv_obj_t *voice_build(void *arg)
{
    if (s_ui.timer) lv_timer_delete(s_ui.timer);   /* rebuilt in place (theme change): drop the old timer */
    memset(&s_ui, 0, sizeof(s_ui));
    lv_obj_t *scr = ui_screen();
    s_ui.scr = scr;
    s_ui.last_state = (voice_state_t)-1;

    /* colored ring around the round display shows the state */
    s_ui.ring = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ui.ring);
    lv_obj_set_size(s_ui.ring, UI_SCREEN_SIZE - 6, UI_SCREEN_SIZE - 6);
    lv_obj_center(s_ui.ring);
    lv_obj_set_style_radius(s_ui.ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_ui.ring, 8, 0);
    lv_obj_set_style_border_color(s_ui.ring, lv_color_hex(0x555555), 0);
    lv_obj_remove_flag(s_ui.ring, LV_OBJ_FLAG_CLICKABLE);

    s_ui.status = ui_label(scr, &lv_font_montserrat_20, ui_pal.text, "");
    lv_obj_align(s_ui.status, LV_ALIGN_TOP_MID, 0, 64);

    /* listening halo + mic orb */
    s_ui.halo = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ui.halo);
    lv_obj_set_size(s_ui.halo, ORB_SIZE, ORB_SIZE);
    lv_obj_align(s_ui.halo, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_ui.halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_ui.halo, 6, 0);
    lv_obj_set_style_border_color(s_ui.halo, lv_color_hex(0xFF5A4F), 0);
    lv_obj_remove_flag(s_ui.halo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_ui.halo, LV_OBJ_FLAG_HIDDEN);

    s_ui.orb = ui_orb(scr, ORB_SIZE, UI_C_VOICE_1, UI_C_VOICE_2, ICON_MIC, &ui_icons_56);
    lv_obj_align(s_ui.orb, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(s_ui.orb, LV_OBJ_FLAG_CLICKABLE);

    /* Transcript and reply share one column, so a long reply never overlaps the transcript.
     * Content is centered vertically; anything that doesn't fit is clipped inside the circle. */
    s_ui.col = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ui.col);
    lv_obj_set_size(s_ui.col, 330, 240);
    lv_obj_align(s_ui.col, LV_ALIGN_CENTER, 0, 12);
    lv_obj_set_flex_flow(s_ui.col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_ui.col, 14, 0);
    lv_obj_remove_flag(s_ui.col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_ui.col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.col, LV_OBJ_FLAG_HIDDEN);

    s_ui.heard = ui_label(s_ui.col, &lv_font_montserrat_16, ui_pal.text_dim, "");
    lv_obj_set_width(s_ui.heard, LV_PCT(100));
    lv_label_set_long_mode(s_ui.heard, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_ui.heard, LV_TEXT_ALIGN_CENTER, 0);

    s_ui.reply = ui_label(s_ui.col, &lv_font_montserrat_20, ui_pal.text, "");
    lv_obj_set_width(s_ui.reply, LV_PCT(100));
    lv_label_set_long_mode(s_ui.reply, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_ui.reply, LV_TEXT_ALIGN_CENTER, 0);

    dots_build(scr);

    s_ui.note = ui_label(scr, &lv_font_montserrat_14, ui_pal.warn, "");
    lv_obj_set_width(s_ui.note, 300);
    lv_label_set_long_mode(s_ui.note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_ui.note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_ui.note, LV_ALIGN_CENTER, 0, ORB_SIZE / 2 + 34);

    /* gesture hint */
    s_ui.hint = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ui.hint);
    lv_obj_remove_flag(s_ui.hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_ui.hint, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_ui.hint, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ui.hint, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_ui.hint, 6, 0);
    ui_icon(s_ui.hint, &ui_icons_16, ui_pal.text_dim, ICON_UP);
    ui_label(s_ui.hint, &lv_font_montserrat_14, ui_pal.text_dim, "Server");
    lv_obj_t *gap = ui_label(s_ui.hint, &lv_font_montserrat_14, ui_pal.text_dim, "");
    lv_obj_set_width(gap, 14);
    ui_icon(s_ui.hint, &ui_icons_16, ui_pal.text_dim, ICON_LEFT);
    ui_label(s_ui.hint, &lv_font_montserrat_14, ui_pal.text_dim, "Home");
    lv_obj_align(s_ui.hint, LV_ALIGN_BOTTOM_MID, 0, -64);

    /* invisible full-screen touch area on top of everything */
    lv_obj_t *touch = lv_obj_create(scr);
    lv_obj_remove_style_all(touch);
    lv_obj_set_size(touch, LV_PCT(100), LV_PCT(100));
    lv_obj_center(touch);
    lv_obj_add_flag(touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(touch, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(touch, touch_event_cb, LV_EVENT_ALL, NULL);

    ui_update(true);
    s_ui.timer = lv_timer_create(timer_cb, 40, NULL);
    lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);
    voice_set_foreground(true);              /* listen for the wake word while this screen shows */
    return scr;
}

/* ===================================================== voice settings (swipe up/down) */
static lv_obj_t  *s_vs_scr;
static ui_row_t   s_vs_url, s_vs_key;
static ui_toggle_row_t s_vs_conn;
static lv_timer_t *s_vs_timer;

static void vs_refresh(void)
{
    app_settings_t cfg;
    settings_get(&cfg);
    lv_label_set_text(s_vs_url.value, cfg.ws_url[0] ? cfg.ws_url : "Not set");
    char masked[48];
    ui_mask_secret(cfg.ws_api_key, masked, sizeof(masked), true);
    lv_label_set_text(s_vs_key.value, masked);

    voice_state_t st = voice_state();
    char note[100];
    voice_text_get(NULL, 0, NULL, 0, note, sizeof(note));
    bool want = voice_wants_connection();
    const char *txt;
    uint32_t c;
    if (!want)                  { txt = note[0] ? note : "Off"; c = UI_C_GRAY; }
    else if (voice_connected()) { txt = "Connected"; c = UI_C_GREEN; }
    else if (st == VOICE_ERROR) { txt = note[0] ? note : voice_state_text(st); c = UI_C_RED; }
    else                        { txt = note[0] ? note : voice_state_text(st); c = UI_C_ORANGE; }
    lv_label_set_text(s_vs_conn.value, txt);
    lv_obj_set_style_bg_color(lv_obj_get_child(s_vs_conn.row, 0), lv_color_hex(c), 0);
    /* follow the engine (an idle timeout switches it off) */
    if (want != lv_obj_has_state(s_vs_conn.sw, LV_STATE_CHECKED)) {
        if (want) lv_obj_add_state(s_vs_conn.sw, LV_STATE_CHECKED);
        else lv_obj_remove_state(s_vs_conn.sw, LV_STATE_CHECKED);
    }
}

static void vs_timer_cb(lv_timer_t *t) { vs_refresh(); }

static void vs_delete_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_vs_scr) return;
    if (s_vs_timer) lv_timer_delete(s_vs_timer);
    s_vs_timer = NULL;
    s_vs_scr = NULL;
}

static bool url_saved(const char *text, void *user)
{
    if (strncmp(text, "wss://", 6) != 0 && strncmp(text, "ws://", 5) != 0) {
        ui_toast("Start the URL with wss://");
        return true;                       /* stay in the editor */
    }
    app_settings_t cfg;
    settings_get(&cfg);
    strlcpy(cfg.ws_url, text, sizeof(cfg.ws_url));
    settings_set(&cfg);
    voice_reconnect();
    ui_toast("Saved - reconnecting");
    return false;
}

static bool key_saved(const char *text, void *user)
{
    app_settings_t cfg;
    settings_get(&cfg);
    strlcpy(cfg.ws_api_key, text, sizeof(cfg.ws_api_key));
    settings_set(&cfg);
    voice_reconnect();
    ui_toast("Saved - reconnecting");
    return false;
}

static void url_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    app_settings_t cfg;
    settings_get(&cfg);
    ui_editor_cfg_t ed = {
        .title = "Server URL", .icon = ICON_LINK, .initial = cfg.ws_url,
        .placeholder = "wss://host:port/path", .max_len = SET_URL_MAX, .on_save = url_saved,
    };
    ui_editor_open(&ed, false);
}

static void key_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    app_settings_t cfg;
    settings_get(&cfg);
    ui_editor_cfg_t ed = {
        .title = "API key", .icon = ICON_KEY, .initial = cfg.ws_api_key,
        .placeholder = "x-api-key", .max_len = SET_KEY_MAX, .password = true, .on_save = key_saved,
    };
    ui_editor_open(&ed, false);
}

static void conn_cb(lv_event_t *e)
{
    voice_set_connected(lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
    vs_refresh();
}

static void reset_server_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    app_settings_t cfg;
    settings_get(&cfg);
    strlcpy(cfg.ws_url, settings_default_url(), sizeof(cfg.ws_url));
    settings_set(&cfg);
    voice_reconnect();
    vs_refresh();
    ui_toast("URL reset to secrets.h");
}

static void do_new_conversation(void *user)
{
    voice_new_conversation();
    ui_toast("New conversation started");
}

static void new_conv_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    ui_confirm("New conversation?", "The voice server starts fresh and forgets what you talked about.",
               "Start new", false, do_new_conversation, NULL);
}

static void done_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    ui_nav_pop(LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM);
}

static void vs_gesture_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev || ui_nav_busy()) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    lv_indev_wait_release(indev);            /* the touch was a swipe, not a tap on a row */
    switch (dir) {
    case LV_DIR_TOP:    ui_nav_pop(LV_SCREEN_LOAD_ANIM_MOVE_TOP); break;
    case LV_DIR_BOTTOM: ui_nav_pop(LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM); break;
    case LV_DIR_LEFT:   ui_nav_home(LV_SCREEN_LOAD_ANIM_MOVE_LEFT); break;
    case LV_DIR_RIGHT:  ui_nav_home(LV_SCREEN_LOAD_ANIM_MOVE_RIGHT); break;
    default: break;
    }
}

static lv_obj_t *voice_settings_build(void *arg)
{
    lv_obj_t *scr;
    lv_obj_t *list = ui_list_screen(&scr, "Voice server", false);
    s_vs_scr = scr;
    lv_obj_set_user_data(scr, UI_CUSTOM_GESTURES);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);   /* vertical swipes are navigation here */
    lv_obj_set_style_pad_top(list, 100, 0);

    s_vs_url = ui_row(list, ICON_LINK, UI_C_BLUE, "Server URL", "", url_cb, NULL);
    lv_label_set_long_mode(s_vs_url.value, LV_LABEL_LONG_MODE_DOTS);   /* up to 2 lines, then "..." */
    lv_obj_set_height(s_vs_url.value, 2 * lv_font_get_line_height(&lv_font_montserrat_14));
    s_vs_key = ui_row(list, ICON_KEY, UI_C_ORANGE, "API key", "", key_cb, NULL);
    s_vs_conn = ui_row_toggle(list, ICON_PLUG, UI_C_ORANGE, "Connection", "", voice_wants_connection(),
                              conn_cb, NULL);

    /* small actions under the rows */
    lv_obj_t *act = lv_obj_create(list);
    lv_obj_remove_style_all(act);
    lv_obj_set_size(act, UI_LIST_WIDTH, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(act, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(act, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(act, 18, 0);
    lv_obj_set_style_pad_top(act, 6, 0);
    lv_obj_t *rst = ui_round_button(act, ICON_RESET, &ui_icons_20, 52, ui_pal.card, ui_pal.text_dim);
    lv_obj_add_event_cb(rst, reset_server_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nc = ui_round_button(act, ICON_COMMENT, &ui_icons_20, 52, ui_pal.card, ui_pal.text_dim);
    lv_obj_add_event_cb(nc, new_conv_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *done = ui_round_button(act, ICON_DOWN, &ui_icons_20, 52, ui_pal.accent, lv_color_white());
    lv_obj_add_event_cb(done, done_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_add_event_cb(scr, vs_gesture_cb, LV_EVENT_GESTURE, NULL);

    vs_refresh();
    if (s_vs_timer) lv_timer_delete(s_vs_timer);
    s_vs_timer = lv_timer_create(vs_timer_cb, 300, NULL);
    lv_obj_add_event_cb(scr, vs_delete_cb, LV_EVENT_DELETE, NULL);
    return scr;
}

/* ===================================================== app descriptor */
static lv_obj_t *voice_app_build(void *arg) { return voice_build(arg); }
static void voice_enter(void)
{
    voice_set_mode(VOICE_MODE_CHAT);         /* the engine is shared with the Translator */
    voice_set_active(true);
}
static void voice_leave(void) { voice_set_active(false); }

const ui_app_t APP_VOICE = {
    .name = "Voice chat",
    .icon = ICON_MIC,
    .color1 = UI_C_VOICE_1,
    .color2 = UI_C_VOICE_2,
    .build = voice_app_build,
    .on_enter = voice_enter,
    .on_leave = voice_leave,
};
