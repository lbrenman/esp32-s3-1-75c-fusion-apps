#pragma once
/* Voice chat engine: recording, WebSocket, reply playback (no LVGL in here).
 * The voice-chat screen polls voice_state() / voice_text_*() from an LVGL timer,
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

esp_err_t     voice_init(void);              /* once at boot: codec + tasks */
void          voice_set_active(bool active); /* app opened / closed: connect / disconnect */
void          voice_reconnect(void);         /* server URL or API key changed (also switches the connection on) */
void          voice_set_connected(bool on);
void          voice_new_conversation(void);  /* new saved conversation ID; reconnects if connected */  /* the Connection switch; on each time the app opens */
bool          voice_wants_connection(void);
void          voice_set_volume(int percent);

/* push-to-talk (call from the LVGL task) */
void          voice_ptt_press(void);
void          voice_ptt_release(void);
void          voice_ptt_cancel(void);        /* the touch was a swipe: discard the recording */

voice_state_t voice_state(void);
const char   *voice_state_text(voice_state_t st);
bool          voice_connected(void);
int64_t       voice_idle_us(void);           /* time since the last turn or touch */

/* Text shown on screen. The version number changes whenever any of it changes. */
uint32_t      voice_text_version(void);
void          voice_text_get(char *heard, size_t heard_len, char *reply, size_t reply_len,
                             char *note, size_t note_len);
