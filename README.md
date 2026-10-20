# esp32-s3-1-75c-fusion-apps — App launcher for the ESP32-S3-Touch-AMOLED-1.75C

A small "watch OS" for the Waveshare **ESP32-S3-Touch-AMOLED-1.75C**: a home screen with a clock and
app icons, a **Voice chat** app (push-to-talk client for the Amplify Fusion WebSocket voice server)
and a **Settings** app. Day and night themes, Font Awesome icons, slide transitions.

![Screens](docs/screenshots/overview.png)

*Screenshots are rendered by `tools/ui_preview` from the real UI code (see below), not photos of
the device.*

> **First draft.** The UI code is compiled and exercised on a PC against LVGL 9.5, and the
> ESP-IDF side is syntax-checked, but this version has not been built with `idf.py` or run on a
> board yet. Expect a small fix or two on the first build.

---

## What's on the device

| Screen | What it does |
| --- | --- |
| **Home** | Date and time (from NTP), app icons, Wi-Fi status, day/night button |
| **Voice chat** | Hold to talk, release to send. Shows what the server heard and its reply, plays the reply audio |
| **Voice server** (swipe up/down in Voice chat) | See and change the server URL and API key, connection status, reset URL to `secrets.h` |
| **Settings** | Wi-Fi network (scan list or typed name) and password, dark mode, brightness, volume, restart, reset |

### Gestures

| Where | Gesture | Result |
| --- | --- | --- |
| Voice chat | Touch and hold | Record while held (up to 10 s); release to send |
| Voice chat | Swipe up / down | Voice server settings |
| Voice chat, Voice server | Swipe left / right | Back to Home |
| Voice server | Swipe up / down, or the ⌄ button | Back to Voice chat |
| Settings and its screens | Swipe right, or the ‹ button | Back |

Recording starts the moment your finger lands, so your first word isn't cut off. If the finger
then moves at least 60 px within 0.8 s, the touch was a swipe and the recording is thrown away.

The WebSocket is opened when Voice chat opens and closed when you go back to Home. After 3 minutes
idle, Voice chat returns to Home by itself (`VOICE_AUTO_HOME_MS`).

### Voice chat ring colors

| Ring | Status | Meaning |
| --- | --- | --- |
| Amber | Waiting for Wi-Fi... / Connecting... | Connecting to Wi-Fi or the server |
| **Green** | **Hold to talk** | Ready |
| Red | Listening... | Recording (the mic orb pulses) |
| Blue | Thinking... | Waiting for the reply (three bouncing dots) |
| Purple | Speaking... | Playing the reply |
| Red | Not connected | No usable server URL; the note under the mic says what to fix |

---

## Settings: secrets.h is only the first-boot default

`main/secrets.h` holds your Wi-Fi and server values, as before. On first boot the device copies
them into its settings. Anything you change on the device (Wi-Fi in Settings, URL and API key in
Voice chat) is saved in the device's flash (NVS) and survives reboots and re-flashing.

- **Settings > Reset settings** goes back to the `secrets.h` values.
- `idf.py erase-flash` also clears them.
- Secrets are never printed to the serial log and are shown masked on screen (`••••••••abcd`).
- Values typed on the device are stored unencrypted in flash, as the compiled-in `secrets.h`
  values already are.

## Folder layout

```
esp32-s3-1-75c-fusion-apps/
├── CMakeLists.txt, partitions.csv, sdkconfig.defaults   Waveshare 1.75C base (+ this project's settings)
├── sdkconfig.fusion_apps          just this project's settings, for reference
├── components/bsp_extra/          Waveshare's audio helper (codec, I2S, speaker amp)
├── main/
│   ├── app_main.c                 boot: settings -> display/UI -> audio -> Wi-Fi
│   ├── app_config.h               compiled-in options: time zone, 12/24 h clock, animation time...
│   ├── settings_store.c/.h        settings in NVS, defaults from secrets.h
│   ├── net.c/.h                   Wi-Fi station, scanning, NTP clock
│   ├── secrets.h.example          copy to secrets.h (gitignored)
│   ├── minimp3.h, minimp3_impl.c  MP3 decoder (CC0)
│   ├── ui/
│   │   ├── ui_theme.c/.h          day/night palettes and app colors
│   │   ├── ui_nav.c/.h            screen stack, app open/close, transitions
│   │   ├── ui_widgets.c/.h        list screens, rows, switches, sliders, editor + keyboard, toasts, dialogs
│   │   └── ui_icons*.c/.h         icon fonts (16/20/28/56 px), generated from Font Awesome
│   └── apps/
│       ├── apps.c/.h              the app list shown on Home
│       ├── app_home.c             Home screen
│       ├── app_settings.c         Settings app (+ Wi-Fi scan screen)
│       ├── app_voice.c            Voice chat screen + Voice server screen
│       └── voice_core.c, voice.h  recording, WebSocket, playback (no LVGL in here)
├── tools/
│   ├── make_icon_font.py          regenerates main/ui/ui_icons*.c
│   └── ui_preview/                runs the UI on your Mac and saves screenshots
├── assets/                        Font Awesome Free TTF + license (input for make_icon_font.py)
└── docs/screenshots/
```

