cmake_minimum_required(VERSION 3.18)
project(erosq_ui C)
set(CMAKE_C_STANDARD 11)

option(EROSQ_HOST "Build host SDL preview" OFF)

set(LV_CONF_PATH ${CMAKE_SOURCE_DIR}/lv_conf.h CACHE STRING "" FORCE)
# LVGL 9.6 defaults: build examples/demos + internal ThorVG (C++) and fetch
# missing deps from the network at configure time. None of that belongs in a
# Buildroot build.
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG OFF CACHE BOOL "" FORCE)
set(LV_FETCH_DEPENDENCIES OFF CACHE BOOL "" FORCE)

add_subdirectory(lvgl)
# The shipped build is -Os everywhere (Buildroot). LVGL's object/style/event
# core, misc helpers and software renderer are the scroll hot path (on-device PC
# sampling: style lookups, event dispatch, blend/fill); -O2 there cut Artists
# scroll CPU ~8% for +113 KB text (file-backed, no anon). Last -O wins.
get_target_property(_lv_srcs lvgl SOURCES)
list(FILTER _lv_srcs INCLUDE REGEX "/src/(core|misc|draw)/")
set_source_files_properties(${_lv_srcs} DIRECTORY ${CMAKE_SOURCE_DIR}/lvgl PROPERTIES COMPILE_OPTIONS "-O2")

# iPod fonts live with the lvgl lib so every target that links lvgl resolves
# LV_FONT_DEFAULT — same reason Montserrat ships inside lvgl. Kept out of src/
# so the UI_SRC glob doesn't also pull them (double-definition).
target_sources(lvgl PRIVATE
  ${CMAKE_SOURCE_DIR}/fonts/lv_font_ipod_14.c
  ${CMAKE_SOURCE_DIR}/fonts/lv_font_ipod_14b.c
  ${CMAKE_SOURCE_DIR}/fonts/lv_font_ipod_18b.c
  ${CMAKE_SOURCE_DIR}/fonts/lv_font_ipod_icon40.c)

# LVGL's GLOB_RECURSE picks up ARM NEON/Helium .S files unconditionally.
# On non-ARM hosts the system assembler cannot handle ARM instructions.
# DIRECTORY option (CMake >= 3.18) applies the property in the lvgl subdir scope.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64")
  set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/lvgl/src/draw/sw/blend/neon/lv_blend_neon.S
    ${CMAKE_SOURCE_DIR}/lvgl/src/draw/sw/blend/helium/lv_blend_helium.S
    DIRECTORY ${CMAKE_SOURCE_DIR}/lvgl
    PROPERTIES HEADER_FILE_ONLY TRUE
  )
endif()

