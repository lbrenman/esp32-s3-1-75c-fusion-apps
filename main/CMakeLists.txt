# No REQUIRES list on purpose: the "main" component then depends on every component in the
# project (Waveshare's bsp_extra, the 1.75C BSP, LVGL, esp_websocket_client, Wi-Fi, cJSON, NVS...).

# ---------------------------------------------------------------------------------------------
# server_ca.pem: the voice server's TLS certificate chain, compiled into the firmware.
# It is used when the server URL (Voice chat > swipe up) points at WS_HOST from secrets.h;
# any other host is verified with ESP-IDF's built-in CA bundle instead.
# If it's missing, fetch it automatically from WS_HOST:WS_PORT in secrets.h, so a fresh clone
# only needs secrets.h filled in. To refresh it (server changed certificate authority):
#     rm main/server_ca.pem && idf.py reconfigure
# (Skipped during ESP-IDF's early "requirements" pass, which runs this file in script mode.)
# ---------------------------------------------------------------------------------------------
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(SECRETS_H "${COMPONENT_DIR}/secrets.h")
    set(CA_PEM    "${COMPONENT_DIR}/server_ca.pem")

    if(NOT EXISTS "${SECRETS_H}")
        message(FATAL_ERROR
            "\nmain/secrets.h is missing. Create it from the template and fill in your values:\n"
            "    cp main/secrets.h.example main/secrets.h\n"
            "    open -e main/secrets.h\n")
    endif()

    if(NOT EXISTS "${CA_PEM}")
        file(STRINGS "${SECRETS_H}" host_line REGEX "^[ \t]*#define[ \t]+WS_HOST[ \t]+\"")
        file(STRINGS "${SECRETS_H}" port_line REGEX "^[ \t]*#define[ \t]+WS_PORT[ \t]+[0-9]+")
        string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" ws_host "${host_line}")
        string(REGEX REPLACE ".*WS_PORT[ \t]+([0-9]+).*" "\\1" ws_port "${port_line}")
        if(ws_port STREQUAL "")
            set(ws_port 443)
        endif()
        if(ws_host STREQUAL "" OR ws_host MATCHES "your-")
            message(FATAL_ERROR "\nSet WS_HOST in main/secrets.h before building.\n")
        endif()

        find_program(OPENSSL_EXE openssl)
        if(NOT OPENSSL_EXE)
            message(FATAL_ERROR "\nopenssl not found; it is needed to fetch main/server_ca.pem.\n")
        endif()

        message(STATUS "Fetching TLS certificate chain from ${ws_host}:${ws_port}")
        execute_process(
            COMMAND "${OPENSSL_EXE}" s_client -connect "${ws_host}:${ws_port}"
                    -servername "${ws_host}" -showcerts
            INPUT_FILE /dev/null
            OUTPUT_VARIABLE s_client_out
            ERROR_QUIET
            TIMEOUT 30)

        # base64 never contains '-', so this matches exactly one certificate per item
        string(REGEX MATCHALL "-----BEGIN CERTIFICATE-----[^-]+-----END CERTIFICATE-----"
               certs "${s_client_out}")
        list(LENGTH certs cert_count)
        if(cert_count EQUAL 0)
            message(FATAL_ERROR
                "\nCould not fetch a certificate from ${ws_host}:${ws_port}.\n"
                "Check WS_HOST / WS_PORT in main/secrets.h and that the server is reachable:\n"
                "    openssl s_client -connect ${ws_host}:${ws_port} -servername ${ws_host}\n")
        endif()
        string(REPLACE ";" "\n" pem_text "${certs}")
        file(WRITE "${CA_PEM}" "${pem_text}\n")
        message(STATUS "Saved ${cert_count} certificate(s) to main/server_ca.pem")
    endif()
endif()

idf_component_register(SRCS "app_main.c"
                            "settings_store.c"
                            "net.c"
                            "text_ascii.c"
                            "power.c"
                            "display_start.c"
                            "minimp3_impl.c"
                            "ui/ui_theme.c"
                            "ui/ui_nav.c"
                            "ui/ui_widgets.c"
                            "ui/ui_icons_16.c"
                            "ui/ui_icons_20.c"
                            "ui/ui_icons_28.c"
                            "ui/ui_icons_56.c"
                            "ui/ui_font_latin_14.c"
                            "ui/ui_font_latin_16.c"
                            "ui/ui_font_latin_20.c"
                            "ui/ui_font_latin_28.c"
                            "ui/ui_font_arabic_16.c"
                            "ui/ui_font_arabic_20.c"
                            "ui/ui_font_arabic_28.c"
                            "ui/ui_font_deva_16.c"
                            "ui/ui_font_deva_20.c"
                            "ui/ui_font_deva_28.c"
                            "apps/apps.c"
                            "apps/app_home.c"
                            "apps/app_settings.c"
                            "apps/app_voice.c"
                            "apps/voice_core.c"
                            "apps/app_translate.c"
                            "apps/translate_langs.c"
                            "apps/app_stocks.c"
                            "apps/stocks_core.c"
                            "apps/stocks_parse.c"
                            "mcp_client.c"
                       INCLUDE_DIRS "." "ui" "apps"
                       EMBED_TXTFILES "server_ca.pem")

# minimp3 is third-party code: don't let its warnings fail the build
set_source_files_properties(minimp3_impl.c PROPERTIES COMPILE_OPTIONS "-Wno-error;-w")
