#!/bin/sh
# Hermetic: fake MPD over a temp unix socket, JPEG bigger than binary_limit so the
# chunked offset loop is exercised; embedded / cover-fallback / none paths.
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"; kill $PID 2>/dev/null || true' EXIT
S=$T/mpd.sock
${CC:-cc} -Wall -O2 -Isrc -I. -DMPD_PATH="\"$S\"" tests/test_readpicture.c src/ui_mpd.c src/strtab.c -o $T/t 2>/dev/null
python3 -c "
from PIL import Image; import random
im=Image.new('RGB',(400,400)); px=im.load()
for x in range(400):
  for y in range(400): px[x,y]=(x%256,y%256,(x*y)%256)
im.save('$T/big.jpg',quality=95)"
ls -l $T/big.jpg | awk '{ if ($5 < 20000) { print "fixture too small"; exit 1 } }'
for MODE in embedded cover none; do
  python3 tests/fake_mpd.py $S $T/big.jpg $MODE & PID=$!
  i=0; while [ ! -e $S.ready ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done; rm -f $S.ready
  if [ $MODE = none ]; then $T/t $T/big.jpg 0; else $T/t $T/big.jpg 1; fi
  kill $PID; wait $PID 2>/dev/null || true
done
echo "test_readpicture: ALL CHECKS PASSED"