file(GLOB UI_SRC ${CMAKE_SOURCE_DIR}/src/*.c)
# podindex.c has its own main() -- keep it out of the erosq-ui link.
list(REMOVE_ITEM UI_SRC ${CMAKE_SOURCE_DIR}/src/podindex.c)
list(APPEND UI_SRC ${CMAKE_SOURCE_DIR}/src/tjpgd/tjpgd.c)   # album-art JPEG decoder (own copy, descale on)
add_executable(erosq-ui ${UI_SRC})
target_include_directories(erosq-ui PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR})
target_link_libraries(erosq-ui PRIVATE lvgl)

# podindex: standalone RSS -> TSV indexer. expat is already on the target
# (libexpat.so.1, pulled in by dbus), so this costs no extra flash.
# The host dev box may not have expat headers; skip it there rather than
# breaking `./build.sh host`.
find_package(EXPAT QUIET)
if(EXPAT_FOUND)
  add_executable(podindex src/podindex.c src/podindex_logic.c)
  target_include_directories(podindex PRIVATE src ${CMAKE_SOURCE_DIR})
  target_link_libraries(podindex PRIVATE EXPAT::EXPAT)
else()
  message(STATUS "expat not found: skipping podindex (host builds only; the "
                 "cross build MUST have it via Buildroot staging)")
endif()

if(EROSQ_HOST)
  # Propagate EROSQ_HOST to lvgl so lv_conf.h picks SDL path and skips DRM includes.
  target_compile_definitions(lvgl PUBLIC EROSQ_HOST=1)
  target_compile_definitions(erosq-ui PRIVATE EROSQ_HOST=1)
  find_package(SDL2 REQUIRED)
  target_link_libraries(erosq-ui PRIVATE SDL2::SDL2)

  enable_testing()
  add_executable(test_keymap tests/test_keymap.c src/ui_input.c)
  target_include_directories(test_keymap PRIVATE src ${CMAKE_SOURCE_DIR})
  target_link_libraries(test_keymap PRIVATE lvgl)
  add_test(NAME keymap COMMAND test_keymap)

  add_executable(test_nav tests/test_nav.c src/ui_nav.c)
  target_include_directories(test_nav PRIVATE src)
  add_test(NAME nav COMMAND test_nav)

  add_executable(test_manifest tests/test_manifest.c src/ui_launch.c)
  target_include_directories(test_manifest PRIVATE src ${CMAKE_SOURCE_DIR})
  target_link_libraries(test_manifest PRIVATE lvgl)
  add_test(NAME manifest COMMAND test_manifest)

  add_executable(test_wpa tests/test_wpa.c src/ui_net.c)
  target_include_directories(test_wpa PRIVATE src ${CMAKE_SOURCE_DIR})
  add_test(NAME wpa COMMAND test_wpa)

  add_executable(test_wpa_join tests/test_wpa_join.c src/ui_net.c)
  target_include_directories(test_wpa_join PRIVATE src ${CMAKE_SOURCE_DIR})
  add_test(NAME wpa_join COMMAND test_wpa_join)

  add_executable(test_wifi_saved tests/test_wifi_saved.c src/ui_net.c)
  target_include_directories(test_wifi_saved PRIVATE src ${CMAKE_SOURCE_DIR})
  add_test(NAME wifi_saved COMMAND test_wifi_saved)

  add_executable(test_mixer tests/test_mixer.c src/ui_mixer.c)
  target_include_directories(test_mixer PRIVATE src ${CMAKE_SOURCE_DIR})
  target_compile_definitions(test_mixer PRIVATE EROSQ_HOST=1)  # compile out tinyalsa HW path
  add_test(NAME mixer COMMAND test_mixer)

  add_executable(test_mpd tests/test_mpd.c src/ui_mpd.c src/strtab.c)
  target_include_directories(test_mpd PRIVATE src ${CMAKE_SOURCE_DIR})
  add_test(NAME mpd COMMAND test_mpd)
  add_executable(test_vlist tests/test_vlist.c src/vlist_logic.c)
  target_include_directories(test_vlist PRIVATE src)
  add_test(NAME vlist COMMAND test_vlist)

  add_executable(test_art tests/test_art.c src/ui_art.c src/ui_mpd.c src/strtab.c src/tjpgd/tjpgd.c)
  target_include_directories(test_art PRIVATE src ${CMAKE_SOURCE_DIR})
  target_compile_definitions(test_art PRIVATE UI_ART_NO_LVGL=1)
  add_test(NAME art COMMAND test_art WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

  add_executable(test_art_rect tests/test_art_rect.c src/ui_art.c src/tjpgd/tjpgd.c)
  target_include_directories(test_art_rect PRIVATE src ${CMAKE_SOURCE_DIR})
  target_compile_definitions(test_art_rect PRIVATE UI_ART_NO_LVGL=1)
  add_test(NAME art_rect COMMAND test_art_rect WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

  add_executable(test_btlogic tests/test_btlogic.c src/ui_bt_logic.c)
  target_include_directories(test_btlogic PRIVATE src ${CMAKE_SOURCE_DIR})
  add_test(NAME btlogic COMMAND test_btlogic)

  add_executable(test_power_logic tests/test_power_logic.c src/ui_power_logic.c)
  target_include_directories(test_power_logic PRIVATE src ${CMAKE_SOURCE_DIR})
  add_test(NAME powerlogic COMMAND test_power_logic)

  add_executable(test_wake_logic tests/test_wake_logic.c)
  target_include_directories(test_wake_logic PRIVATE src)
  add_test(NAME wakelogic COMMAND test_wake_logic)

  add_executable(test_refresh_logic tests/test_refresh_logic.c)
  target_include_directories(test_refresh_logic PRIVATE src)
  add_test(NAME refreshlogic COMMAND test_refresh_logic)
  add_executable(test_settings_logic tests/test_settings_logic.c)
  target_include_directories(test_settings_logic PRIVATE src)
  add_test(NAME settingslogic COMMAND test_settings_logic)

  add_executable(test_input_idle tests/test_input_idle.c)
  target_include_directories(test_input_idle PRIVATE src)
  add_test(NAME inputidle COMMAND test_input_idle)

  add_executable(test_usb_logic tests/test_usb_logic.c src/ui_usb_logic.c)
  target_include_directories(test_usb_logic PRIVATE src ${CMAKE_SOURCE_DIR})
  add_test(NAME usblogic COMMAND test_usb_logic)

  add_executable(test_clock_logic tests/test_clock_logic.c src/ui_clock.c)
  target_include_directories(test_clock_logic PRIVATE src ${CMAKE_SOURCE_DIR})
  # persist to the build tree, not the host's /data
  target_compile_definitions(test_clock_logic PRIVATE
    UI_CLOCK_DATA_DIR="${CMAKE_CURRENT_BINARY_DIR}/clocktest")
  add_test(NAME clocklogic COMMAND test_clock_logic)

  add_executable(test_clock_offset tests/test_clock_offset.c src/ui_clock.c)
  target_include_directories(test_clock_offset PRIVATE src ${CMAKE_SOURCE_DIR})
  target_compile_definitions(test_clock_offset PRIVATE
    UI_CLOCK_DATA_DIR="${CMAKE_CURRENT_BINARY_DIR}/clocktest")
  add_test(NAME clockoffset COMMAND test_clock_offset)

  add_executable(test_batt tests/test_batt.c src/ui_batt.c)
  target_include_directories(test_batt PRIVATE src ${CMAKE_SOURCE_DIR})
  # fake power_supply node in the build tree, not the host's /sys
  target_compile_definitions(test_batt PRIVATE
    UI_BATT_PSY="${CMAKE_CURRENT_BINARY_DIR}/batttest")
  add_test(NAME batt COMMAND test_batt)

  # bt_vol is header-only (src/bt_vol.h) -- no source to compile, no lvgl link.
  add_executable(test_bt_vol tests/test_bt_vol.c)
  target_include_directories(test_bt_vol PRIVATE src ${CMAKE_SOURCE_DIR})
  add_test(NAME bt_vol COMMAND test_bt_vol)
  # seek is header-only too (src/ui_seek.h).
  add_executable(test_seek tests/test_seek.c)
  target_include_directories(test_seek PRIVATE src)
  add_test(NAME seek COMMAND test_seek)
  # memtrim's page-window maths is header-only too (src/ui_memtrim.h).
  add_executable(test_memtrim tests/test_memtrim.c)
  target_include_directories(test_memtrim PRIVATE src)
  add_test(NAME memtrim COMMAND test_memtrim)
  add_executable(test_np tests/test_np.c)
  target_include_directories(test_np PRIVATE src)
  add_test(NAME np COMMAND test_np)
  add_executable(test_about_logic tests/test_about_logic.c)
  target_include_directories(test_about_logic PRIVATE src)
  add_test(NAME about_logic COMMAND test_about_logic)

  add_executable(test_podindex tests/test_podindex.c src/podindex_logic.c)
  target_include_directories(test_podindex PRIVATE src ${CMAKE_SOURCE_DIR})
  add_test(NAME podindex COMMAND test_podindex)

  add_executable(test_podcast_logic tests/test_podcast_logic.c src/ui_podcast_logic.c)
  target_include_directories(test_podcast_logic PRIVATE src ${CMAKE_SOURCE_DIR})
  add_test(NAME podcastlogic COMMAND test_podcast_logic)

  add_executable(test_rbtheme tests/test_rbtheme.c src/rbtheme_logic.c src/theme_logic.c)
  target_include_directories(test_rbtheme PRIVATE src)
  add_test(NAME rbtheme COMMAND test_rbtheme)

  add_executable(test_nativetheme tests/test_nativetheme.c src/nativetheme_logic.c src/theme_logic.c)
  target_include_directories(test_nativetheme PRIVATE src)
  add_test(NAME nativetheme COMMAND test_nativetheme)
  add_executable(test_binfont tests/test_binfont.c src/binfont_logic.c)
  target_include_directories(test_binfont PRIVATE src)
  add_test(NAME binfont COMMAND test_binfont WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

  # Registered here as well as in tests/run.sh: run.sh's own header advertises
  # `./build.sh host && ctest --test-dir build-host` as the equivalent path, so
  # a fuzzer present only in run.sh is silently skipped by CTest and by CI
  # (code review 2026-09-08).
  add_executable(fuzz_podindex tests/fuzz_podindex.c src/podindex_logic.c src/ui_podcast_logic.c)
  target_include_directories(fuzz_podindex PRIVATE src ${CMAKE_SOURCE_DIR})
  add_test(NAME fuzz_podindex COMMAND fuzz_podindex)
else()
  # LIBDRM_INC must be on lvgl too: lv_linux_drm.c includes <xf86drm.h> which
  # must resolve to the cross-built libdrm source, not the host system headers.
  target_include_directories(lvgl PRIVATE ${LIBDRM_INC})
  target_include_directories(erosq-ui PRIVATE ${LIBDRM_INC})
  target_link_libraries(erosq-ui PRIVATE ${LIBDRM_LIB})
  target_include_directories(erosq-ui PRIVATE ${LIBTINYALSA_INC})
  target_link_libraries(erosq-ui PRIVATE ${LIBTINYALSA_LIB})

  # glib-2.0 + gio-2.0 for D-Bus (system bus via GDBusConnection).
  # Buildroot passes PKG_CONFIG_PATH pointing at the staging dir .pc files,
  # so find_package(PkgConfig) + pkg_check_modules() resolves the cross headers
  # and libs automatically.  We guard with if() so a missing .pc produces a
  # clear message rather than a cryptic link error.
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLIB REQUIRED glib-2.0)
  pkg_check_modules(GIO  REQUIRED gio-2.0)
  target_include_directories(erosq-ui PRIVATE ${GLIB_INCLUDE_DIRS} ${GIO_INCLUDE_DIRS})
  target_link_directories(erosq-ui PRIVATE ${GIO_LIBRARY_DIRS})
  target_link_libraries(erosq-ui PRIVATE ${GIO_LIBRARIES} ${GLIB_LIBRARIES})
  target_compile_options(erosq-ui PRIVATE ${GLIB_CFLAGS_OTHER} ${GIO_CFLAGS_OTHER})
endif()
