// Conker's Bad Fur Day: Recompiled for PS4: the console-specific services the host, the runtime
// and RecompFrontend call (ps4_platform.cpp, ps4_pad.cpp).
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The app's folder: /data/Conker (port/ps4_crashlog.cpp)
const char *ps4_data_dir(void);
// The log, the user service: first thing in main()
void ps4_platform_init(void);

// Game memory. The PS4 can't reserve gigabytes with mmap(PROT_NONE) (anonymous memory is
// flexible memory, a few hundred MiB): the runtime's 4 GiB of guest space is a virtual range
// reservation, and the parts in use are backed by direct memory, zeroed, 2 MiB at a time.
void *ps4_vm_reserve(size_t size);
// Backs [address, address + size) inside the reservation (backed parts keep their data). 0 on success.
int ps4_vm_commit(void *address, size_t size);

// DualShock 4s as SDL game controllers (virtual joysticks fed from ScePad), after SDL_Init.
void ps4_pad_init(void);
// How many times the quick menu was asked for (touch pad click, L3 + R3) since the last call.
int ps4_pad_take_quick_menu_requests(void);
// The first pad's held buttons (ORBIS_PAD_BUTTON_*, the left stick as the d-pad), for the quick menu.
uint32_t ps4_pad_menu_buttons(void);

// The quick menu (ps4_quick_menu.cpp), from the host's frame update on the main thread.
void ps4_quick_menu_update(void);

// The port's own settings, /data/Conker/ps4.ini ("key=number" lines): the quick menu's switches.
int ps4_setting_get(const char *key, int fallback);
void ps4_setting_set(const char *key, int value);

// Threads (ps4_platform.cpp). The PS4 schedules by priority (256 highest, 767 lowest; threads start
// at 700) on the app's 7 cores (0-6). Called where a thread names itself (ultramodern, RT64):
//   ps4_thread_name: the name in crash and freeze reports
//   ps4_thread_role: its priority, and for PS4_ROLE_BACKGROUND cores 4-6 only (shader compiles:
//                    they take spare time and can never hold up the game or the renderer)
enum {
    PS4_ROLE_BACKGROUND = -2,
    PS4_ROLE_LOW = -1,
    PS4_ROLE_NORMAL = 0,
    PS4_ROLE_HIGH = 1,
    PS4_ROLE_HIGHER = 2,
    PS4_ROLE_HIGHEST = 3,
};
void ps4_thread_name(const char *name);
void ps4_thread_role(int role);

// The performance log (ps4_perf.cpp; quick menu > Performance Log, on by default): every 5 s a
// line in conker.log with each stage's rate, wall and CPU time per item, and the shader queue.
// The stages time themselves: ps4_perf_begin/end around one item, on the thread doing it.
enum {
    PS4_PERF_DISPLAY_LIST, // ultramodern's Gfx Thread: a frame's display list handed to RT64
    PS4_PERF_AUDIO,        // SP Task Thread: an audio (RSP) task
    PS4_PERF_WORKLOAD,     // RT64 Workload: a frame drawn
    PS4_PERF_PRESENT,      // RT64 Present: a picture shown
    PS4_PERF_SHADER,       // RT64 Shader: a specialized pipeline compiled
    PS4_PERF_UI,           // the main thread's frame (SDL events, the menus)
    PS4_PERF_VI,           // the VI thread's tick (60 a second when the clock keeps up)
    PS4_PERF_COUNT
};
void ps4_perf_begin(int stage);
void ps4_perf_end(int stage);
// Shaders waiting to be compiled (RT64's queue, after each one taken)
void ps4_perf_shader_queue(int waiting);
int ps4_perf_enabled(void);
void ps4_perf_set_enabled(int on);

// The hot-spot profiler (port/ps4_sampler.cpp; quick menu > Profiler, ps4.ini profiler, on by default
// while the port is slow): profile.log every 10 s, each running thread's busiest code.
void ps4_hot_profiler_set_enabled(int on);
int ps4_hot_profiler_enabled(void);

#ifdef __cplusplus
}
#endif
