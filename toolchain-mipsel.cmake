set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mips)
set(CMAKE_C_COMPILER mipsel-buildroot-linux-musl-gcc)
# Dynamic link: the device runs musl + glib/gio .so, and NO static glib .a exists
# in any cross sysroot -- a -static link fails on -lglib-2.0. Vendored helpers
# (libdrm, tinyalsa) are still pulled in as explicit .a paths.
set(CMAKE_C_FLAGS_INIT "-Os -ffunction-sections -fdata-sections")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--gc-sections")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
