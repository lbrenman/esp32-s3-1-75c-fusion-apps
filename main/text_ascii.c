/* Converts UTF-8 text to what the built-in Montserrat fonts can draw. See text_ascii.h.
 * (Moved here from voice_core.c so Wi-Fi names and other screens can use it too.) */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "text_ascii.h"
#include "text_deva_map.h"

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

/* What ui_font_latin_* can draw beyond ASCII (keep in step with tools/make_text_font.py) */
static int latin_drawable(uint32_t cp)
{
    if (cp >= 0x00A0 && cp <= 0x017F) return 1;
    switch (cp) {
    case 0x0218: case 0x0219: case 0x021A: case 0x021B:
    case 0x2013: case 0x2014: case 0x2018: case 0x2019: case 0x201A: case 0x201C: case 0x201D:
    case 0x201E: case 0x2022: case 0x2026: case 0x2039: case 0x203A: case 0x20AC:
        return 1;
    default:
        return 0;
    }
}

/* Decodes one UTF-8 character; returns its byte length (invalid bytes come back as '?', 1 byte). */
static int utf8_next(const unsigned char *p, uint32_t *cp)
{
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu); return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu); return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *cp = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
        return 4;
    }
    *cp = '?';
    return 1;
}

/* ------------------------------------------------------------ text_to_intl
 * Arabic: kept as is. LVGL joins the letters (LV_USE_ARABIC_PERSIAN_CHARS) and lays them out right
 * to left (LV_USE_BIDI) when the label is drawn with ui_font_arabic_*.
 * Devanagari: LVGL has no Indic shaping, so the parts Hindi needs most are done here, by swapping
 * letter sequences for pre-shaped glyphs of ui_font_deva_* (text_deva_map.h, U+E000...):
 *   C + virama + C   half form of the first consonant (क्य -> half क + य)
 *   C + virama + ra  rakar (प्र), ra + virama + C  reph (र्म), and common conjuncts (क्ष त्र ज्ञ श्र ...)
 *   C + nukta        the precomposed letter (ड + ़ -> ड़)
 *   C + vowel sign i the i sign (ि) moved in front of the consonant cluster, where it's drawn */

#define DEVA_VIRAMA 0x094D
#define DEVA_NUKTA  0x093C
#define DEVA_I      0x093F
#define ZWNJ        0x200C
#define ZWJ_CP      0x200D
#define INTL_MAX_CPS 640

static int is_deva_consonant(uint32_t c)
{
    return (c >= 0x0915 && c <= 0x0939) || (c >= 0x0958 && c <= 0x095F);
}

