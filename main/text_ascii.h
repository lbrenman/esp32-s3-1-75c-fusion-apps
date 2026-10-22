#pragma once
/* The built-in Montserrat fonts only contain ASCII (plus a few symbols), so "smart" punctuation
 * and accented letters show up as empty boxes. text_to_ascii() converts UTF-8 text to ASCII
 * look-alikes for display: ’ -> ', “ ” -> ", – — -> -, … -> ..., é -> e. Emoji are dropped;
 * anything else unknown becomes '?'. Use it only for what is shown on screen: keep the original
 * bytes for anything sent anywhere (a Wi-Fi name, for example, must stay exactly as it is).
 * Pure C: safe from any task. */

#include <stddef.h>

void text_to_ascii(const char *in, char *out, size_t out_len);

/* For the Translator: keeps what the ui_font_latin_* / ui_font_arabic_* / ui_font_deva_* fonts
 * (ui_fonts.h) can draw: Latin with accents (á é ñ ü ç ß œ ł ...), Cyrillic, Arabic and Devanagari,
 * plus curly quotes, dashes, "…" and "€". Devanagari is pre-shaped for ui_font_deva_* (half forms,
 * conjuncts, reph/rakar, the i sign); Arabic is left to LVGL (letter joining + right to left).
 * Anything else gets an ASCII look-alike or '?'. Returns how many letters couldn't be shown
 * (Chinese, Japanese, Korean...), so the caller can say so instead of showing question marks.
 * Not reentrant (static work buffers): call it from one task. */
size_t text_to_intl(const char *in, char *out, size_t out_len);

/* Which font family text needs: the first Devanagari or Arabic letter decides. */
enum { TEXT_SCRIPT_LATIN, TEXT_SCRIPT_ARABIC, TEXT_SCRIPT_DEVANAGARI };
int text_script(const char *text);
