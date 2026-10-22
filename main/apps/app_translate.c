/* Translator app: push-to-talk voice translator (client for the Amplify Fusion translator server).
 *
 *   touch and hold     -> talk (release to send); the translation is shown and spoken
 *   tap the language bar -> Languages: From / Into / swap
 *   swipe up / down    -> Translator server (URL, API key, connection)
 *   swipe left / right -> back to Home
 *
 * The engine is the Voice chat engine (voice_core.c) in VOICE_MODE_TRANSLATE: same recording,
 * WebSocket and playback, with the translator's URL, the language pair in the query string and
 * TRANSLATION replies. Only one of the two apps runs at a time. This file only draws, polling the
 * engine from an LVGL timer, like app_voice.c.
 *
 * Text is shown with the ui_font_latin_* / ui_font_arabic_* / ui_font_deva_* fonts, picked by the
 * script of the text: accents (Adiós, Grüße), Cyrillic, Arabic (joined, right to left) and Hindi
 * (pre-shaped by text_to_intl()). Chinese, Japanese and Korean text is replaced by the engine with
 * a short note; the spoken translation still plays.
 *
 * The Replay button plays the last spoken translation again (until the next turn). */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "esp_timer.h"

#include "apps.h"
#include "ui_widgets.h"
#include "ui_fonts.h"
#include "voice.h"
#include "translate_langs.h"
#include "settings_store.h"
#include "app_config.h"
#include "text_ascii.h"

/* Arabic needs LVGL's letter joining and right-to-left layout (sdkconfig.defaults). */
#if !LV_USE_BIDI || !LV_USE_ARABIC_PERSIAN_CHARS
#error "The Translator needs CONFIG_LV_USE_BIDI=y and CONFIG_LV_USE_ARABIC_PERSIAN_CHARS=y. They're in sdkconfig.defaults: rm sdkconfig && idf.py set-target esp32s3, then build again."
#endif

#define ORB_SIZE        120
#define REPLY_BIG_MAX   40          /* translations up to this many letters use the 28 px font */
#define REPLY_MID_MAX   110         /* up to this many: 20 px; longer: 16 px */
#define REPLY_SOLO_MIN  170         /* translations longer than this hide the original to make room */
#define REPLY_SOLO_MIN_REPLAY 100   /* ... when the Replay button takes the bottom of the screen */
#define HEARD_SHOW_MAX  120         /* the original is cut to this many letters ("...") */
#define REPLAY_W        196         /* Replay button: big enough to hit while walking... */
#define REPLAY_H        66
#define REPLAY_EXT_PX   14          /* ...and touches this close to it count as Replay too */
#define COL_H_FULL      262         /* text card height */
#define COL_H_REPLAY    216         /* ... with the Replay button under it */

/* teal accents, as in the web app */
#define TR_TEAL_DARK    0x34C7B5
#define TR_TEAL_LIGHT   0x0B7A72

/* ---- "translating" animation: three dots bouncing in a wave (as in Voice chat) ---------- */
#define DOT_COUNT     3
#define DOT_SIZE      16
#define DOT_GAP       14
#define DOT_BOUNCE    20
#define DOT_PERIOD_MS 1000
#define DOT_STAGGER   140

typedef struct {
    lv_obj_t *scr, *ring, *status, *bar, *bar_src, *bar_dst, *orb, *halo, *col;
    lv_obj_t *src_cap, *heard, *dst_cap, *reply, *col_note, *note, *dots, *hint;
    lv_obj_t *dot[DOT_COUNT];
    lv_timer_t *timer;
    voice_state_t last_state;
    uint32_t last_ver;
    lv_obj_t *replay;
    bool last_audio, last_replay, dots_on, halo_on;
    char last_src[SET_LANG_MAX + 1], last_dst[SET_LANG_MAX + 1];
} tr_ui_t;

static tr_ui_t s_ui;

static lv_color_t teal(void)
{
    return lv_color_hex(ui_theme_is_dark() ? TR_TEAL_DARK : TR_TEAL_LIGHT);
}

