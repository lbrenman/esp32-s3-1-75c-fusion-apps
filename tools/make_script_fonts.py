#!/usr/bin/env python3
"""
make_script_fonts.py - Arabic and Devanagari (Hindi) text fonts for LVGL 9, plus the Hindi
shaping table the device uses.

    python3 tools/make_script_fonts.py assets main

Writes, for each size in SIZES:
    main/ui/ui_font_arabic_<size>.c   Arabic letters and the presentation forms LVGL's
                                      Arabic/Persian processing (LV_USE_ARABIC_PERSIAN_CHARS)
                                      turns them into; falls back to ui_font_latin_<size>
    main/ui/ui_font_deva_<size>.c     Devanagari, plus pre-shaped half forms, conjuncts, rakar and
                                      reph in the Private Use Area (U+E000...); falls back to
                                      ui_font_latin_<size>
and main/text_deva_map.h, the table text_to_intl() (main/text_ascii.c) uses to swap letter
sequences for those pre-shaped glyphs. LVGL has no shaping engine for Indic scripts, so the device
does a small part of it itself: half forms, the common conjuncts, rakar, reph, nukta letters and
the reordering of the vowel sign i. It reads well; it isn't as polished as a phone's text engine.

Needs Pillow built with libraqm (for the shaped glyphs; `python3 -c "from PIL import features;
print(features.check('raqm'))"`) and fontTools. Fonts: Noto Sans Arabic / Devanagari Medium
(assets/, SIL Open Font License 1.1).
"""
import sys, os
from PIL import ImageFont, Image, ImageDraw
from fontTools.ttLib import TTFont

SIZES = [16, 20, 28]
PUA_BASE = 0xE000

VIRAMA, RA, NUKTA, ZWJ = 0x094D, 0x0930, 0x093C, 0x200D
CONSONANTS = list(range(0x0915, 0x093A)) + list(range(0x0958, 0x0960))
# conjuncts Hindi uses often that Noto draws as one ligature (or stacked) rather than half form + letter
LIGATURES = ["क्ष", "त्र", "ज्ञ", "श्र", "द्ध", "द्व", "द्य", "द्म", "द्द", "द्ग", "द्भ", "द्र", "त्त", "ह्म", "ह्य",
             "ह्न", "ह्र", "ह्ल", "ह्व", "ट्ट", "ट्ठ", "ड्ड", "ड्ढ", "क्त", "क्क", "ङ्क", "ङ्ग", "श्च", "श्व", "प्त",
             "न्न", "स्त्र", "क्ष्म", "ष्ट", "ष्ठ"]


def arabic_codepoints(ttf, lvgl_dir=None):
    cmap = TTFont(ttf).getBestCmap()
    cps = [c for c in cmap if 0x0600 <= c <= 0x06FF or 0x0750 <= c <= 0x077F
           or 0xFB50 <= c <= 0xFBFF or 0xFE70 <= c <= 0xFEFF]
    return sorted(set(cps))


def shaped(font_raqm, text):
    """(image, bbox, advance) of text shaped by raqm."""
    l, t, r, b = font_raqm.getbbox(text)
    return (l, t, r, b), font_raqm.getlength(text)


def deva_plan(ttf, probe_size=40):
    """Decides which sequences get a pre-shaped glyph. Returns (singles, pua) where pua is a list
    of (codepoint sequence, kind)."""
    cmap = TTFont(ttf).getBestCmap()
    singles = sorted(c for c in cmap if 0x0900 <= c <= 0x097F)
    fr = ImageFont.truetype(ttf, probe_size, layout_engine=ImageFont.Layout.RAQM)
    cons = [c for c in CONSONANTS if c in cmap]

    def w(s):
        return fr.getlength(s)

    pua = []
    for c in cons:
        # half form: shaping "C + virama + ZWJ" draws the half letter (no visible virama)
        half = chr(c) + chr(VIRAMA) + chr(ZWJ)
        full = chr(c) + chr(VIRAMA)
        if fr.getbbox(half) != fr.getbbox(full) and w(half) < w(full) + 0.5:
            pua.append(([c, VIRAMA], "half", half))
    for c in cons:
        pua.append(([c, VIRAMA, RA], "rakar", chr(c) + chr(VIRAMA) + chr(RA)))
    for c in cons:
        pua.append(([RA, VIRAMA, c], "reph", chr(RA) + chr(VIRAMA) + chr(c)))
    for s in LIGATURES:
        seq = [ord(ch) for ch in s]
        if all(ch in cmap for ch in seq):
            pua.append((seq, "lig", s))
    return singles, pua


