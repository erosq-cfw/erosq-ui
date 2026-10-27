#!/bin/sh
# Off -> On reconnect trigger across a bluetoothd restart, over a real private
# D-Bus daemon (tests/test_bt_owner.c). Runs twice when the Buildroot tree and
# qemu-mipsel-static are present: host GLib, then the DEVICE's GLib (the
# mipsel binary against output/target, under qemu) -- the signal routing that
# matters is the device library's, which may differ from the host's.
set -e
cd "$(dirname "$0")/.."
OUT=${OUT:-build-host}
mkdir -p "$OUT"
cc -O1 -g -Wall -Wextra -Isrc tests/test_bt_owner.c src/ui_bt_logic.c \
   $(pkg-config --cflags --libs gio-2.0) -o "$OUT/test_bt_owner"
echo "--- host GLib $(pkg-config --modversion gio-2.0)"
dbus-run-session -- "$OUT/test_bt_owner"

BR=${BR:-../buildroot/output}   # Buildroot output dir; override via env
QEMU=$(command -v qemu-mipsel-static || true)
if [ -x "$BR/host/bin/mipsel-buildroot-linux-musl-gcc" ] && [ -n "$QEMU" ]; then
  PC="$BR/host/bin/pkg-config"
  "$BR/host/bin/mipsel-buildroot-linux-musl-gcc" -O1 -g -Isrc tests/test_bt_owner.c src/ui_bt_logic.c \
     $("$PC" --cflags --libs gio-2.0) -o "$OUT/test_bt_owner.mipsel"
  echo "--- device GLib $("$PC" --modversion gio-2.0) (qemu-mipsel)"
  dbus-run-session -- "$QEMU" -L "$BR/target" "$OUT/test_bt_owner.mipsel"
else
  echo "--- device GLib run skipped (no Buildroot toolchain or qemu-mipsel-static)"
fi
