#!/bin/sh
# Smoke the HOST SDL build of erosq-ui under Valgrind memcheck to catch leaks in
# the LVGL/D-Bus glue (ui_screens.c, ui_bt.c, ui_dbus.c) that the pure-logic
# unit tests structurally cannot reach. Needs SDL2 + valgrind on the host --
# this is a dev-box tool, not part of `run.sh`.
#
# The app is an event loop with no self-exit, so we run it for DUR seconds then
# SIGINT it; memcheck prints its leak summary at teardown. We report only
# definite + indirect losses -- "still reachable" is steady-state LVGL objects
# (the widget tree, styles) and is noise here. Library one-time allocs are
# quashed by tests/valgrind.supp.
#
# Usage:
#   tests/vg.sh                 # 15s window -- drive the UI by hand meanwhile
#   DUR=30 tests/vg.sh          # longer window for a bigger repro
#
# Reading the result: a real glue leak GROWS with activity. Run twice (short vs
# long, or idle vs heavy BT connect/disconnect) and compare the lost-bytes
# count -- a fixed number is a one-time alloc; a rising one is the leak.
set -e
cd "$(dirname "$0")/.."

command -v valgrind >/dev/null 2>&1 || { echo "vg.sh: valgrind not installed"; exit 1; }

# build.sh host is idempotent; only rebuild if the host binary is missing.
[ -x build-host/erosq-ui ] || ./build.sh host

DUR=${DUR:-15}
echo "vg.sh: running erosq-ui under valgrind for ${DUR}s -- exercise the UI now."

# timeout returns 124 on expiry; valgrind's own exit code is masked by the
# signal, so we don't gate on it -- the leak summary in the output is the result.
timeout -s INT "${DUR}" \
  valgrind \
    --leak-check=full \
    --show-leak-kinds=definite,indirect \
    --errors-for-leak-kinds=definite,indirect \
    --track-origins=yes \
    --suppressions=tests/valgrind.supp \
    build-host/erosq-ui || true

echo
echo "vg.sh: done. 'definitely/indirectly lost' above = real leaks in our code."
echo "        none reported = clean for the paths you exercised."
