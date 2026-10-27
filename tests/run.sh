#!/bin/sh
# Host unit tests for the pure-logic units. These are dependency-free (no LVGL,
# no SDL, no glib) and build with a plain host cc -- that is the whole point of
# keeping the testable logic in *_logic.c / small self-contained units.
#
# The lvgl-linked units (keymap, manifest) and the full UI are covered by the
# CMake host build, which also registers these same tests in CTest:
#     ./build.sh host && ctest --test-dir build-host --output-on-failure
#
# Rendering/D-Bus glue (ui_screens.c, ui_bt.c, ui_dbus.c) stays HITL by design.
# Sanitizers (ASan + LSan + UBSan) are ON by default -- a switch nobody flips
# protects nothing; the flag existed for a month before anything ran it.
# SAN=0 turns them off for a quick plain build. These units don't
# allocate, so LSan mostly future-proofs; the real catch is out-of-bounds reads
# in the string parsers (wpa/mpd/manifest/podindex) and signed integer overflow
# in the arithmetic. Leaks/CPU in the LVGL/D-Bus glue are NOT host-testable --
# measure those on-device (MemAvailable + loadavg under load).
# fuzz_podindex is a dep-free random-mutation fuzzer over the podcast logic,
# the only hand-rolled code fed by network bytes. ITERS=/SEED= override.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-cc}
CFLAGS="-Wall -Wextra -O2 -Isrc -I."
case "${SAN-1}" in ""|0|no|off) ;; *) CFLAGS="$CFLAGS -g -fsanitize=address,undefined -fno-sanitize-recover=all" ;; esac
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

run() {  # run <name> <cc-args...>   (flags and sources, any order)
    name=$1; shift
    printf '%-16s ' "--- $name"
    $CC $CFLAGS "$@" -o "$T/$name"
    "$T/$name"
}

run bt_vol      tests/test_bt_vol.c
run seek        tests/test_seek.c
run memtrim     tests/test_memtrim.c
run np          tests/test_np.c
run about_logic tests/test_about_logic.c
run nav         tests/test_nav.c         src/ui_nav.c
run power_logic tests/test_power_logic.c src/ui_power_logic.c
run wake_logic  tests/test_wake_logic.c
run refresh_logic tests/test_refresh_logic.c
run settings_logic tests/test_settings_logic.c
run input_idle  tests/test_input_idle.c
run usb_logic   tests/test_usb_logic.c   src/ui_usb_logic.c
run btlogic     tests/test_btlogic.c     src/ui_bt_logic.c
run mixer       -DEROSQ_HOST=1 tests/test_mixer.c src/ui_mixer.c
run fd          tests/test_fd.c          src/ui_fd.c
run mpd         tests/test_mpd.c         src/ui_mpd.c src/strtab.c
run vlist       tests/test_vlist.c       src/vlist_logic.c
run play_cmd    tests/test_play_cmd.c
run art         -DUI_ART_NO_LVGL tests/test_art.c src/ui_art.c src/ui_mpd.c src/strtab.c src/tjpgd/tjpgd.c
run art_job     -pthread tests/test_art_job.c src/ui_art_job.c
run art_share   tests/test_art_share.c   src/ui_art_share.c
run art_scale   -DUI_ART_NO_LVGL tests/test_art_scale.c src/ui_art.c src/tjpgd/tjpgd.c
run art_stream  -DUI_ART_NO_LVGL tests/test_art_stream.c src/ui_art.c src/tjpgd/tjpgd.c
run art_rect    -DUI_ART_NO_LVGL tests/test_art_rect.c src/ui_art.c src/tjpgd/tjpgd.c
run wpa         tests/test_wpa.c         src/ui_net.c
run wpa_join    tests/test_wpa_join.c    src/ui_net.c
run wifi_saved  tests/test_wifi_saved.c  src/ui_net.c
run update_logic tests/test_update_logic.c
run podindex    tests/test_podindex.c    src/podindex_logic.c
run fuzz_podindex tests/fuzz_podindex.c  src/podindex_logic.c src/ui_podcast_logic.c
run podcast_logic tests/test_podcast_logic.c src/ui_podcast_logic.c
run rbtheme    tests/test_rbtheme.c     src/rbtheme_logic.c src/theme_logic.c
run nativetheme tests/test_nativetheme.c src/nativetheme_logic.c src/theme_logic.c
run binfont     tests/test_binfont.c     src/binfont_logic.c
# batt writes a fake power_supply node, so point it at a scratch dir.
run batt -DUI_BATT_PSY="\"$T/psy\"" tests/test_batt.c src/ui_batt.c
# clock_logic persists settings, so point it at a scratch dir instead of /data.
run clock_logic -DUI_CLOCK_DATA_DIR="\"$T/data\"" tests/test_clock_logic.c src/ui_clock.c
run clock_offset -DUI_CLOCK_DATA_DIR="\"$T/data\"" tests/test_clock_offset.c src/ui_clock.c

# --- script-level tests -----------------------------------------------------
# These shell out (stubbed wget/podindex, no network) instead of linking a unit,
# but they are run from here on purpose: test_pod_dirname_agree.sh is the only
# thing that keeps podfetch.sh's feed-directory rule byte-identical to
# pod_dir_name() in src/pod_name.h, and a check nobody runs protects nothing.
sh_test() {  # sh_test <name> <script>
    printf '%-16s ' "--- $1"
    if sh "$2" >"$T/$1.out" 2>&1; then
        tail -n 1 "$T/$1.out"
    else
        cat "$T/$1.out"
        exit 1
    fi
}
sh_test podfetch      tests/test_podfetch_hermetic.sh
sh_test dirname_agree tests/test_pod_dirname_agree.sh
sh_test podindex_wr   tests/test_podindex_write.sh
sh_test picstream     tests/test_picstream.sh

sh_test readpicture   tests/test_readpicture.sh
sh_test art_fallback  tests/test_art_fallback.sh
echo "all pure-logic tests passed"
