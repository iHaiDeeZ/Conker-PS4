#!/usr/bin/env bash
# The port's own libraries for the PS4, installed into ~/cbfd-build/sysroot:
#   SDL2 2.32: core services for the host, RT64 and RecompFrontend (events, timers, game controller
#              API over a virtual joystick fed from ScePad). Dummy video and audio drivers only: the
#              picture goes through RADV + VideoOut (ps4/ps4_wsi.cpp), sound through the sceAudioOut
#              driver patches/sdl2-ps4.patch adds (ps4/sdl2/SDL_ps4audio.c).
#   FreeType 2.13: RmlUi's fonts (the launcher and menus).
# Configure checks link real PS4 executables (PS4_TRY_LINK), so only functions the console has are used.
#   build-deps.sh [sdl2|freetype]...
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
PSB="$PS4_BUILD_ROOT"
export PS4_SYSROOT="$PSB/sysroot"
mkdir -p "$PSB/logs"
deps_cmake() {
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
    cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE="$HERE/../toolchain/ps4-deps.cmake" -DPS4_TRY_LINK=ON \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PS4_SYSROOT" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@"
}
build_sdl2() {
    local B="$PSB/build/sdl2"
    rm -rf "$B"
    deps_cmake -S "$PSB/src/SDL2" -B "$B" \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL2_DISABLE_SDL2MAIN=ON -DSDL_STATIC_PIC=ON \
        -DSDL_LOADSO=OFF -DSDL_RPATH=OFF \
        -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF \
        -DSDL_DIRECTFB=OFF -DSDL_RPI=OFF -DSDL_VIVANTE=OFF -DSDL_OFFSCREEN=OFF -DSDL_METAL=OFF \
        -DSDL_RENDER=OFF -DSDL_DUMMYVIDEO=ON -DSDL_DUMMYAUDIO=ON -DSDL_PS4AUDIO=ON -DSDL_DISKAUDIO=OFF \
        -DSDL_OSS=OFF -DSDL_ALSA=OFF -DSDL_JACK=OFF -DSDL_PIPEWIRE=OFF -DSDL_PULSEAUDIO=OFF -DSDL_SNDIO=OFF \
        -DSDL_ESD=OFF -DSDL_ARTS=OFF -DSDL_NAS=OFF -DSDL_FUSIONSOUND=OFF \
        -DSDL_HIDAPI=OFF -DSDL_HIDAPI_JOYSTICK=OFF -DSDL_VIRTUAL_JOYSTICK=ON -DSDL_JOYSTICK=ON -DSDL_HAPTIC=ON \
        -DSDL_SENSOR=ON -DSDL_POWER=OFF -DSDL_LIBUDEV=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_LIBSAMPLERATE=OFF \
        -DSDL_SYSTEM_ICONV=OFF -DSDL_LIBICONV=OFF \
        -DSDL_PTHREADS=ON -DSDL_PTHREADS_SEM=ON -DSDL_CLOCK_GETTIME=ON -DSDL_SSE3=ON -DSDL_3DNOW=OFF \
        > "$PSB/logs/sdl2-configure.log" 2>&1 || { tail -40 "$PSB/logs/sdl2-configure.log"; exit 1; }
    sed -n '/SDL2 was configured/,/^$/p;/Options:/,/^ *$/p' "$PSB/logs/sdl2-configure.log" | head -60 || true
    ninja -C "$B" > "$PSB/logs/sdl2-build.log" 2>&1 || { grep -E "error|undefined" "$PSB/logs/sdl2-build.log" | head -40; exit 1; }
    ninja -C "$B" install > /dev/null
    # The static target asks for libm and -pthread, which ld.lld (called directly) rejects: both are
    # in the system libraries already.
    sed -i 's/\\\$<LINK_ONLY:m>;\\\$<LINK_ONLY:-pthread>;//' "$PS4_SYSROOT/lib/cmake/SDL2/SDL2staticTargets.cmake"
    ls -la "$PS4_SYSROOT/lib/libSDL2.a"
}
build_freetype() {
    local B="$PSB/build/freetype"
    rm -rf "$B"
    deps_cmake -S "$PSB/src/freetype" -B "$B" \
        -DFT_DISABLE_ZLIB=OFF -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON \
        > "$PSB/logs/freetype-configure.log" 2>&1 || { tail -40 "$PSB/logs/freetype-configure.log"; exit 1; }
    ninja -C "$B" > "$PSB/logs/freetype-build.log" 2>&1 || { grep -E "error|undefined" "$PSB/logs/freetype-build.log" | head -40; exit 1; }
    ninja -C "$B" install > /dev/null
    ls -la "$PS4_SYSROOT/lib/libfreetype.a"
}
for dep in "${@:-sdl2 freetype}"; do
    for d in $dep; do "build_$d"; done
done
