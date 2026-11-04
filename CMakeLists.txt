# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------------------------
# Board: 1_75c (Waveshare ESP32-S3-Touch-AMOLED-1.75C, the default) or 2_06 (ESP32-S3-Touch-AMOLED-2.06).
#   idf.py build flash monitor                                          -> 1.75C, build/ + sdkconfig
#   idf.py -B build_2_06 -D BOARD=2_06 build flash monitor              -> 2.06, build_2_06/ + sdkconfig.2_06
# Each board keeps its own build folder and sdkconfig, so switching never mixes the two.
# The choice is remembered in the build folder's CMake cache.
# ---------------------------------------------------------------------------------------------
if(NOT DEFINED BOARD)
    if(DEFINED ENV{FUSION_BOARD})
        set(BOARD $ENV{FUSION_BOARD})
    else()
        set(BOARD 1_75c)
    endif()
endif()
set(BOARD "${BOARD}" CACHE STRING "Board: 1_75c or 2_06")
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/boards/${BOARD}/board_bsp")
    message(FATAL_ERROR "Unknown BOARD '${BOARD}'. Use 1_75c or 2_06 (folders in boards/).")
endif()
message(STATUS "Fusion Apps board: ${BOARD}")

# common settings first, then the board's own
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;boards/${BOARD}/sdkconfig.defaults")
if(NOT BOARD STREQUAL "1_75c" AND NOT DEFINED SDKCONFIG)
    set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${BOARD}")   # 1.75C keeps plain "sdkconfig"
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Only the chosen board's folder is a component, so only its BSP is downloaded and built.
set(EXTRA_COMPONENT_DIRS
    ./components/bsp_extra
    ./boards/${BOARD}/board_bsp
    )

# dependencies.lock is shared: the component manager re-solves it (and swaps the BSP in
# managed_components/) when you switch boards. That takes a minute the first time.

string(TOUPPER "${BOARD}" BOARD_UPPER)
idf_build_set_property(COMPILE_DEFINITIONS "APP_BOARD_${BOARD_UPPER}=1" APPEND)

add_compile_options(-Wno-format)
project(fusion_apps)
