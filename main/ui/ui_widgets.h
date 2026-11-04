#pragma once
/* Shared building blocks for the screen (round 466x466 on the 1.75C, 410x502 with rounded corners
 * on the 2.06; see board.h): list screens with a header,
 * iOS-style rows, a text editor with keyboard, toasts and confirm dialogs.
 * All functions run in the LVGL task. */

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"
#include "ui_theme.h"
#include "ui_icons.h"

#include "board.h"

#define UI_W           BOARD_SCREEN_W
#define UI_H           BOARD_SCREEN_H
#define UI_ROUND       BOARD_SCREEN_ROUND          /* 1: a circle, 0: a rectangle with rounded corners */
#define UI_LIST_WIDTH  (UI_ROUND ? 340 : UI_W - 40) /* rows and cards */
#define UI_EXTRA_H     (UI_H - 466)                 /* extra height over the 1.75C's 466 (36 on the 2.06) */

/* Sizes that differ by screen shape: UI_SZ(round, rect) and UI_FONT(round px, rect px).
 * The round 1.75C keeps its original sizes; the 2.06's rectangle has room for bigger text and
 * more rows (a circle is only full width across the middle). */
#define UI_SZ(round, rect)    (UI_ROUND ? (round) : (rect))
#define UI_FONT(round, rect)  (UI_ROUND ? &lv_font_montserrat_##round : &lv_font_montserrat_##rect)

#define UI_F_TITLE     UI_FONT(24, 28)       /* list screen header */
#define UI_F_ROW       UI_FONT(16, 20)       /* row title */
#define UI_F_VALUE     UI_FONT(14, 16)       /* row value / status line */
#define UI_F_SECTION   UI_FONT(14, 16)       /* section caption */
#define UI_F_BODY      UI_FONT(16, 18)       /* toasts, dialog buttons */
#define UI_F_HINT      UI_FONT(14, 16)       /* gesture hints, small notes */
#define UI_ROW_H       UI_SZ(62, 74)         /* list row minimum height */
#define UI_ROW_ICON    UI_SZ(36, 44)         /* colored icon square in a row */
#define UI_HEADER_Y    UI_SZ(34, 22)         /* list screen header from the top */
#define UI_LIST_TOP    UI_SZ(96, 82)         /* list content starts here */
#define UI_LIST_BOTTOM UI_SZ(130, 70)        /* room left under the last row */

lv_obj_t *ui_screen(void);
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);
lv_obj_t *ui_icon(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *icon);

/* Round button with an icon. */
lv_obj_t *ui_round_button(lv_obj_t *parent, const char *icon, const lv_font_t *icon_font,
                          int size, lv_color_t bg, lv_color_t fg);

/* The colored status ring around the edge of the screen (Voice chat, Translator): a circle on
 * a round screen, a rounded rectangle that follows the corners on the 2.06. */
lv_obj_t *ui_status_ring(lv_obj_t *parent, int border);

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
/* Row with a title, a status line underneath and a switch on the right. */
typedef struct {
    lv_obj_t *row;
    lv_obj_t *value;
    lv_obj_t *sw;
} ui_toggle_row_t;
ui_toggle_row_t ui_row_toggle(lv_obj_t *list, const char *icon, uint32_t icon_color, const char *title,
                              const char *value, bool on, lv_event_cb_t on_change, void *user);
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
