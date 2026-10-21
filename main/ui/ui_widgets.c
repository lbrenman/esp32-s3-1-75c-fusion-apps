#include <string.h>
#include <stdio.h>
#include "ui_widgets.h"
#include "ui_nav.h"

#define CENTER (UI_SCREEN_SIZE / 2)

/* ------------------------------------------------------------------ basics */
static void plain(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *ui_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, ui_pal.bg, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

lv_obj_t *ui_icon(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *icon)
{
    return ui_label(parent, font, color, icon);
}

lv_obj_t *ui_round_button(lv_obj_t *parent, const char *icon, const lv_font_t *icon_font,
                          int size, lv_color_t bg, lv_color_t fg)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, ui_pal.card_press, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_t *i = ui_icon(b, icon_font, fg, icon);
    lv_obj_center(i);
    return b;
}

lv_obj_t *ui_orb(lv_obj_t *parent, int size, uint32_t c1, uint32_t c2, const char *icon,
                 const lv_font_t *icon_font)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, size, size);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(c1), 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(c2), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    /* pressed: shrink a little */
    lv_obj_set_style_transform_pivot_x(o, size / 2, 0);
    lv_obj_set_style_transform_pivot_y(o, size / 2, 0);
    lv_obj_set_style_transform_scale(o, 236, LV_STATE_PRESSED);
    if (icon) {
        lv_obj_t *i = ui_icon(o, icon_font, lv_color_white(), icon);
        lv_obj_center(i);
    }
    return o;
}

/* ------------------------------------------------------------- list screen */

/* Rows fade out as they scroll toward the curved top and bottom edges. */
static void list_fade(lv_obj_t *list)
{
    uint32_t n = lv_obj_get_child_count(list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(list, i);
        lv_area_t a;
        lv_obj_get_coords(c, &a);
        int32_t cy = (a.y1 + a.y2) / 2;
        int32_t d = LV_ABS(cy - CENTER);
        lv_opa_t opa = LV_OPA_COVER;
        if (d > 130) {
            int32_t v = 255 - (d - 130) * 215 / 100;
            opa = (lv_opa_t)LV_CLAMP(30, v, 255);
        }
        if (lv_obj_get_style_opa(c, 0) != opa) lv_obj_set_style_opa(c, opa, 0);
    }
}

static void list_event_cb(lv_event_t *e)
{
    list_fade(lv_event_get_target_obj(e));
}

static void back_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    ui_nav_pop(LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
}

/* Swipe right on a list screen = back. Screens that handle swipes themselves set
 * UI_CUSTOM_GESTURES as their user data. */
static void list_gesture_cb(lv_event_t *e)
{
    lv_obj_t *scr = lv_event_get_current_target_obj(e);
    if (lv_obj_get_user_data(scr) == UI_CUSTOM_GESTURES) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev || lv_indev_get_gesture_dir(indev) != LV_DIR_RIGHT || ui_nav_busy()) return;
    lv_indev_wait_release(indev);
    ui_nav_pop(LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
}

