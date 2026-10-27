#!/bin/sh
# Screenshot the SDL host build after sending keys. arg1=bin arg2=out keys...
# Uses xdotool's own `sleep` subcommand (not the shell's) for timing.
set -e
BIN="$1"; OUT="$2"; shift 2
xvfb-run -a -s "-screen 0 320x240x24" sh -c "
  '$BIN' >/dev/null 2>&1 &
  P=\$!
  xdotool sleep 2 $(for k in $*; do printf 'key %s sleep 0.5 ' "$k"; done)
  scrot -o '$OUT'
  kill \$P 2>/dev/null || true
"
echo "wrote $OUT"