/* Letters (code points) in UTF-8 text */
static size_t utf8_count(const char *s)
{
    size_t n = 0;
    for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

/* Cuts UTF-8 text to at most max letters and adds "..." */
static void utf8_ellipsize(char *s, size_t max)
{
    if (utf8_count(s) <= max) return;
    size_t n = 0, i = 0;
    for (; s[i]; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80 && n++ == max - 3) break;
    }
    strcpy(s + i, "...");
}

/* The font for this text at 16, 20 or 28 px: Arabic and Devanagari have their own families
 * (which fall back to the Latin one for digits and punctuation). */
static const lv_font_t *font_for(const char *text, int size)
{
    switch (text_script(text)) {
    case TEXT_SCRIPT_ARABIC:
        return size >= 28 ? &ui_font_arabic_28 : size >= 20 ? &ui_font_arabic_20 : &ui_font_arabic_16;
    case TEXT_SCRIPT_DEVANAGARI:
        return size >= 28 ? &ui_font_deva_28 : size >= 20 ? &ui_font_deva_20 : &ui_font_deva_16;
    default:
        return size >= 28 ? &ui_font_latin_28 : size >= 20 ? &ui_font_latin_20 : &ui_font_latin_16;
    }
}

static void upper(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

/* ---- dots + halo (same as Voice chat) ---------------------------------------------------- */
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
    static const uint32_t colors[DOT_COUNT] = { 0x14B8C8, 0x2F8CFF, 0x30C26A };
    const int32_t width = DOT_COUNT * DOT_SIZE + (DOT_COUNT - 1) * DOT_GAP + 4;
    s_ui.dots = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ui.dots);
    lv_obj_set_size(s_ui.dots, width, DOT_SIZE + DOT_BOUNCE + 4);
    lv_obj_align(s_ui.dots, LV_ALIGN_CENTER, 0, 20);
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
    lv_obj_align(s_ui.dots, below_text ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_CENTER, 0, below_text ? -70 : 20);
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

static void halo_anim_cb(void *var, int32_t v)
{
    lv_obj_t *h = var;
    int32_t size = ORB_SIZE + v * 64 / 1000;
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
    uint32_t c1 = UI_C_TRANSLATE_1, c2 = UI_C_TRANSLATE_2;
    if (st == VOICE_LISTENING) { c1 = 0xFF5A4F; c2 = 0xE0306A; }
    else if (st != VOICE_READY) { c1 = 0x6B7079; c2 = 0x3E434C; }
    lv_obj_set_style_bg_color(s_ui.orb, lv_color_hex(c1), 0);
    lv_obj_set_style_bg_grad_color(s_ui.orb, lv_color_hex(c2), 0);
}

static void bar_update(void)
{
    app_settings_t cfg;
    settings_get(&cfg);
    if (!strcmp(cfg.tr_src, s_ui.last_src) && !strcmp(cfg.tr_dst, s_ui.last_dst)) return;
    strlcpy(s_ui.last_src, cfg.tr_src, sizeof(s_ui.last_src));
    strlcpy(s_ui.last_dst, cfg.tr_dst, sizeof(s_ui.last_dst));
    lv_label_set_text(s_ui.bar_src, tr_lang_name(cfg.tr_src));
    lv_label_set_text(s_ui.bar_dst, tr_lang_name(cfg.tr_dst));
}

