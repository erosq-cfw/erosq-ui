#!/bin/sh
# Hermetic: strict fake MPD over a temp unix socket, a ~400 KB picture (> 3 x
# the 128 KB binarylimit) so mpd_pic_read crosses chunk boundaries, and a
# 1.5 s stall before the first chunk (a cold big cover measured 1.3-1.9 s on
# the device, which the old 1 s socket timeout turned into "no art").
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'kill $PID 2>/dev/null; rm -rf "$T"' EXIT
S=$T/mpd.sock
${CC:-cc} ${CFLAGS:--Wall -O2} -Isrc -I. -DMPD_PATH="\"$S\"" tests/test_picstream.c src/ui_mpd.c src/strtab.c -o $T/t
head -c 400000 /dev/urandom > $T/pic.bin
python3 tests/fake_mpd_strict.py $S $T/pic.bin 1.5 & PID=$!
i=0; while [ ! -e $S.ready ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done; rm -f $S.ready
$T/t $T/pic.bin
echo "test_picstream: ALL CHECKS PASSED"
