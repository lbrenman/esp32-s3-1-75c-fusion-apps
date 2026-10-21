/* Converts UTF-8 text to what the built-in Montserrat fonts can draw. See text_ascii.h.
 * (Moved here from voice_core.c so Wi-Fi names and other screens can use it too.) */

#include <stdint.h>
#include <stddef.h>
#include "text_ascii.h"

/* Look-alikes for U+00C0..U+00FF (accented letters) */
static const char *const s_latin1_hi[64] = {   /* U+00C0 .. U+00FF */
    "A","A","A","A","A","A","AE","C","E","E","E","E","I","I","I","I",
    "D","N","O","O","O","O","O","x","O","U","U","U","U","Y","Th","ss",
    "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
    "d","n","o","o","o","o","o","/","o","u","u","u","u","y","th","y",
};

static const char *ascii_for(uint32_t cp)
{
    if (cp >= 0xC0 && cp <= 0xFF) return s_latin1_hi[cp - 0xC0];
    switch (cp) {
    case 0x00A0: case 0x2002: case 0x2003: case 0x2007: case 0x2009: case 0x200A: case 0x202F:
        return " ";
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: case 0x00B4: case 0x0060:
        return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: case 0x00AB: case 0x00BB:
        return "\"";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
        return "-";
    case 0x2026: return "...";
    case 0x2022: case 0x00B7: case 0x25CF: return "-";
    case 0x00B0: return " deg";
    case 0x00A9: return "(c)";
    case 0x00AE: return "(R)";
    case 0x2122: return "(TM)";
    case 0x00B1: return "+/-";
    case 0x00A3: return "GBP";
    case 0x20AC: return "EUR";
    case 0x00A5: return "JPY";
    case 0x00BF: return "?";
    case 0x00A1: return "!";
    case 0x2192: return "->";
    case 0x2190: return "<-";
    case 0x200B: case 0x200C: case 0x200D: case 0xFE0F: case 0xFEFF: return "";   /* invisible */
    default: break;
    }
    if (cp >= 0x1F000) return "";   /* emoji and pictographs */
    return "?";
}

void text_to_ascii(const char *in, char *out, size_t out_len)
{
    const unsigned char *p = (const unsigned char *)in;
    size_t o = 0;
    while (*p && o + 1 < out_len) {
        uint32_t cp;
        int n;
        if (p[0] < 0x80)                                  { cp = p[0]; n = 1; }
        else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu); n = 2;
        } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu); n = 3;
        } else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
                   (p[3] & 0xC0) == 0x80) {
            cp = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
            n = 4;
        } else { cp = '?'; n = 1; }                       /* invalid byte */
        p += n;

        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else if (cp == 0x2014 || cp == 0x2015) {
            /* em dash: "word—word" reads better as "word - word" (no doubled spaces) */
            if (o > 0 && out[o - 1] != ' ' && o + 1 < out_len) out[o++] = ' ';
            if (o + 1 < out_len) out[o++] = '-';
            if (*p && *p != ' ' && o + 1 < out_len) out[o++] = ' ';
        } else {
            for (const char *r = ascii_for(cp); *r && o + 1 < out_len; r++) out[o++] = *r;
        }
    }
    out[o] = '\0';
}