All LVGL work happens in the LVGL task. The voice engine, Wi-Fi and settings never touch LVGL
objects; the screens read their state from LVGL timers. That keeps tasks from drawing on screens
that have already been closed.

## Requirements

Same as voice_chat:

- Waveshare **ESP32-S3-Touch-AMOLED-1.75C** (not the plain 1.75)
- ESP-IDF **5.5 or newer**, exported in your terminal (`get_idf`)
- `openssl` (preinstalled on macOS), used once to fetch the server certificate
- A 2.4 GHz Wi-Fi network, a speaker on the board's speaker connector, and your voice server on `wss://`

## Setup

ESP-IDF doesn't support paths with spaces, so keep the project in `~/esp/projects`.

```bash
# 1. Unpack the project
mkdir -p ~/esp/projects && cd ~/esp/projects
unzip ~/Downloads/esp32-s3-1-75c-fusion-apps.zip
cd esp32-s3-1-75c-fusion-apps

# 2. Your settings. Reuse the voice_chat ones if you have them...
cp ~/esp/projects/ESP32-S3-1-75C-fusion-voice-chat/main/secrets.h main/secrets.h
#    ...or start from the template:
#    cp main/secrets.h.example main/secrets.h && open -e main/secrets.h

# 3. Build, flash, watch the log (exit the monitor with Ctrl-])
get_idf
idf.py set-target esp32s3
idf.py -p /dev/cu.usbmodem101 build flash monitor
```

The first build downloads the BSP, LVGL and the WebSocket client and fetches `main/server_ca.pem`
from `WS_HOST:WS_PORT`, so it takes a few minutes.

Optional: put it in git and on GitHub (`secrets.h` and `server_ca.pem` are gitignored):

```bash
git init && git add . && git commit -m "First draft: home screen, voice chat, settings"
gh repo create lbrenman/esp32-s3-1-75c-fusion-apps --public --source . --push
```

## TLS certificate

As in voice_chat, the build saves the server's certificate chain to `main/server_ca.pem` and
compiles it in. The device uses it when the server URL's host is the `WS_HOST` from `secrets.h`.
If you point Voice chat at a **different host** on the device, it checks that server against
ESP-IDF's built-in CA bundle instead (the log says `using the built-in CA bundle`). That works for
servers with publicly trusted certificates that send their full chain; for anything else, put the
new host in `secrets.h` and refresh the certificate:

```bash
rm main/server_ca.pem && idf.py reconfigure && idf.py build flash
```

## Adding an app

1. Create `main/apps/app_<name>.c` with a build function that returns a screen, and a descriptor:

   ```c
   #include "apps.h"
   #include "ui_widgets.h"

   static lv_obj_t *build(void *arg)
   {
       lv_obj_t *scr;
       lv_obj_t *list = ui_list_screen(&scr, "Hello", true);   // header + back button + swipe-right back
       ui_row(list, ICON_INFO, UI_C_BLUE, "Hello", "From a new app", NULL, NULL);
       return scr;
   }

   const ui_app_t APP_HELLO = {
       .name = "Hello", .icon = ICON_ROBOT,
       .color1 = 0xFF9F0A, .color2 = 0xFF5A1F,     // tile gradient
       .build = build,                             // .on_enter / .on_leave are optional
   };
   ```

2. Declare it in `main/apps/apps.h` (`extern const ui_app_t APP_HELLO;`) and add `&APP_HELLO` to
   `g_apps` in `main/apps/apps.c`.
3. Add the file to `SRCS` in `main/CMakeLists.txt`.

Home lays the tiles out three per row and scrolls when they don't fit.

## Icons

