#include <string.h>
#include "translate_langs.h"

/* Alphabetical by English name, as in the web app. Native names are UTF-8 for the ui_font_* fonts,
 * already in display order: Hindi's i sign comes before its consonant ("हिंदी" is stored as
 * ि ह ं द ी), and LVGL joins Arabic letters itself. NULL for scripts the screen can't draw. */
const tr_lang_t g_tr_langs[] = {
    { "ar", "Arabic",     "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9", true, true, true },   /* العربية */
    { "zh", "Chinese",    NULL,                   true, false, false },
    { "nl", "Dutch",      "Nederlands",           true, true,  true  },
    { "en", "English",    "English",              true, true,  true  },
    { "fr", "French",     "Fran\xC3\xA7" "ais",   true, true,  true  },
    { "de", "German",     "Deutsch",              true, true,  true  },
    { "hi", "Hindi",      "\xE0\xA4\xBF\xE0\xA4\xB9\xE0\xA4\x82\xE0\xA4\xA6\xE0\xA5\x80", true, true, true },   /* हिंदी */
    { "it", "Italian",    "Italiano",             true, true,  true  },
    { "ja", "Japanese",   NULL,                   true, false, false },
    { "ko", "Korean",     NULL,                   true, false, false },
    { "pt", "Portuguese", "Portugu\xC3\xAAs",     true, true,  true  },
    { "ru", "Russian",    "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9", true, false, true },   /* Русский */
    { "es", "Spanish",    "Espa\xC3\xB1ol",       true, true,  true  },
};
const int g_tr_lang_count = sizeof(g_tr_langs) / sizeof(g_tr_langs[0]);

const tr_lang_t *tr_lang_find(const char *code)
{
    if (!code) return NULL;
    for (int i = 0; i < g_tr_lang_count; i++) {
        if (strcmp(g_tr_langs[i].code, code) == 0) return &g_tr_langs[i];
    }
    return NULL;
}

const char *tr_lang_name(const char *code)
{
    const tr_lang_t *l = tr_lang_find(code);
    return l ? l->name : (code ? code : "");
}

bool tr_lang_is_source(const char *code) { const tr_lang_t *l = tr_lang_find(code); return l && l->source; }
bool tr_lang_is_target(const char *code) { const tr_lang_t *l = tr_lang_find(code); return l && l->target; }

bool tr_pair_valid(const char *src, const char *dst)
{
    return tr_lang_is_source(src) && tr_lang_is_target(dst) && strcmp(src, dst) != 0;
}

bool tr_can_swap(const char *src, const char *dst)
{
    return tr_pair_valid(dst, src);
}

static void copy(char *d, size_t len, const char *s)
{
    size_t n = strlen(s);
    if (n >= len) n = len - 1;
    memcpy(d, s, n);
    d[n] = '\0';
}

void tr_pick(char *src, char *dst, size_t len, const char *code, bool pick_source)
{
    char old_src[16], old_dst[16];
    copy(old_src, sizeof(old_src), src);
    copy(old_dst, sizeof(old_dst), dst);

    if (pick_source) {
        copy(src, len, code);
        if (strcmp(code, old_dst) == 0) {
            /* picked the Into language as From: swap, if the old From can be an Into language */
            if (tr_lang_is_target(old_src) && strcmp(old_src, code) != 0) copy(dst, len, old_src);
            else copy(dst, len, strcmp(code, "en") ? "en" : "es");
        }
        if (!tr_lang_is_target(dst)) copy(dst, len, strcmp(code, "en") ? "en" : "es");
    } else {
        copy(dst, len, code);
        if (strcmp(code, old_src) == 0) {
            /* picked the From language as Into: swap (every Into language can be a From language) */
            copy(src, len, strcmp(old_dst, code) ? old_dst : (strcmp(code, "en") ? "en" : "es"));
        }
        if (!tr_lang_is_source(src)) copy(src, len, strcmp(code, "en") ? "en" : "es");
    }
}
