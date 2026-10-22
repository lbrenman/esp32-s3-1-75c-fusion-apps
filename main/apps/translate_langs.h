#pragma once
/* Languages the Translator offers (ISO 639-1 codes), matching what the Fusion translator flow
 * supports with Mistral:
 *   "From" (sourceLanguage): what Voxtral Mini Transcribe V2 can transcribe (13 languages)
 *   "Into" (targetLanguage): what Voxtral TTS can speak (9 languages)
 * Chinese, Japanese, Korean and Russian can be spoken to it but not translated into.
 * Pure C: used by the Translator screens and the voice engine. */

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *code;        /* "en" */
    const char *name;        /* "English" */
    const char *native;      /* "Español", "العربية": only for scripts the screen can draw, else NULL */
    bool        source;      /* can be spoken (sourceLanguage) */
    bool        target;      /* can be translated into (targetLanguage) */
    bool        drawable;    /* the screen can show its text (not Chinese, Japanese, Korean) */
} tr_lang_t;

extern const tr_lang_t g_tr_langs[];
extern const int       g_tr_lang_count;

const tr_lang_t *tr_lang_find(const char *code);       /* NULL if unknown */
const char      *tr_lang_name(const char *code);       /* "English", or the code itself if unknown */
bool             tr_lang_is_source(const char *code);
bool             tr_lang_is_target(const char *code);

/* The pair is usable: both known, source can be spoken, target can be spoken back, and different. */
bool             tr_pair_valid(const char *src, const char *dst);
/* Swapping From and Into gives a usable pair (false when From is e.g. Chinese). */
bool             tr_can_swap(const char *src, const char *dst);

/* Picks code as the new From (pick_source) or Into language and fixes the other side so the pair
 * stays valid: picking the language that's already on the other side swaps them, and if that
 * can't work (e.g. Chinese can't be an Into language) the other side moves to English, or to
 * Spanish when English is taken. src and dst are SET_LANG_MAX + 1 byte buffers. */
void             tr_pick(char *src, char *dst, size_t len, const char *code, bool pick_source);