static int is_arabic(uint32_t c)
{
    return (c >= 0x0600 && c <= 0x06FF) || (c >= 0x0750 && c <= 0x077F) ||
           (c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}

static int is_cyrillic(uint32_t c) { return c >= 0x0400 && c <= 0x045F; }

static uint32_t nukta_form(uint32_t c)
{
    switch (c) {
    case 0x0915: return 0x0958; case 0x0916: return 0x0959; case 0x0917: return 0x095A;
    case 0x091C: return 0x095B; case 0x0921: return 0x095C; case 0x0922: return 0x095D;
    case 0x092B: return 0x095E; case 0x092F: return 0x095F;
    default: return 0;
    }
}

/* The longest pre-shaped sequence starting at cps[i] (index into s_deva_seq), or -1 */
static int deva_match(const uint32_t *cps, size_t n, size_t i)
{
    int best = -1, best_len = 0;
    for (int k = 0; k < DEVA_PUA_COUNT; k++) {
        const deva_seq_t *d = &s_deva_seq[k];
        if (d->len <= best_len || i + d->len > n) continue;
        int ok = 1;
        for (int j = 0; j < d->len && ok; j++) ok = (cps[i + j] == d->seq[j]);
        if (!ok) continue;
        /* a half form only before another consonant (or ZWJ, which asks for it) */
        if (d->kind == 0 && !(i + 2 < n && (is_deva_consonant(cps[i + 2]) || cps[i + 2] == ZWJ_CP))) continue;
        best = k;
        best_len = d->len;
    }
    return best;
}

static size_t put_utf8(char *out, size_t o, size_t out_len, uint32_t cp)
{
    char b[4];
    int n;
    if (cp < 0x80)        { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800)  { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    if (o + n >= out_len) return o;          /* never cut a character in half */
    for (int i = 0; i < n; i++) out[o++] = b[i];
    return o;
}

size_t text_to_intl(const char *in, char *out, size_t out_len)
{
    static uint32_t cps[INTL_MAX_CPS], shaped[INTL_MAX_CPS];   /* callers: one task at a time */
    size_t n = 0, missing = 0;

    /* 1. decode, folding consonant + nukta into the precomposed letter */
    for (const unsigned char *p = (const unsigned char *)in; *p && n < INTL_MAX_CPS;) {
        uint32_t cp;
        p += utf8_next(p, &cp);
        if (cp == DEVA_NUKTA && n > 0 && nukta_form(cps[n - 1])) { cps[n - 1] = nukta_form(cps[n - 1]); continue; }
        cps[n++] = cp;
    }

    /* 2. Devanagari: pre-shaped glyphs and the i sign */
    size_t m = 0, syl = 0;
    int prev_half = 0, have_base = 0;
    for (size_t i = 0; i < n && m < INTL_MAX_CPS;) {
        uint32_t c = cps[i];
        int k = (c >= 0x0900 && c <= 0x097F) ? deva_match(cps, n, i) : -1;
        if (k >= 0) {
            if (!prev_half) syl = m;               /* a new syllable starts here */
            shaped[m++] = DEVA_PUA_BASE + k;
            i += s_deva_seq[k].len;
            prev_half = (s_deva_seq[k].kind == 0);
            have_base = !prev_half;
            if (prev_half && i < n && cps[i] == ZWJ_CP) i++;
            continue;
        }
        if (is_deva_consonant(c)) {
            if (!prev_half) syl = m;
            shaped[m++] = c;
            prev_half = 0;
            have_base = 1;
            i++;
            continue;
        }
        if (c == DEVA_I && have_base) {            /* drawn before the cluster it follows */
            memmove(&shaped[syl + 1], &shaped[syl], (m - syl) * sizeof(uint32_t));
            shaped[syl] = c;
            m++;
            have_base = 0;
            prev_half = 0;
            i++;
            continue;
        }
        if (c == ZWJ_CP || c == ZWNJ) { i++; continue; }   /* joiners: nothing to draw */
        /* vowel signs and other marks stay with the cluster; anything else ends it */
        if (!(c >= 0x0900 && c <= 0x0903) && !(c >= 0x093A && c <= 0x094F) && !(c >= 0x0955 && c <= 0x0957)) {
            have_base = 0;
        } else if (c != DEVA_VIRAMA) {
            have_base = have_base && (c != DEVA_I);
        }
        prev_half = 0;
        shaped[m++] = c;
        i++;
    }

    /* 3. encode what the fonts can draw; look-alikes or '?' for the rest */
    size_t o = 0;
    for (size_t i = 0; i < m && o + 1 < out_len; i++) {
        uint32_t cp = shaped[i];
        if (cp < 0x80 || latin_drawable(cp) || is_cyrillic(cp) || is_arabic(cp) ||
            (cp >= 0x0900 && cp <= 0x097F) || (cp >= DEVA_PUA_BASE && cp < DEVA_PUA_BASE + DEVA_PUA_COUNT)) {
            size_t before = o;
            o = put_utf8(out, o, out_len, cp);
            if (o == before) break;
        } else {
            const char *r = ascii_for(cp);
            if (r[0] == '?' && r[1] == '\0') missing++;   /* a script these fonts don't have */
            for (; *r && o + 1 < out_len; r++) out[o++] = *r;
        }
    }
    out[o] = '\0';
    return missing;
}

int text_script(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        uint32_t cp;
        p += utf8_next(p, &cp);
        if ((cp >= 0x0900 && cp <= 0x097F) || (cp >= DEVA_PUA_BASE && cp < DEVA_PUA_BASE + DEVA_PUA_COUNT)) {
            return TEXT_SCRIPT_DEVANAGARI;
        }
        if (is_arabic(cp)) return TEXT_SCRIPT_ARABIC;
    }
    return TEXT_SCRIPT_LATIN;
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
