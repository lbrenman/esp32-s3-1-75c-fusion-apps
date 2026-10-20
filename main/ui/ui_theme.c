#include "ui_theme.h"

ui_pal_t ui_pal;

static lv_display_t *s_disp;
static bool          s_dark;

static void load_palette(bool dark)
{
    if (dark) {
        ui_pal.bg         = lv_color_hex(0x000000);
        ui_pal.card       = lv_color_hex(0x16171B);
        ui_pal.card_press = lv_color_hex(0x26282E);
        ui_pal.text       = lv_color_hex(0xF4F5F7);
        ui_pal.text_dim   = lv_color_hex(0x8D929B);
        ui_pal.line       = lv_color_hex(0x2A2C33);
        ui_pal.accent     = lv_color_hex(0x8B74FF);
        ui_pal.ok         = lv_color_hex(0x30D158);
        ui_pal.warn       = lv_color_hex(0xFFB020);
        ui_pal.err        = lv_color_hex(0xFF5A4F);
    } else {
        ui_pal.bg         = lv_color_hex(0xEEF0F5);
        ui_pal.card       = lv_color_hex(0xFFFFFF);
        ui_pal.card_press = lv_color_hex(0xE2E5EC);
        ui_pal.text       = lv_color_hex(0x15161A);
        ui_pal.text_dim   = lv_color_hex(0x6B7079);
        ui_pal.line       = lv_color_hex(0xDCDFE6);
        ui_pal.accent     = lv_color_hex(0x5B3DF5);
        ui_pal.ok         = lv_color_hex(0x1E9E4A);
        ui_pal.warn       = lv_color_hex(0xD98200);
        ui_pal.err        = lv_color_hex(0xE0362C);
    }
}

static void apply_lvgl_theme(void)
{
    /* LVGL's default theme styles the stock widgets (switch, slider, keyboard, text area) */
    lv_theme_t *th = lv_theme_default_init(s_disp, ui_pal.accent, lv_color_hex(UI_C_VOICE_2),
                                           s_dark, &lv_font_montserrat_16);
    lv_display_set_theme(s_disp, th);
}

void ui_theme_init(lv_display_t *disp, bool dark)
{
    s_disp = disp ? disp : lv_display_get_default();
    s_dark = dark;
    load_palette(dark);
    apply_lvgl_theme();
}

void ui_theme_set_dark(bool dark)
{
    if (dark == s_dark) return;
    s_dark = dark;
    load_palette(dark);
    apply_lvgl_theme();
}

bool ui_theme_is_dark(void) { return s_dark; }
