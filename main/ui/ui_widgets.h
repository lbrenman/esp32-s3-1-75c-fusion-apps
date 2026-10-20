#pragma once
/* Shared building blocks for the round 466x466 screen: list screens with a header,
 * iOS-style rows, a text editor with keyboard, toasts and confirm dialogs.
 * All functions run in the LVGL task. */

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"
#include "ui_theme.h"
#include "ui_icons.h"

#define UI_SCREEN_SIZE 466
#define UI_LIST_WIDTH  340

lv_obj_t *ui_screen(void);
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);
lv_obj_t *ui_icon(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *icon);

/* Round button with an icon. */
lv_obj_t *ui_round_button(lv_obj_t *parent, const char *icon, const lv_font_t *icon_font,
                          int size, lv_color_t bg, lv_color_t fg);

/* Gradient circle with a big icon (Home tiles, voice-chat orb). */
lv_obj_t *ui_orb(lv_obj_t *parent, int size, uint32_t c1, uint32_t c2, const char *icon,
                 const lv_font_t *icon_font);

/* A list screen: scrolling column + header with title and optional back button.
 * Returns the list to add sections/rows to. Back pops one screen. */
lv_obj_t *ui_list_screen(lv_obj_t **out_screen, const char *title, bool back);

/* Set as a screen's user data to turn off the default swipe-right-to-go-back. */
#define UI_CUSTOM_GESTURES ((void *)1)

lv_obj_t *ui_section(lv_obj_t *list, const char *text);

typedef struct {
    lv_obj_t *row;
    lv_obj_t *title;
    lv_obj_t *value;      /* NULL when created without a value */
} ui_row_t;

/* Tappable (on_click != NULL) or informational row. */
ui_row_t  ui_row(lv_obj_t *list, const char *icon, uint32_t icon_color, const char *title,
                 const char *value, lv_event_cb_t on_click, void *user);
/* Row with a switch on the right. on_change gets LV_EVENT_VALUE_CHANGED from the switch. */
lv_obj_t *ui_row_switch(lv_obj_t *list, const char *icon, uint32_t icon_color, const char *title,
                        bool on, lv_event_cb_t on_change, void *user);
/* Row with a slider underneath. cb gets VALUE_CHANGED (while dragging) and RELEASED. */
lv_obj_t *ui_row_slider(lv_obj_t *list, const char *icon, uint32_t icon_color, const char *title,
                        int min, int max, int value, lv_event_cb_t cb, void *user);

/* Small dim hint at the bottom of a screen, e.g. "Swipe up for settings". */
lv_obj_t *ui_hint(lv_obj_t *scr, const char *icon, const char *text);

void ui_toast(const char *text);
void ui_confirm(const char *title, const char *msg, const char *ok_text, bool danger,
                void (*on_ok)(void *user), void *user);

/* "••••••••" or "••••abcd" (last 4 shown) for secrets on screen. */
void ui_mask_secret(const char *in, char *out, size_t out_len, bool show_tail);

/* Full-screen text editor with keyboard. */
typedef struct {
    const char *title;
    const char *icon;
    const char *initial;          /* copied */
    const char *placeholder;
    int         max_len;
    bool        password;         /* hidden text with a show/hide button */
    /* Called with the new text. Return true if the callback navigated itself
     * (e.g. opened the next editor); otherwise the editor closes pop_after screens. */
    bool      (*on_save)(const char *text, void *user);
    void       *user;
    int         pop_after;        /* 0 = 1 */
} ui_editor_cfg_t;

void ui_editor_open(const ui_editor_cfg_t *cfg, bool replace_current);