static lv_obj_t *edge_fade(lv_obj_t *scr, bool top, int h)
{
    lv_obj_t *f = lv_obj_create(scr);
    plain(f);
    lv_obj_set_size(f, UI_SCREEN_SIZE, h);
    lv_obj_align(f, top ? LV_ALIGN_TOP_MID : LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(f, ui_pal.bg, 0);
    lv_obj_set_style_bg_grad_color(f, ui_pal.bg, 0);
    lv_obj_set_style_bg_grad_dir(f, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_opa(f, top ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_opa(f, top ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_main_stop(f, top ? 150 : 40, 0);
    lv_obj_set_style_bg_grad_stop(f, top ? 255 : 215, 0);
    return f;
}

lv_obj_t *ui_list_screen(lv_obj_t **out_screen, const char *title, bool back)
{
    lv_obj_t *scr = ui_screen();

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, UI_LIST_WIDTH + 8, UI_SCREEN_SIZE);
    lv_obj_center(list);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(list, 96, 0);
    lv_obj_set_style_pad_bottom(list, 130, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_add_event_cb(list, list_event_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(list, list_event_cb, LV_EVENT_SIZE_CHANGED, NULL);
    lv_obj_add_event_cb(list, list_event_cb, LV_EVENT_SCREEN_LOADED, NULL);

    lv_obj_add_event_cb(scr, list_gesture_cb, LV_EVENT_GESTURE, NULL);

    edge_fade(scr, true, 112);
    edge_fade(scr, false, 110);

    /* header: [<] Title, centered */
    lv_obj_t *hdr = lv_obj_create(scr);
    plain(hdr);
    lv_obj_set_size(hdr, LV_SIZE_CONTENT, 48);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 10, 0);
    if (back) {
        lv_obj_t *b = ui_round_button(hdr, ICON_LEFT, &ui_icons_16, 40, ui_pal.card, ui_pal.text);
        lv_obj_add_event_cb(b, back_cb, LV_EVENT_CLICKED, NULL);
    }
    ui_label(hdr, &lv_font_montserrat_24, ui_pal.text, title);
    if (back) {
        /* invisible spacer keeps the title centered */
        lv_obj_t *sp = lv_obj_create(hdr);
        plain(sp);
        lv_obj_set_size(sp, 40, 40);
    }

    if (out_screen) *out_screen = scr;
    return list;
}

lv_obj_t *ui_section(lv_obj_t *list, const char *text)
{
    lv_obj_t *l = ui_label(list, &lv_font_montserrat_14, ui_pal.text_dim, text);
    lv_obj_set_width(l, UI_LIST_WIDTH - 28);
    lv_obj_set_style_pad_top(l, 10, 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    return l;
}

/* -------------------------------------------------------------------- rows */
static lv_obj_t *row_base(lv_obj_t *list, const char *icon, uint32_t icon_color, bool clickable)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, UI_LIST_WIDTH);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, 62, 0);
    lv_obj_set_style_radius(row, 20, 0);
    lv_obj_set_style_bg_color(row, ui_pal.card, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_set_style_pad_ver(row, 10, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (clickable) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, ui_pal.card_press, LV_STATE_PRESSED);
    } else {
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }

    if (icon) {
        lv_obj_t *box = lv_obj_create(row);
        plain(box);
        lv_obj_set_size(box, 36, 36);
        lv_obj_set_style_radius(box, 11, 0);
        lv_obj_set_style_bg_color(box, lv_color_hex(icon_color), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_t *i = ui_icon(box, &ui_icons_20, lv_color_white(), icon);
        lv_obj_center(i);
    }
    return row;
}

static lv_obj_t *text_col(lv_obj_t *row)
{
    lv_obj_t *col = lv_obj_create(row);
    plain(col);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    return col;
}

ui_row_t ui_row(lv_obj_t *list, const char *icon, uint32_t icon_color, const char *title,
                const char *value, lv_event_cb_t on_click, void *user)
{
    ui_row_t r = { 0 };
    r.row = row_base(list, icon, icon_color, on_click != NULL);
    lv_obj_t *col = text_col(r.row);
    r.title = ui_label(col, &lv_font_montserrat_16, ui_pal.text, title);
    if (value) {
        r.value = ui_label(col, &lv_font_montserrat_14, ui_pal.text_dim, value);
        lv_obj_set_width(r.value, LV_PCT(100));
        lv_label_set_long_mode(r.value, LV_LABEL_LONG_MODE_DOTS);
    }
    if (on_click) {
        ui_icon(r.row, &ui_icons_16, ui_pal.text_dim, ICON_RIGHT);
        lv_obj_add_event_cb(r.row, on_click, LV_EVENT_CLICKED, user);
    }
    return r;
}

lv_obj_t *ui_row_switch(lv_obj_t *list, const char *icon, uint32_t icon_color, const char *title,
                        bool on, lv_event_cb_t on_change, void *user)
{
    lv_obj_t *row = row_base(list, icon, icon_color, false);
    lv_obj_t *col = text_col(row);
    ui_label(col, &lv_font_montserrat_16, ui_pal.text, title);
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 56, 32);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_GESTURE_BUBBLE);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, on_change, LV_EVENT_VALUE_CHANGED, user);
    return sw;
}