static void ui_update(bool force)
{
    bar_update();
    voice_state_t st = voice_state();
    uint32_t ver = voice_text_version();
    bool audio = voice_audio_pending();
    bool replay = voice_can_replay();
    if (!force && st == s_ui.last_state && ver == s_ui.last_ver && audio == s_ui.last_audio &&
        replay == s_ui.last_replay) return;
    s_ui.last_state = st;
    s_ui.last_ver = ver;
    s_ui.last_audio = audio;
    s_ui.last_replay = replay;

    static char heard[380], reply[620], note[100];
    char src[SET_LANG_MAX + 1], dst[SET_LANG_MAX + 1], cap[32];
    voice_text_get(heard, sizeof(heard), reply, sizeof(reply), note, sizeof(note));
    voice_text_langs(src, sizeof(src), dst, sizeof(dst));
    bool has_text = heard[0] || reply[0];

    lv_label_set_text(s_ui.status, audio ? "Loading audio..." : voice_state_text(st));
    lv_obj_set_style_border_color(s_ui.ring, lv_color_hex(state_color(st)), 0);

    /* center: the orb (idle / listening / connecting), the dots, or the translation card */
    bool thinking = (st == VOICE_THINKING && !reply[0]);
    if (has_text) {
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
    halo_set(st == VOICE_LISTENING && !has_text);
    dots_set(thinking, has_text);

    /* the original, unless the translation needs the room */
    bool solo = utf8_count(reply) > (replay ? REPLY_SOLO_MIN_REPLAY : REPLY_SOLO_MIN);
    if (heard[0] && !solo) {
        utf8_ellipsize(heard, HEARD_SHOW_MAX);
        strlcpy(cap, tr_lang_name(src), sizeof(cap));
        upper(cap);
        lv_label_set_text(s_ui.src_cap, cap);
        lv_obj_set_style_text_font(s_ui.heard, font_for(heard, 16), 0);
        lv_label_set_text(s_ui.heard, heard);
        lv_obj_remove_flag(s_ui.src_cap, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.heard, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.src_cap, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.heard, LV_OBJ_FLAG_HIDDEN);
    }

    /* the translation */
    if (reply[0]) {
        strlcpy(cap, tr_lang_name(dst), sizeof(cap));
        upper(cap);
        lv_label_set_text(s_ui.dst_cap, cap);
        size_t n = utf8_count(reply);
        lv_obj_set_style_text_font(s_ui.reply, font_for(reply, n <= REPLY_BIG_MAX ? 28 : n <= REPLY_MID_MAX ? 20 : 16), 0);
        lv_label_set_text(s_ui.reply, reply);
        lv_obj_remove_flag(s_ui.dst_cap, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.reply, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.dst_cap, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.reply, LV_OBJ_FLAG_HIDDEN);
    }

    /* a note goes in the card when there's text, else under the orb */
    lv_label_set_text(s_ui.col_note, has_text ? note : "");
    if (has_text && note[0]) lv_obj_remove_flag(s_ui.col_note, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_ui.col_note, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_style_text_color(s_ui.note, ui_pal.warn, 0);
    if (st == VOICE_DISCONNECTED && !has_text) {
        char buf[140];
        snprintf(buf, sizeof(buf), "%s%sTap to connect", note, note[0] ? "\n" : "");
        lv_label_set_text(s_ui.note, buf);
    } else {
        lv_label_set_text(s_ui.note, (!has_text && st != VOICE_LISTENING) ? note : "");
    }

    /* Replay: once a spoken translation is there, until the next turn */
    if (has_text && replay) lv_obj_remove_flag(s_ui.replay, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_ui.replay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(s_ui.col, has_text && replay ? COL_H_REPLAY : COL_H_FULL);   /* the button takes the bottom */

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
    if (s_ui.timer) lv_timer_delete(s_ui.timer);
    for (int i = 0; i < DOT_COUNT; i++) lv_anim_delete(s_ui.dot[i], dot_anim_cb);
    lv_anim_delete(s_ui.halo, halo_anim_cb);
    memset(&s_ui, 0, sizeof(s_ui));
}

/* ---- touch: hold to talk, swipe to navigate, tap the language bar ------------------------ */
static lv_obj_t *server_build(void *arg);
static lv_obj_t *langs_build(void *arg);

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
        if (!s_swiped) voice_ptt_press();    /* record from the first moment: the first word counts */
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
            ui_nav_push(server_build, NULL,
                        dy < 0 ? LV_SCREEN_LOAD_ANIM_MOVE_TOP : LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM);
        } else {
            ui_nav_home(dx < 0 ? LV_SCREEN_LOAD_ANIM_MOVE_LEFT : LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s_swiped) voice_ptt_release();
    }
}

static void replay_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    if (!voice_replay()) ui_toast("Nothing to replay");
}

