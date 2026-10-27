#!/bin/sh
# Self-check for the last-intent reconcile (bt_up/bt_audio_up/bt_down + ui_bt.c
# bt_write_want). The mutex already serializes the scripts (see test_bt_lock.sh),
# so here we model the two serialized orders x two final intents and assert the
# state converges to the LAST intent written -- i.e. a fast off<->on can't invert.
set -u
TMP=$(mktemp -d); WANT="$TMP/want"; STATE="$TMP/state"
fail() { echo "FAIL: $1 (state=$(cat "$STATE"), want=$(cat "$WANT"))"; rm -rf "$TMP"; exit 1; }

# Mirror the shipped guards: an op bails if /run/bt-want no longer matches it.
up()   { [ "$(cat "$WANT" 2>/dev/null)" = 0 ] && return; echo up   > "$STATE"; }  # bt_up/bt_audio_up
down() { [ "$(cat "$WANT" 2>/dev/null)" = 1 ] && return; echo down > "$STATE"; }  # bt_down

# Final intent = ON (off then on). Both lock orders must end "up".
echo up   > "$STATE"; echo 1 > "$WANT"; down; up  ; [ "$(cat "$STATE")" = up ]   || fail "onlast/down-first"
echo down > "$STATE"; echo 1 > "$WANT"; up  ; down; [ "$(cat "$STATE")" = up ]   || fail "onlast/up-first"
# Final intent = OFF (on then off). Both lock orders must end "down".
echo up   > "$STATE"; echo 0 > "$WANT"; down; up  ; [ "$(cat "$STATE")" = down ] || fail "offlast/down-first"
echo up   > "$STATE"; echo 0 > "$WANT"; up  ; down; [ "$(cat "$STATE")" = down ] || fail "offlast/up-first"
# Absent want-file -> no opposing intent -> op proceeds (backward compatible).
rm -f "$WANT"; echo down > "$STATE"; up; [ "$(cat "$STATE")" = up ] || fail "absent-want/up-proceeds"

rm -rf "$TMP"
echo "PASS: converges to last intent for both lock orders; absent-want proceeds"