ui_toggle_row_t ui_row_toggle(lv_obj_t *list, const char *icon, uint32_t icon_color, const char *title,
                              const char *value, bool on, lv_event_cb_t on_change, void *user)
{
    ui_toggle_row_t r;
    r.row = row_base(list, icon, icon_color, false);
    lv_obj_t *col = text_col(r.row);
    ui_label(col, &lv_font_montserrat_16, ui_pal.text, title);
    r.value = ui_label(col, &lv_font_montserrat_14, ui_pal.text_dim, value);
    lv_obj_set_width(r.value, LV_PCT(100));
    lv_label_set_long_mode(r.value, LV_LABEL_LONG_MODE_DOTS);
    r.sw = lv_switch_create(r.row);
    lv_obj_set_size(r.sw, 56, 32);
    lv_obj_remove_flag(r.sw, LV_OBJ_FLAG_GESTURE_BUBBLE);
    if (on) lv_obj_add_state(r.sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(r.sw, on_change, LV_EVENT_VALUE_CHANGED, user);
    return r;
}

static void slider_label_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target_obj(e);
    lv_obj_t *lbl = lv_obj_get_user_data(s);
    if (lbl) lv_label_set_text_fmt(lbl, "%d%%", (int)lv_slider_get_value(s));
}

lv_obj_t *ui_row_slider(lv_obj_t *list, const char *icon, uint32_t icon_color, const char *title,
                        int min, int max, int value, lv_event_cb_t cb, void *user)
{
    lv_obj_t *row = row_base(list, icon, icon_color, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(row, 14, 0);
    lv_obj_t *col = text_col(row);
    ui_label(col, &lv_font_montserrat_16, ui_pal.text, title);
    lv_obj_t *val = ui_label(row, &lv_font_montserrat_14, ui_pal.text_dim, "");
    lv_label_set_text_fmt(val, "%d%%", value);

    lv_obj_t *s = lv_slider_create(row);
    lv_obj_set_width(s, LV_PCT(100));
    lv_obj_set_height(s, 10);
    lv_obj_set_style_pad_hor(s, 8, 0);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_ext_click_area(s, 16);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_GESTURE_BUBBLE);   /* dragging the slider isn't "swipe = back" */
    lv_obj_set_user_data(s, val);
    lv_obj_add_event_cb(s, slider_label_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, user);
    lv_obj_add_event_cb(s, cb, LV_EVENT_RELEASED, user);
    return s;
}

lv_obj_t *ui_hint(lv_obj_t *scr, const char *icon, const char *text)
{
    lv_obj_t *h = lv_obj_create(scr);
    plain(h);
    lv_obj_set_size(h, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(h, 6, 0);
    if (icon) ui_icon(h, &ui_icons_16, ui_pal.text_dim, icon);
    ui_label(h, &lv_font_montserrat_14, ui_pal.text_dim, text);
    lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -44);
    return h;
}

/* ----------------------------------------------------------- toast + confirm */
static void opa_anim_cb(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }

void ui_toast(const char *text)
{
    lv_obj_t *t = lv_obj_create(lv_layer_top());
    plain(t);
    lv_obj_set_size(t, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(t, ui_theme_is_dark() ? lv_color_hex(0x2C2E35) : lv_color_hex(0x22242A), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(t, 20, 0);
    lv_obj_set_style_pad_ver(t, 10, 0);
    lv_obj_t *l = ui_label(t, &lv_font_montserrat_16, lv_color_white(), text);
    lv_obj_set_style_max_width(l, 300, 0);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -90);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, t);
    lv_anim_set_exec_cb(&a, opa_anim_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&a, 1600);
    lv_anim_set_duration(&a, 300);
    lv_anim_set_deleted_cb(&a, NULL);
    lv_anim_start(&a);
    lv_obj_delete_delayed(t, 2000);
}

