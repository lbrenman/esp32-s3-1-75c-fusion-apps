#pragma once
/* Voice engine: recording, wake word, WebSocket, reply playback (no LVGL in here).
 * Shared by Voice chat and Translator: one microphone, one speaker, one connection, and only one
 * of the two apps is open at a time. The app sets the mode before it activates the engine.
 * The screens poll voice_state() / voice_text_*() from an LVGL timer,
 * so this code never touches LVGL objects from its own tasks. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    VOICE_OFF,          /* app not open: WebSocket closed */
    VOICE_WAIT_WIFI,
    VOICE_CONNECTING,
    VOICE_READY,
    VOICE_LISTENING,
    VOICE_THINKING,
    VOICE_SPEAKING,
    VOICE_ERROR,        /* e.g. no server URL set; details in the note text */
    VOICE_DISCONNECTED, /* app open, connection switched off (or closed after idle) */
} voice_state_t;

typedef enum {
    VOICE_MODE_CHAT,       /* Voice chat: ws_url + conversationId, TRANSCRIBED_REQUEST / RESPONSE, wake word */
    VOICE_MODE_TRANSLATE,  /* Translator: tr_url/translate + language pair, TRANSLATION events, hold to talk only */
} voice_mode_t;

esp_err_t     voice_init(void);              /* once at boot: codec, wake word model, tasks */
void          voice_set_mode(voice_mode_t mode);   /* call before voice_set_active(true) */
voice_mode_t  voice_mode(void);
void          voice_set_active(bool active); /* app opened / closed: connect / disconnect */
void          voice_reconnect(void);         /* server URL or API key changed (also switches the connection on) */
void          voice_set_connected(bool on);  /* the Connection switch; on each time the app opens */
void          voice_new_conversation(void);  /* new saved conversation ID; reconnects if connected */
/* Translator languages changed (saved by the caller): the pair is part of the connection URL, so
 * reconnect, but only once no turn is in flight (a translation still arriving isn't cut off).
 * Does nothing while the connection is switched off: the next connect uses the new pair. */
void          voice_reconnect_when_idle(void);
/* Translator: the endpoint for a base URL (adds /translate, never twice; /speech becomes /translate). */
void          voice_tr_endpoint(const char *base, char *out, size_t out_len);
bool          voice_wants_connection(void);
void          voice_set_volume(int percent);

/* push-to-talk (call from the LVGL task). With the wake word on, a quick tap listens hands-free. */
void          voice_ptt_press(void);
void          voice_ptt_release(void);
void          voice_ptt_cancel(void);        /* the touch was a swipe: discard the recording */

/* Wake word. The engine listens only while the Voice chat screen itself is showing (not its
 * server settings screen), the connection is wanted and the setting is on. */
typedef enum {
    WAKE_OFF,           /* switched off in Settings */
    WAKE_ON,            /* on: listens while the Voice chat screen is open */
    WAKE_NO_MODEL,      /* the model partition is empty or has no wake word model (flash again) */
    WAKE_NO_MEMORY,     /* turned itself off: not enough internal RAM left (see the log) */
} wake_status_t;

void          voice_set_wake_word(bool on);  /* Settings switch (saved by the caller) */
wake_status_t voice_wake_status(void);
bool          voice_wake_listening(void);    /* listening for the wake word right now */
void          voice_set_foreground(bool on); /* the Voice chat screen is showing */

voice_state_t voice_state(void);
const char   *voice_state_text(voice_state_t st);
bool          voice_connected(void);
int64_t       voice_idle_us(void);           /* time since the last turn or touch */

/* Text shown on screen. The version number changes whenever any of it changes. */
uint32_t      voice_text_version(void);
void          voice_text_get(char *heard, size_t heard_len, char *reply, size_t reply_len,
                             char *note, size_t note_len);
/* Translator: the language pair of the text on screen (the server's TRANSLATION says which pair it
 * used; until then, the pair the connection was opened with). In Translator mode the text is UTF-8
 * for the ui_font_latin_* fonts (accents kept); in Voice chat mode it's ASCII. */
void          voice_text_langs(char *src, size_t src_len, char *dst, size_t dst_len);
/* Translator: the translation is on screen and its announced audio is still on the way. */
bool          voice_audio_pending(void);
/* Translator: play the last spoken translation again (kept until the next turn). Works while
 * disconnected too. voice_can_replay(): there is one and nothing else is recording or playing. */
bool          voice_can_replay(void);
bool          voice_replay(void);
