# Third-party code in this project

| Path | Source | License |
| --- | --- | --- |
| `CMakeLists.txt`, `partitions.csv`, `sdkconfig.defaults` (first part), `components/bsp_extra/` | Waveshare [ESP32-S3-Touch-AMOLED-1.75C](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C) example `examples/esp-idf/05_Spec_Analyzer` | Apache-2.0 (`components/bsp_extra/LICENSE`) |
| `main/minimp3.h` | [lieff/minimp3](https://github.com/lieff/minimp3) | CC0 1.0 (`main/LICENSE.minimp3`) |
| `assets/fa-solid-900.ttf`, icon glyphs in `main/ui/ui_icons_*.c` | [Font Awesome Free 6.7.2](https://fontawesome.com) by Fonticons, Inc. | Font: SIL OFL 1.1; icons: CC BY 4.0 (`assets/LICENSE.fontawesome.txt`) |

Changes to Waveshare's files: the project name in `CMakeLists.txt` is `fusion_apps`
(was `lvgl_demo_v9`), and `sdkconfig.fusion_apps` is appended to `sdkconfig.defaults`.

The voice engine (`main/apps/voice_core.c`) comes from
[ESP32-S3-1-75C-fusion-voice-chat](https://github.com/lbrenman/ESP32-S3-1-75C-fusion-voice-chat).