def glyph_bitmap(font, text):
    l, t, r, b = font.getbbox(text)
    w, h = max(r - l, 0), max(b - t, 0)
    if w == 0 or h == 0:
        return [], 0, 0, 0, 0
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).text((-l, -t), text, font=font, fill=255)
    px = list(img.get_flattened_data()) if hasattr(img, "get_flattened_data") else list(img.getdata())
    return px, w, h, l, b


def emit_font(name, size, entries, ascent, fallback, out_dir, note, metric=None):
    """entries: list of (codepoint, pixels, w, h, left, bottom, advance_px). The line height covers
    the glyphs for which metric(cp) is true (all if None), so a few tall rarities don't space out every line."""
    bitmap, glyphs = [], []
    top, bottom = 0, 0
    for cp, px, w, h, l, b, adv in entries:
        start = len(bitmap)
        if w and h:
            nib = [p >> 4 for p in px]
            if len(nib) % 2:
                nib.append(0)
            for i in range(0, len(nib), 2):
                bitmap.append((nib[i] << 4) | nib[i + 1])
            if metric is None or metric(cp):
                top, bottom = max(top, ascent - (b - h)), min(bottom, ascent - b)
        glyphs.append((start, min(int(round(adv * 16)), 4095), w, h, l if w else 0, (ascent - b) if w else 0))
    cps = [e[0] for e in entries]
    L = []
    a = L.append
    a("/* %s: %d px, 4 bpp. %s" % (name, size, note))
    a(" * Generated by tools/make_script_fonts.py. Do not edit. */")
    a('#include "lvgl.h"\n')
    a("LV_FONT_DECLARE(%s)\n" % fallback)
    a("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
    for i in range(0, len(bitmap), 16):
        a("    " + ", ".join("0x%02x" % v for v in bitmap[i:i + 16]) + ",")
    a("};\n")
    a("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    a("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,")
    for (bi, adv, w, h, ox, oy), cp in zip(glyphs, cps):
        a("    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d}, /* U+%04X */"
          % (bi, adv, w, h, ox, oy, cp))
    a("};\n")
    # one sparse cmap per 0x1000 block (offsets in a sparse list must fit in 16 bits and stay small)
    blocks = []
    for i, cp in enumerate(cps):
        if not blocks or cp - blocks[-1][0] > 0xFFF:
            blocks.append([cp, []])
        blocks[-1][1].append((cp, i + 1))
    for k, (first, items) in enumerate(blocks):
        a("static const uint16_t unicode_list_%d[] = {" % k)
        offs = ["0x%x" % (cp - first) for cp, _ in items]
        for i in range(0, len(offs), 12):
            a("    " + ", ".join(offs[i:i + 12]) + ",")
        a("};\n")
    a("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    for k, (first, items) in enumerate(blocks):
        a("    {")
        a("        .range_start = %d, .range_length = %d, .glyph_id_start = %d," %
          (first, items[-1][0] - first + 1, items[0][1]))
        a("        .unicode_list = unicode_list_%d, .glyph_id_ofs_list = NULL, .list_length = %d, "
          ".type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY" % (k, len(items)))
        a("    },")
    a("};\n")
    a("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    a("    .glyph_bitmap = glyph_bitmap,")
    a("    .glyph_dsc = glyph_dsc,")
    a("    .cmaps = cmaps,")
    a("    .kern_dsc = NULL,")
    a("    .kern_scale = 0,")
    a("    .cmap_num = %d," % len(blocks))
    a("    .bpp = 4,")
    a("    .kern_classes = 0,")
    a("    .bitmap_format = 0,")
    a("};\n")
    a("const lv_font_t %s = {" % name)
    a("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    a("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    a("    .line_height = %d," % (top - bottom))
    a("    .base_line = %d," % (-bottom))
    a("    .subpx = LV_FONT_SUBPX_NONE,")
    a("    .underline_position = -1,")
    a("    .underline_thickness = 1,")
    a("    .dsc = &font_dsc,")
    a("    .fallback = &%s,   /* Latin letters, digits, punctuation */" % fallback)
    a("};")
    with open(os.path.join(out_dir, name + ".c"), "w") as f:
        f.write("\n".join(L) + "\n")


def build_arabic(ttf, size, out_dir, cps):
    font = ImageFont.truetype(ttf, size, layout_engine=ImageFont.Layout.BASIC)
    ascent, _ = font.getmetrics()
    entries = []
    for cp in cps:
        px, w, h, l, b = glyph_bitmap(font, chr(cp))
        entries.append((cp, px, w, h, l, b, font.getlength(chr(cp))))
    emit_font("ui_font_arabic_%d" % size, size, entries, ascent, "ui_font_latin_%d" % size, out_dir,
              "Arabic (Noto Sans Arabic Medium, SIL OFL 1.1).\n * Needs LV_USE_ARABIC_PERSIAN_CHARS and LV_USE_BIDI (letters join and run right to left).",
              metric=lambda cp: 0x0621 <= cp <= 0x064A or 0xFE70 <= cp <= 0xFEFC)


def build_deva(ttf, size, out_dir, singles, pua):
    basic = ImageFont.truetype(ttf, size, layout_engine=ImageFont.Layout.BASIC)
    raqm = ImageFont.truetype(ttf, size, layout_engine=ImageFont.Layout.RAQM)
    ascent, _ = basic.getmetrics()
    entries = []
    for cp in singles:
        px, w, h, l, b = glyph_bitmap(basic, chr(cp))
        entries.append((cp, px, w, h, l, b, basic.getlength(chr(cp))))
    for k, (seq, kind, text) in enumerate(pua):
        px, w, h, l, b = glyph_bitmap(raqm, text)
        entries.append((PUA_BASE + k, px, w, h, l, b, raqm.getlength(text)))
    emit_font("ui_font_deva_%d" % size, size, entries, ascent, "ui_font_latin_%d" % size, out_dir,
              "Devanagari (Noto Sans Devanagari Medium, SIL OFL 1.1),\n * with pre-shaped conjuncts at U+E000... (see main/text_deva_map.h).")


def write_map(path, pua):
    L = ["#pragma once",
         "/* Devanagari sequences drawn as one pre-shaped glyph of ui_font_deva_* (U+E000 + index).",
         " * Generated by tools/make_script_fonts.py with the fonts; used by text_to_intl(). Do not edit. */",
         "#include <stdint.h>", "",
         "#define DEVA_PUA_BASE 0xE000",
         "#define DEVA_PUA_COUNT %d" % len(pua), "",
         "typedef struct { uint16_t seq[5]; uint8_t len; uint8_t kind; } deva_seq_t;   /* kind: 0 half, 1 rakar, 2 reph, 3 ligature */",
         "", "/* index = PUA code point - DEVA_PUA_BASE; longer sequences are tried first by the code */",
         "static const deva_seq_t s_deva_seq[DEVA_PUA_COUNT] = {"]
    kinds = {"half": 0, "rakar": 1, "reph": 2, "lig": 3}
    for k, (seq, kind, text) in enumerate(pua):
        L.append("    { {%s}, %d, %d },   /* U+%04X %s %s */" % (", ".join("0x%04X" % c for c in seq), len(seq),
                                                               kinds[kind], PUA_BASE + k, kind, text))
    L.append("};")
    with open(path, "w") as f:
        f.write("\n".join(L) + "\n")


def main():
    assets, main_dir = sys.argv[1], sys.argv[2]
    ar = os.path.join(assets, "NotoSansArabic-Medium.ttf")
    dv = os.path.join(assets, "NotoSansDevanagari-Medium.ttf")
    out_dir = os.path.join(main_dir, "ui")
    ar_cps = arabic_codepoints(ar)
    singles, pua = deva_plan(dv)
    for s in SIZES:
        build_arabic(ar, s, out_dir, ar_cps)
        build_deva(dv, s, out_dir, singles, pua)
    write_map(os.path.join(main_dir, "text_deva_map.h"), pua)
    h = ["#pragma once", "/* Arabic and Devanagari text fonts, generated by tools/make_script_fonts.py. */",
         '#include "lvgl.h"', ""]
    h += ["LV_FONT_DECLARE(ui_font_%s_%d)" % (f, s) for f in ("arabic", "deva") for s in SIZES]
    with open(os.path.join(out_dir, "ui_fonts_intl.h"), "w") as f:
        f.write("\n".join(h) + "\n")
    print("Arabic: %d glyphs; Devanagari: %d letters + %d pre-shaped (%s); sizes %s" % (
        len(ar_cps), len(singles), len(pua),
        ", ".join("%d %s" % (sum(1 for p in pua if p[1] == k), k) for k in ("half", "rakar", "reph", "lig")),
        SIZES))


if __name__ == "__main__":
    main()
