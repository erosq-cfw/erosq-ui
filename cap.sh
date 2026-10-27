#!/bin/sh
# Throwaway screenshot harness for the SDL host build. arg1=binary arg2=out.png
set -e
BIN="$1"; OUT="$2"
xvfb-run -a -s "-screen 0 320x240x24" sh -c "
  '$BIN' >/dev/null 2>&1 &
  P=\$!
  scrot -d 2 -o '$OUT'
  kill \$P 2>/dev/null || true
"
echo "wrote $OUT"
