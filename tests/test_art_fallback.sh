#!/bin/sh
# Hermetic: the art worker falls back from an undecodable embedded picture
# (progressive JPEG) to the folder's cover file via `albumart`.
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"; kill $PID 2>/dev/null || true' EXIT
S=$T/mpd.sock
${CC:-cc} ${CFLAGS:--Wall -O2} -pthread -Isrc -I. -DUI_ART_NO_LVGL -DMPD_PATH="\"$S\"" tests/test_art_fallback.c \
    src/ui_art_fetch.c src/ui_art_job.c src/ui_art.c src/ui_mpd.c src/strtab.c src/tjpgd/tjpgd.c -o $T/t
for CASE in "badembed cover" "badembed-nocover none" "embedded cover"; do
  set -- $CASE
  # "embedded": control -- a decodable embedded picture is used as is
  if [ $1 = embedded ]; then EMB=tests/art_600.jpg; else EMB=tests/art_prog.jpg; fi
  python3 tests/fake_mpd.py $S tests/art_600.jpg $1 $EMB & PID=$!
  i=0; while [ ! -e $S.ready ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done; rm -f $S.ready
  $T/t tests/art_600.jpg $2
  kill $PID; wait $PID 2>/dev/null || true
done
echo "test_art_fallback: ALL CHECKS PASSED"