Icons are Font Awesome Free 6 (solid) glyphs converted to LVGL fonts at 16, 20, 28 and 56 px.
The names are in `main/ui/ui_icons.h` (`ICON_MIC`, `ICON_GEAR`, `ICON_WIFI`, ...). To add one, find
its code point on [fontawesome.com](https://fontawesome.com/search?o=r&m=free&s=solid), add a line
to `ICONS` in `tools/make_icon_font.py`, then regenerate:

```bash
python3 -m pip install --user pillow        # once
python3 tools/make_icon_font.py assets/fa-solid-900.ttf main/ui
```

## Preview the UI on your Mac

`tools/ui_preview` compiles the real `main/ui` and `main/apps` code for your Mac with fake Wi-Fi,
settings and voice data, taps and swipes through every screen, and saves screenshots. It's the
fastest way to try a layout change before flashing.

```bash
cd tools/ui_preview
make                      # first run clones and builds LVGL 9.5 (about a minute)
open shots/contact_sheet.png
```

Change the fake values in `stubs.c` or the click-through script in `sim.c` to preview other states.

## Settings you might change

| Where | Setting | Default |
| --- | --- | --- |
| `app_config.h` | `APP_TZ` (POSIX time zone for the clock) | US Eastern |
| `app_config.h` | `APP_CLOCK_24H` | 0 (12-hour) |
| `app_config.h` | `UI_ANIM_MS` (screen slide time) | 220 ms; 0 = instant |
| `app_config.h` | `UI_SWIPE_MIN_PX`, `UI_SWIPE_MAX_MS` | 60 px, 800 ms |
| `app_config.h` | `VOICE_AUTO_HOME_MS` | 3 min; 0 = never |
| `app_config.h` | `DEFAULT_DARK_MODE`, `DEFAULT_BRIGHTNESS`, `DEFAULT_VOLUME` | dark, 80 %, 75 % (first boot) |
| `apps/voice_core.c` | `REC_MAX_SECONDS`, `TURN_TIMEOUT_US`, `RX_MAX_BYTES` | 10 s, 45 s, 3 MB |
| `ui/ui_theme.c` | palette colors | |

## Protocol

Unchanged from voice_chat: connect to `<server URL>?x-api-key=<key>&conversationId=<uuid>`, send
one binary frame per turn (WAV, 16 kHz mono 16-bit), receive JSON text frames
(`TRANSCRIBED_REQUEST`, `RESPONSE`, `ERROR`) and binary reply audio (MP3, WAV or raw PCM).
A new `conversationId` is made each time Voice chat opens.

## Troubleshooting

Read the serial log first (`idf.py -p /dev/cu.usbmodem101 monitor`). Tags: `app`, `settings`,
`net`, `voice`.

| Symptom / log | Likely cause | Fix |
| --- | --- | --- |
| Home pill says "Wi-Fi not set" | No SSID saved | Settings > Network, or set `WIFI_SSID` and Reset settings |
| Settings > Status "Network not found" (`reason 201`) | SSID wrong or 5 GHz | Pick a 2.4 GHz network |
| Settings > Status "Wrong password?" (`reason 15`/`202`) | Password | Settings > Password |
| Home shows "Fusion Apps" instead of the time | NTP hasn't synced yet | Wait for Wi-Fi; the clock appears within seconds |
| Voice chat: "Set the server URL: swipe up" | URL doesn't start with `wss://` | Swipe up > Server URL |
| "Server refused the connection - check the API key" | Handshake rejected (code 1006) | Swipe up > API key |
| "Can't reach the server - check the URL" | Host, port, path or TLS | Check the URL; for a new host see "TLS certificate" |
| `No matching trusted root certificate found` | Host not covered by `server_ca.pem` or the CA bundle | Put the host in `secrets.h`, refresh `server_ca.pem` |
| Build stops: `main/secrets.h is missing` | Fresh unzip | `cp main/secrets.h.example main/secrets.h` |
| Build error about a `lv_font_montserrat_*` font | Fonts not enabled | `rm sdkconfig && idf.py set-target esp32s3` (rereads `sdkconfig.defaults`) |
| `Draw bitmap failed: ESP_ERR_NO_MEM` | Too little internal RAM for display DMA | Check the `SPIRAM_MALLOC_*` lines are in `sdkconfig.defaults`, then `rm sdkconfig` and set-target again |
| Recorded 0 ms / no transcript | Mic not capturing | Confirm the board is the 1.75C |
| Swipes start recordings, or holds count as swipes | Swipe thresholds | Adjust `UI_SWIPE_MIN_PX` / `UI_SWIPE_MAX_MS` |