typedef struct {
    void (*on_ok)(void *);
    void *user;
} confirm_t;

static void confirm_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    lv_obj_t *overlay = lv_event_get_user_data(e);
    confirm_t *c = lv_obj_get_user_data(overlay);
    bool ok = (bool)(uintptr_t)lv_obj_get_user_data(btn);
    confirm_t copy = *c;
    lv_free(c);
    lv_obj_delete(overlay);
    if (ok && copy.on_ok) copy.on_ok(copy.user);
}

static lv_obj_t *dialog_button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_color_t fg,
                               bool ok, lv_obj_t *overlay)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 124, 48);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(b, (void *)(uintptr_t)ok);
    lv_obj_t *l = ui_label(b, &lv_font_montserrat_16, fg, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, confirm_btn_cb, LV_EVENT_CLICKED, overlay);
    return b;
}

void ui_confirm(const char *title, const char *msg, const char *ok_text, bool danger,
                void (*on_ok)(void *user), void *user)
{
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, UI_SCREEN_SIZE, UI_SCREEN_SIZE);
    lv_obj_set_style_bg_color(ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_60, 0);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);       /* blocks touches to the screen below */
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    confirm_t *c = lv_malloc(sizeof(confirm_t));
    c->on_ok = on_ok;
    c->user = user;
    lv_obj_set_user_data(ov, c);

    lv_obj_t *card = lv_obj_create(ov);
    plain(card);
    lv_obj_set_size(card, 330, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 32, 0);
    lv_obj_set_style_bg_color(card, ui_pal.card, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(card, 22, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    ui_label(card, &lv_font_montserrat_20, ui_pal.text, title);
    lv_obj_t *m = ui_label(card, &lv_font_montserrat_14, ui_pal.text_dim, msg);
    lv_obj_set_width(m, LV_PCT(100));
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_t *btns = lv_obj_create(card);
    plain(btns);
    lv_obj_set_size(btns, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(btns, 8, 0);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    dialog_button(btns, "Cancel", ui_pal.card_press, ui_pal.text, false, ov);
    dialog_button(btns, ok_text, danger ? ui_pal.err : ui_pal.accent, lv_color_white(), true, ov);
}

void ui_mask_secret(const char *in, char *out, size_t out_len, bool show_tail)
{
    size_t n = strlen(in);
    if (n == 0) {
        strlcpy(out, "Not set", out_len);
        return;
    }
    /* "\xE2\x80\xA2" is the bullet (U+2022), which the built-in Montserrat fonts include */
    out[0] = '\0';
    for (int i = 0; i < 8 && strlen(out) + 4 < out_len; i++) strcat(out, "\xE2\x80\xA2");
    if (show_tail && n > 8 && strlen(out) + 5 < out_len) strcat(out, in + n - 4);
}

/* -------------------------------------------------------------------- editor */
static ui_editor_cfg_t s_ed;
static char            s_ed_title[64];
static char            s_ed_text[256];
static lv_obj_t       *s_ed_ta;

static void editor_save(void)
{
    const char *txt = lv_textarea_get_text(s_ed_ta);
    char buf[sizeof(s_ed_text)];
    strlcpy(buf, txt, sizeof(buf));
    int pop = s_ed.pop_after > 0 ? s_ed.pop_after : 1;
    bool navigated = s_ed.on_save ? s_ed.on_save(buf, s_ed.user) : false;
    if (!navigated) ui_nav_pop_n(pop, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
}

static void editor_kb_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) editor_save();
    else if (code == LV_EVENT_CANCEL) ui_nav_pop(LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
}

static void editor_btn_cb(lv_event_t *e)
{
    if (ui_nav_busy()) return;
    if ((bool)(uintptr_t)lv_event_get_user_data(e)) editor_save();
    else ui_nav_pop(LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
}

static void editor_eye_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    bool hidden = lv_textarea_get_password_mode(s_ed_ta);
    lv_textarea_set_password_mode(s_ed_ta, !hidden);
    lv_obj_t *icon = lv_obj_get_child(btn, 0);
    lv_label_set_text(icon, hidden ? ICON_EYE_SLASH : ICON_EYE);
}

static lv_obj_t *editor_build(void *arg)
{
    lv_obj_t *scr = ui_screen();

    /* top row: [x]  icon Title  [check] */
    lv_obj_t *top = lv_obj_create(scr);
    plain(top);
    lv_obj_set_size(top, 272, 44);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *cancel = ui_round_button(top, ICON_XMARK, &ui_icons_16, 40, ui_pal.card, ui_pal.text);
    lv_obj_add_event_cb(cancel, editor_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)false);

    lv_obj_t *mid = lv_obj_create(top);
    plain(mid);
    lv_obj_set_size(mid, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(mid, 8, 0);
    if (s_ed.icon) ui_icon(mid, &ui_icons_16, ui_pal.accent, s_ed.icon);
    lv_obj_t *t = ui_label(mid, &lv_font_montserrat_16, ui_pal.text, s_ed_title);
    lv_obj_set_style_max_width(t, 170, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *ok = ui_round_button(top, ICON_CHECK, &ui_icons_16, 40, ui_pal.accent, lv_color_white());
    lv_obj_add_event_cb(ok, editor_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)true);

    /* text field (+ show/hide for secrets) */
    lv_obj_t *field = lv_obj_create(scr);
    plain(field);
    lv_obj_set_size(field, 352, 52);
    lv_obj_align(field, LV_ALIGN_TOP_MID, 0, 86);
    lv_obj_set_flex_flow(field, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(field, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(field, 8, 0);

    lv_obj_t *ta = lv_textarea_create(field);
    s_ed_ta = ta;
    lv_obj_set_height(ta, 52);
    lv_obj_set_flex_grow(ta, 1);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, s_ed.max_len > 0 ? s_ed.max_len : (int)sizeof(s_ed_text) - 1);
    lv_textarea_set_placeholder_text(ta, s_ed.placeholder ? s_ed.placeholder : "");
    lv_textarea_set_password_mode(ta, s_ed.password);
    lv_textarea_set_text(ta, s_ed_text);
    lv_obj_set_style_radius(ta, 16, 0);
    lv_obj_set_style_bg_color(ta, ui_pal.card, 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_border_color(ta, ui_pal.accent, 0);
    lv_obj_set_style_text_color(ta, ui_pal.text, 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_ver(ta, 14, 0);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);       /* shows the cursor */

    if (s_ed.password) {
        lv_obj_t *eye = ui_round_button(field, ICON_EYE, &ui_icons_16, 44, ui_pal.card, ui_pal.text_dim);
        lv_obj_add_event_cb(eye, editor_eye_cb, LV_EVENT_CLICKED, NULL);
    }

    /* keyboard: sized to stay inside the round display */
    lv_obj_t *kb = lv_keyboard_create(scr);
    lv_obj_set_size(kb, 384, 224);
    lv_obj_align(kb, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(kb, 2, 0);
    lv_obj_set_style_pad_gap(kb, 5, 0);
    lv_obj_set_style_radius(kb, 10, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_18, LV_PART_ITEMS);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, editor_kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, editor_kb_cb, LV_EVENT_CANCEL, NULL);

    return scr;
}

void ui_editor_open(const ui_editor_cfg_t *cfg, bool replace_current)
{
    s_ed = *cfg;
    strlcpy(s_ed_title, cfg->title ? cfg->title : "", sizeof(s_ed_title));
    strlcpy(s_ed_text, cfg->initial ? cfg->initial : "", sizeof(s_ed_text));
    s_ed.title = s_ed_title;
    s_ed.initial = s_ed_text;
    if (replace_current) ui_nav_replace(editor_build, NULL, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
    else ui_nav_push(editor_build, NULL, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}