static void bar_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    voice_state_t st = voice_state();
    if (st == VOICE_LISTENING) return;       /* a finger is already talking */
    ui_nav_push(langs_build, NULL, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

static lv_obj_t *tr_build(void *arg)
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
    lv_obj_align(s_ui.status, LV_ALIGN_TOP_MID, 0, 50);

    /* listening halo + mic orb */
    s_ui.halo = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ui.halo);
    lv_obj_set_size(s_ui.halo, ORB_SIZE, ORB_SIZE);
    lv_obj_align(s_ui.halo, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_radius(s_ui.halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_ui.halo, 6, 0);
    lv_obj_set_style_border_color(s_ui.halo, lv_color_hex(0xFF5A4F), 0);
    lv_obj_remove_flag(s_ui.halo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_ui.halo, LV_OBJ_FLAG_HIDDEN);

    s_ui.orb = ui_orb(scr, ORB_SIZE, UI_C_TRANSLATE_1, UI_C_TRANSLATE_2, ICON_MIC, &ui_icons_56);
    lv_obj_align(s_ui.orb, LV_ALIGN_CENTER, 0, 20);
    lv_obj_remove_flag(s_ui.orb, LV_OBJ_FLAG_CLICKABLE);

    /* The translation card: original (small, dim) above, translation (big) below.
     * Anything that doesn't fit is clipped inside the column. */
    s_ui.col = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ui.col);
    lv_obj_set_size(s_ui.col, 330, COL_H_FULL);   /* shorter while the Replay button shows */
    lv_obj_align(s_ui.col, LV_ALIGN_TOP_MID, 0, 146);
    lv_obj_set_flex_flow(s_ui.col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_ui.col, 4, 0);
    lv_obj_set_style_pad_top(s_ui.col, 8, 0);
    lv_obj_remove_flag(s_ui.col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_ui.col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.col, LV_OBJ_FLAG_HIDDEN);

    s_ui.src_cap = ui_label(s_ui.col, &lv_font_montserrat_14, ui_pal.text_dim, "");
    lv_obj_set_style_text_letter_space(s_ui.src_cap, 1, 0);
    s_ui.heard = ui_label(s_ui.col, &ui_font_latin_16, ui_pal.text_dim, "");
    lv_obj_set_width(s_ui.heard, LV_PCT(100));
    lv_label_set_long_mode(s_ui.heard, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_ui.heard, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_bottom(s_ui.heard, 10, 0);

    s_ui.dst_cap = ui_label(s_ui.col, &lv_font_montserrat_14, teal(), "");
    lv_obj_set_style_text_letter_space(s_ui.dst_cap, 1, 0);
    s_ui.reply = ui_label(s_ui.col, &ui_font_latin_28, ui_pal.text, "");
    lv_obj_set_width(s_ui.reply, LV_PCT(100));
    lv_label_set_long_mode(s_ui.reply, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_ui.reply, LV_TEXT_ALIGN_CENTER, 0);

    s_ui.col_note = ui_label(s_ui.col, &ui_font_latin_14, ui_pal.warn, "");
    lv_obj_set_width(s_ui.col_note, LV_PCT(90));
    lv_label_set_long_mode(s_ui.col_note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_ui.col_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(s_ui.col_note, 6, 0);

    dots_build(scr);

    s_ui.note = ui_label(scr, &ui_font_latin_14, ui_pal.warn, "");
    lv_obj_set_width(s_ui.note, 300);
    lv_label_set_long_mode(s_ui.note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_ui.note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_ui.note, LV_ALIGN_CENTER, 0, 20 + ORB_SIZE / 2 + 30);

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
    lv_obj_align(s_ui.hint, LV_ALIGN_BOTTOM_MID, 0, -48);

    /* invisible full-screen touch area on top of everything but the language bar */
    lv_obj_t *touch = lv_obj_create(scr);
    lv_obj_remove_style_all(touch);
    lv_obj_set_size(touch, LV_PCT(100), LV_PCT(100));
    lv_obj_center(touch);
    lv_obj_add_flag(touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(touch, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(touch, touch_event_cb, LV_EVENT_ALL, NULL);

    /* language bar: "English -> Spanish", tap for Languages (created last: it's above the touch area) */
    s_ui.bar = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ui.bar);
    lv_obj_set_size(s_ui.bar, LV_SIZE_CONTENT, 46);
    lv_obj_align(s_ui.bar, LV_ALIGN_TOP_MID, 0, 86);
    lv_obj_set_style_radius(s_ui.bar, 23, 0);
    lv_obj_set_style_bg_color(s_ui.bar, ui_pal.card, 0);
    lv_obj_set_style_bg_color(s_ui.bar, ui_pal.card_press, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_ui.bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_ui.bar, 20, 0);
    lv_obj_set_style_pad_column(s_ui.bar, 12, 0);
    lv_obj_set_flex_flow(s_ui.bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ui.bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_ui.bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_ui.bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ui.bar, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_ui.bar, bar_cb, LV_EVENT_CLICKED, NULL);
    s_ui.bar_src = ui_label(s_ui.bar, &lv_font_montserrat_20, ui_pal.text, "");
    ui_icon(s_ui.bar, &ui_icons_16, teal(), ICON_ARROW_RIGHT);
    s_ui.bar_dst = ui_label(s_ui.bar, &lv_font_montserrat_20, teal(), "");

    /* Replay: plays the last spoken translation again. It's above the hold-to-talk area, so a
     * touch on it (or within REPLAY_EXT_PX of it) never starts a recording. */
    s_ui.replay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ui.replay);
    lv_obj_set_size(s_ui.replay, REPLAY_W, REPLAY_H);
    lv_obj_align(s_ui.replay, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_ext_click_area(s_ui.replay, REPLAY_EXT_PX);
    lv_obj_set_style_radius(s_ui.replay, REPLAY_H / 2, 0);
    lv_obj_set_style_bg_color(s_ui.replay, lv_color_hex(UI_C_TRANSLATE_2), 0);
    lv_obj_set_style_bg_color(s_ui.replay, lv_color_hex(UI_C_TRANSLATE_1), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_ui.replay, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_scale(s_ui.replay, 240, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(s_ui.replay, REPLAY_W / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_ui.replay, REPLAY_H / 2, 0);
    lv_obj_set_style_pad_column(s_ui.replay, 10, 0);
    lv_obj_set_flex_flow(s_ui.replay, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ui.replay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_ui.replay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_ui.replay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ui.replay, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_ui.replay, replay_cb, LV_EVENT_CLICKED, NULL);
    ui_icon(s_ui.replay, &ui_icons_28, lv_color_white(), ICON_VOLUME);
    ui_label(s_ui.replay, &lv_font_montserrat_22, lv_color_white(), "Replay");
    lv_obj_add_flag(s_ui.replay, LV_OBJ_FLAG_HIDDEN);

    ui_update(true);
    s_ui.timer = lv_timer_create(timer_cb, 40, NULL);
    lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);
    return scr;
}

/* ===================================================== Languages (tap the language bar) */
static void languages_changed(const app_settings_t *cfg)
{
    settings_set(cfg);
    voice_reconnect_when_idle();             /* the pair is part of the connection URL */
}

static lv_obj_t *picker_build(void *arg);

static void from_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    ui_nav_push(picker_build, (void *)1, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

static void into_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    ui_nav_push(picker_build, (void *)0, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

static void swap_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    app_settings_t cfg;
    settings_get(&cfg);
    if (!tr_can_swap(cfg.tr_src, cfg.tr_dst)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Can't translate into %s", tr_lang_name(cfg.tr_src));
        ui_toast(msg);
        return;
    }
    char tmp[SET_LANG_MAX + 1];
    strlcpy(tmp, cfg.tr_src, sizeof(tmp));
    strlcpy(cfg.tr_src, cfg.tr_dst, sizeof(cfg.tr_src));
    strlcpy(cfg.tr_dst, tmp, sizeof(cfg.tr_dst));
    languages_changed(&cfg);
    ui_nav_refresh();                        /* rebuild this screen with the new pair */
}

static lv_obj_t *langs_build(void *arg)
{
    app_settings_t cfg;
    settings_get(&cfg);
    lv_obj_t *scr;
    lv_obj_t *list = ui_list_screen(&scr, "Languages", true);

    ui_section(list, "I SPEAK");
    ui_row(list, ICON_MIC, UI_C_TEAL, "From", tr_lang_name(cfg.tr_src), from_cb, NULL);
    ui_section(list, "TRANSLATE INTO");
    ui_row(list, ICON_LANGUAGE, UI_C_BLUE, "Into", tr_lang_name(cfg.tr_dst), into_cb, NULL);

    char v[64];
    bool can = tr_can_swap(cfg.tr_src, cfg.tr_dst);
    if (can) snprintf(v, sizeof(v), "%s to %s", tr_lang_name(cfg.tr_dst), tr_lang_name(cfg.tr_src));
    else snprintf(v, sizeof(v), "%s can only be spoken", tr_lang_name(cfg.tr_src));
    ui_row_t sw = ui_row(list, ICON_SWAP, can ? UI_C_PURPLE : UI_C_GRAY, "Swap", v, swap_cb, NULL);
    if (!can) lv_obj_set_style_text_color(sw.title, ui_pal.text_dim, 0);

    lv_obj_t *n = ui_label(list, &lv_font_montserrat_14, ui_pal.text_dim,
                           "Chinese, Japanese, Korean and Russian can be spoken but not translated into. "
                           "Text in other alphabets isn't shown on screen; the translation is still spoken.");
    lv_obj_set_width(n, UI_LIST_WIDTH - 40);
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(n, 8, 0);
    return scr;
}

/* ---- language picker: arg 1 = From, 0 = Into ----------------------------------------- */
static void pick_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    bool from = (v & 0x100) != 0;
    const tr_lang_t *l = &g_tr_langs[v & 0xFF];

    app_settings_t cfg;
    settings_get(&cfg);
    char old_src[SET_LANG_MAX + 1], old_dst[SET_LANG_MAX + 1];
    strlcpy(old_src, cfg.tr_src, sizeof(old_src));
    strlcpy(old_dst, cfg.tr_dst, sizeof(old_dst));
    tr_pick(cfg.tr_src, cfg.tr_dst, sizeof(cfg.tr_src), l->code, from);
    if (strcmp(old_src, cfg.tr_src) || strcmp(old_dst, cfg.tr_dst)) {
        languages_changed(&cfg);
        /* say so when picking moved the other side too */
        if (from && strcmp(old_dst, cfg.tr_dst)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Into is now %s", tr_lang_name(cfg.tr_dst));
            ui_toast(msg);
        } else if (!from && strcmp(old_src, cfg.tr_src)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "From is now %s", tr_lang_name(cfg.tr_src));
            ui_toast(msg);
        }
    }
    ui_nav_pop(LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
}

static lv_obj_t *picker_build(void *arg)
{
    bool from = arg != NULL;
    app_settings_t cfg;
    settings_get(&cfg);
    const char *cur = from ? cfg.tr_src : cfg.tr_dst;

    lv_obj_t *scr;
    lv_obj_t *list = ui_list_screen(&scr, from ? "I speak" : "Translate into", true);
    lv_obj_t *selected = NULL;
    for (int i = 0; i < g_tr_lang_count; i++) {
        const tr_lang_t *l = &g_tr_langs[i];
        if (from ? !l->source : !l->target) continue;
        const char *value = l->native && strcmp(l->native, l->name) ? l->native : NULL;
        if (from && !l->target) value = "Spoken only";
        intptr_t ud = i | (from ? 0x100 : 0);
        ui_row_t r = ui_row(list, NULL, 0, l->name, value, pick_cb, (void *)ud);
        if (r.value) {
            /* Español, Русский: 14 px; العربية, हिंदी: their own fonts (16 px is the smallest) */
            lv_obj_set_style_text_font(r.value, text_script(value) == TEXT_SCRIPT_LATIN ? &ui_font_latin_14 :
                                       font_for(value, 16), 0);
        }
        else lv_obj_set_height(r.row, 62);   /* a fixed height centers a lone title */
        if (strcmp(l->code, cur) == 0) {
            /* the current language: a check mark instead of the chevron, teal title */
            lv_obj_t *chev = lv_obj_get_child(r.row, lv_obj_get_child_count(r.row) - 1);
            lv_label_set_text(chev, ICON_CHECK);
            lv_obj_set_style_text_color(chev, teal(), 0);
            lv_obj_set_style_text_color(r.title, teal(), 0);
            selected = r.row;
        }
    }
    if (selected) {
        lv_obj_update_layout(scr);
        lv_obj_scroll_to_view(selected, LV_ANIM_OFF);
    }
    return scr;
}

/* ===================================================== Translator server (swipe up/down) */
static lv_obj_t  *s_ts_scr;
static ui_row_t   s_ts_url, s_ts_key;
static ui_toggle_row_t s_ts_conn;
static lv_timer_t *s_ts_timer;

static void ts_refresh(void)
{
    app_settings_t cfg;
    settings_get(&cfg);
    char ep[SET_URL_MAX + 32];
    if (cfg.tr_url[0]) voice_tr_endpoint(cfg.tr_url, ep, sizeof(ep));
    lv_label_set_text(s_ts_url.value, cfg.tr_url[0] ? ep : "Not set");
    char masked[48];
    ui_mask_secret(cfg.tr_api_key, masked, sizeof(masked), true);
    lv_label_set_text(s_ts_key.value, masked);

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
    lv_label_set_text(s_ts_conn.value, txt);
    lv_obj_set_style_bg_color(lv_obj_get_child(s_ts_conn.row, 0), lv_color_hex(c), 0);
    if (want != lv_obj_has_state(s_ts_conn.sw, LV_STATE_CHECKED)) {
        if (want) lv_obj_add_state(s_ts_conn.sw, LV_STATE_CHECKED);
        else lv_obj_remove_state(s_ts_conn.sw, LV_STATE_CHECKED);
    }
}

static void ts_timer_cb(lv_timer_t *t) { ts_refresh(); }

static void ts_delete_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_ts_scr) return;
    if (s_ts_timer) lv_timer_delete(s_ts_timer);
    s_ts_timer = NULL;
    s_ts_scr = NULL;
}

static bool url_saved(const char *text, void *user)
{
    if (strncmp(text, "wss://", 6) != 0 && strncmp(text, "ws://", 5) != 0) {
        ui_toast("Start the URL with wss://");
        return true;                       /* stay in the editor */
    }
    app_settings_t cfg;
    settings_get(&cfg);
    strlcpy(cfg.tr_url, text, sizeof(cfg.tr_url));
    settings_set(&cfg);
    voice_reconnect();
    ui_toast("Saved - reconnecting");
    return false;
}

static bool key_saved(const char *text, void *user)
{
    app_settings_t cfg;
    settings_get(&cfg);
    strlcpy(cfg.tr_api_key, text, sizeof(cfg.tr_api_key));
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
        .title = "Server URL", .icon = ICON_LINK, .initial = cfg.tr_url,
        .placeholder = "wss://host:port/translator", .max_len = SET_URL_MAX, .on_save = url_saved,
    };
    ui_editor_open(&ed, false);
}

