#!/bin/sh
set -e
cd "$(dirname "$0")"
ROOT=$(pwd)

# Cross toolchain + pkg-config come from the Buildroot host tree. Wire them up if
# they're not already on PATH so a bare ./build.sh works. The Buildroot pkg-config
# wrapper bakes in the staging sysroot, so glib-2.0/gio-2.0 resolve to the cross
# (musl) headers + .so -- NOT the host's /usr/include (which the wrapper rejects).
BR_HOST=${BR_HOST:-$ROOT/../buildroot/output/host}   # Buildroot output/host; override via env
if [ "$1" != "host" ] && [ -d "$BR_HOST/bin" ]; then
  command -v mipsel-buildroot-linux-musl-gcc >/dev/null 2>&1 || PATH="$BR_HOST/bin:$PATH"
  export PATH
  [ -n "$PKG_CONFIG" ] || export PKG_CONFIG="$BR_HOST/bin/pkg-config"
fi

# --- apply our vendored-lvgl patches (lvgl is gitignored, so this is how they
#     survive a re-vendor). Idempotent: skip any already applied.
#       * lv_linux_drm-drm-master-handoff: get_fd + force_modeset for the Extras launcher,
#         FB_DAMAGE_CLIPS for the windowed SLCD DMA
#     (lv_buttonmatrix-keypad-pressing-no-reset is upstream since LVGL 9.6)
# ---
if [ -d lvgl/.git ]; then
  for PATCH in "$ROOT"/patches/*.patch; do
    [ -f "$PATCH" ] || continue
    if git -C lvgl apply --reverse --check "$PATCH" 2>/dev/null; then
      : # already applied
    elif git -C lvgl apply --check "$PATCH" 2>/dev/null; then
      git -C lvgl apply "$PATCH" && echo "applied $(basename "$PATCH")"
    else
      echo "WARNING: $(basename "$PATCH") does not apply cleanly"
    fi
  done
fi

if [ "$1" = "host" ]; then
  cmake -S . -B build-host -DEROSQ_HOST=ON
  cmake --build build-host -j"$(nproc)"
  echo "host binary: build-host/erosq-ui"
  exit 0
fi

# --- cross-build libdrm (static, core only) ---
if [ ! -f libdrm/build-mipsel/libdrm.a ]; then
  [ -d libdrm ] || git clone --depth 1 --branch libdrm-2.4.120 https://gitlab.freedesktop.org/mesa/drm.git libdrm
  cd libdrm
  meson setup build-mipsel --cross-file ../cross-mipsel.txt \
    -Ddefault_library=static -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled \
    -Dnouveau=disabled -Dvmwgfx=disabled -Dvc4=disabled -Dfreedreno=disabled \
    -Detnaviv=disabled -Dtests=false -Dman-pages=disabled --reconfigure || true
  ninja -C build-mipsel
  cd "$ROOT"
fi

# --- cross-build tinyalsa 1.1.1 (static; MUST be 1.1.1, on-device 2.x can't set the 2-val ctl) ---
if [ ! -f tinyalsa/src/libtinyalsa.a ]; then
  [ -d tinyalsa ] || git clone --depth 1 --branch 1.1.1 https://github.com/tinyalsa/tinyalsa.git tinyalsa
  make -C tinyalsa/src CROSS_COMPILE=mipsel-buildroot-linux-musl- libtinyalsa.a
fi

cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-mipsel.cmake \
  -DLIBDRM_INC="$ROOT/libdrm;$ROOT/libdrm/build-mipsel;$ROOT/libdrm/include/drm" \
  -DLIBDRM_LIB="$ROOT/libdrm/build-mipsel/libdrm.a" \
  -DLIBTINYALSA_INC="$ROOT/tinyalsa/include" \
  -DLIBTINYALSA_LIB="$ROOT/tinyalsa/src/libtinyalsa.a"
cmake --build build -j"$(nproc)"
file build/erosq-ui
