#!/usr/bin/env bash
# Configure CBFD-Recompiled's host (ConkerRecomp) for the PS4: RT64 on RADV (GNM) through the port's
# VideoOut WSI, RecompFrontend's launcher and menus, SDL2/FreeType from the sysroot. Run through ~/conker
# (no space in the path).
#   configure-conker.sh [extra cmake args...]
# pthreads are in libkernel, but CMake's probe programs don't link: FindThreads is told.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
SRC="${CBFD_SRC:-$PS4_BUILD_ROOT/src/CBFD-Recompiled}"
BUILD="$PS4_BUILD_ROOT/build/conker"
export PS4_SYSROOT="$PS4_BUILD_ROOT/sysroot"
export PKG_CONFIG_LIBDIR="$PS4_SYSROOT/lib/pkgconfig"
unset PKG_CONFIG_PATH
ps4_cmake -S "$SRC/host" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$HERE/../toolchain/ps4-conker.cmake" \
    "-DCMAKE_PROJECT_ConkerRecomp_INCLUDE=$HERE/../toolchain/ps4-love-style.cmake" \
    -DCONKER_PS4_DIR="$(cd "$HERE/../ps4" && pwd)" \
    -DPS4_PAID="${PS4_PAID:-0x3800000000000035}" \
    -DPS4_MESA_BUILD="$PS4_BUILD_ROOT/build/mesa" \
    -DPS4_SYSROOT="$PS4_SYSROOT" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DSDL2_DIR="$PS4_SYSROOT/lib/cmake/SDL2" \
    -DCMAKE_HAVE_LIBC_PTHREAD=1 -DCMAKE_THREAD_LIBS_INIT= -DThreads_FOUND=TRUE \
    -DBUILD_TESTING=OFF \
    "$@"