static void key_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    app_settings_t cfg;
    settings_get(&cfg);
    ui_editor_cfg_t ed = {
        .title = "API key", .icon = ICON_KEY, .initial = cfg.tr_api_key,
        .placeholder = "x-api-key", .max_len = SET_KEY_MAX, .password = true, .on_save = key_saved,
    };
    ui_editor_open(&ed, false);
}

static void conn_cb(lv_event_t *e)
{
    voice_set_connected(lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
    ts_refresh();
}

static void reset_server_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    app_settings_t cfg;
    settings_get(&cfg);
    strlcpy(cfg.tr_url, settings_default_tr_url(), sizeof(cfg.tr_url));
    settings_set(&cfg);
    voice_reconnect();
    ts_refresh();
    ui_toast("URL reset to secrets.h");
}

static void ts_langs_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    ui_nav_replace(langs_build, NULL, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

static void done_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    ui_nav_pop(LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM);
}

static void ts_gesture_cb(lv_event_t *e)
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

static lv_obj_t *server_build(void *arg)
{
    lv_obj_t *scr;
    lv_obj_t *list = ui_list_screen(&scr, "Translator server", false);
    s_ts_scr = scr;
    lv_obj_set_user_data(scr, UI_CUSTOM_GESTURES);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);   /* vertical swipes are navigation here */
    lv_obj_set_style_pad_top(list, 100, 0);

    s_ts_url = ui_row(list, ICON_LINK, UI_C_BLUE, "Server URL", "", url_cb, NULL);
    lv_label_set_long_mode(s_ts_url.value, LV_LABEL_LONG_MODE_DOTS);   /* up to 2 lines, then "..." */
    lv_obj_set_height(s_ts_url.value, 2 * lv_font_get_line_height(&lv_font_montserrat_14));
    s_ts_key = ui_row(list, ICON_KEY, UI_C_ORANGE, "API key", "", key_cb, NULL);
    s_ts_conn = ui_row_toggle(list, ICON_PLUG, UI_C_ORANGE, "Connection", "", voice_wants_connection(),
                              conn_cb, NULL);

    /* small actions under the rows: reset URL, languages, done */
    lv_obj_t *act = lv_obj_create(list);
    lv_obj_remove_style_all(act);
    lv_obj_set_size(act, UI_LIST_WIDTH, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(act, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(act, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(act, 18, 0);
    lv_obj_set_style_pad_top(act, 6, 0);
    lv_obj_t *rst = ui_round_button(act, ICON_RESET, &ui_icons_20, 52, ui_pal.card, ui_pal.text_dim);
    lv_obj_add_event_cb(rst, reset_server_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lg = ui_round_button(act, ICON_LANGUAGE, &ui_icons_20, 52, ui_pal.card, ui_pal.text_dim);
    lv_obj_add_event_cb(lg, ts_langs_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *done = ui_round_button(act, ICON_DOWN, &ui_icons_20, 52, ui_pal.accent, lv_color_white());
    lv_obj_add_event_cb(done, done_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_add_event_cb(scr, ts_gesture_cb, LV_EVENT_GESTURE, NULL);

    ts_refresh();
    if (s_ts_timer) lv_timer_delete(s_ts_timer);
    s_ts_timer = lv_timer_create(ts_timer_cb, 300, NULL);
    lv_obj_add_event_cb(scr, ts_delete_cb, LV_EVENT_DELETE, NULL);
    return scr;
}

/* ===================================================== app descriptor */
static lv_obj_t *tr_app_build(void *arg) { return tr_build(arg); }

static void tr_enter(void)
{
    voice_set_mode(VOICE_MODE_TRANSLATE);
    voice_set_active(true);
}

static void tr_leave(void) { voice_set_active(false); }

const ui_app_t APP_TRANSLATE = {
    .name = "Translator",
    .icon = ICON_LANGUAGE,
    .color1 = UI_C_TRANSLATE_1,
    .color2 = UI_C_TRANSLATE_2,
    .build = tr_app_build,
    .on_enter = tr_enter,
    .on_leave = tr_leave,
};
