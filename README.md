# erosq-ui

A music-player user interface for the EROS Q (Ingenic X1000) running mainline
Linux. It is written in C on top of [LVGL](https://lvgl.io) 9.6 and drives:

- playback through MPD (library browsing, playlists, album art, seek, shuffle)
- Bluetooth audio through BlueZ and BlueALSA, Wi-Fi through wpa_supplicant
- podcasts (RSS), themes, clock, battery and power settings
- an "Extras" launcher for SD-card applications (`manifest/extras.tsv`)

The display is a 320x240 panel driven over DRM; the host build renders the
same UI in an SDL window for development.

## Build

LVGL is not vendored. Clone it next to the sources first:

    git clone --depth 1 --branch v9.6.0 https://github.com/lvgl/lvgl.git lvgl

Host preview (needs CMake, SDL2, GLib/GIO and expat development packages):

    ./build.sh host
    ./build-host/erosq-ui

Device (cross) build, with a Buildroot toolchain for mipsel/musl:

    BR_HOST=/path/to/buildroot/output/host ./build.sh

`build.sh` applies the patches in `patches/` to `lvgl/`, builds static libdrm
and tinyalsa, then the UI.

## Tests

Pure-logic unit tests build with a plain host C compiler:

    tests/run.sh

The CMake host build registers the same tests (plus the LVGL-linked ones) in
CTest:

    ./build.sh host && ctest --test-dir build-host --output-on-failure

## Licence

MIT, see [LICENSE](LICENSE). Bundled third-party fonts and the JPEG decoder
keep their own licences, listed in [NOTICE](NOTICE).
