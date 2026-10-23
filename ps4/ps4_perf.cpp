// Conker's Bad Fur Day: Recompiled for PS4: the performance log.
//
// Timers, not sampling: the signal-based sampler (port/ps4_sampler.cpp) froze SMW's game thread
// in a libkernel wait, so the stages that matter time themselves. Every 5 s, while it's on, one
// line in conker.log:
//   perf: game 14.8 fps | workload 14.8/s 41.2 ms (cpu 38.0) | present 14.8/s 2.1 ms (cpu 1.9) |
//         audio 30.0/s 1.2 ms | ui 59.8/s 3.3 ms | shaders 4 built, 812 ms each, 23 waiting | cpu 3.1 of 7 cores
// "game" is the display lists the game sent (its own frame rate), the others are per item: wall
// time (waits included) and the thread's CPU time within it.

#include "ps4_platform.h"
#include "ps4_log.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <thread>

#include <time.h>

namespace {
constexpr clockid_t THREAD_CPU_CLOCK = 14;  // FreeBSD CLOCK_THREAD_CPUTIME_ID
constexpr clockid_t PROCESS_CPU_CLOCK = 15; // FreeBSD CLOCK_PROCESS_CPUTIME_ID

struct Stage {
    std::atomic<uint64_t> count{ 0 };
    std::atomic<uint64_t> wall_ns{ 0 };
    std::atomic<uint64_t> cpu_ns{ 0 };
};
Stage stages[PS4_PERF_COUNT];
std::atomic<int> shader_waiting{ 0 };
std::atomic<bool> enabled{ false };
std::atomic<bool> reporter_started{ false };

// the item each thread is timing (a thread times one stage at a time)
thread_local uint64_t begin_wall = 0;
thread_local uint64_t begin_cpu = 0;

uint64_t read_clock(clockid_t clock) {
    timespec ts{};
    if (clock_gettime(clock, &ts) != 0)
        return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

void reporter() {
    ps4_thread_name("perf log");
    ps4_thread_role(PS4_ROLE_LOW);
    const char *names[PS4_PERF_COUNT] = { "game", "audio", "workload", "present", "shaders", "ui" };
    uint64_t last[PS4_PERF_COUNT][3] = {};
    uint64_t last_wall = read_clock(CLOCK_MONOTONIC);
    uint64_t last_process = read_clock(PROCESS_CPU_CLOCK);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (!enabled.load(std::memory_order_relaxed)) {
            last_wall = read_clock(CLOCK_MONOTONIC);
            last_process = read_clock(PROCESS_CPU_CLOCK);
            for (int i = 0; i < PS4_PERF_COUNT; i++) {
                last[i][0] = stages[i].count.load();
                last[i][1] = stages[i].wall_ns.load();
                last[i][2] = stages[i].cpu_ns.load();
            }
            continue;
        }
        const uint64_t wall = read_clock(CLOCK_MONOTONIC);
        const double seconds = (wall - last_wall) / 1e9;
        last_wall = wall;
        std::string line = "perf:";
        for (int i = 0; i < PS4_PERF_COUNT; i++) {
            const uint64_t count = stages[i].count.load(), wall_ns = stages[i].wall_ns.load(), cpu_ns = stages[i].cpu_ns.load();
            const uint64_t n = count - last[i][0];
            const double ms = n ? (wall_ns - last[i][1]) / 1e6 / n : 0.0;
            const double cpu_ms = n ? (cpu_ns - last[i][2]) / 1e6 / n : 0.0;
            last[i][0] = count;
            last[i][1] = wall_ns;
            last[i][2] = cpu_ns;
            if (i == PS4_PERF_DISPLAY_LIST)
                line += std::format(" game {:.1f} fps", n / seconds);
            else if (i == PS4_PERF_SHADER)
                line += std::format(" | shaders {} built, {:.0f} ms each, {} waiting", n, ms, shader_waiting.load());
            else
                line += std::format(" | {} {:.1f}/s {:.1f} ms (cpu {:.1f})", names[i], n / seconds, ms, cpu_ms);
        }
        const uint64_t process = read_clock(PROCESS_CPU_CLOCK);
        if (process != 0 && last_process != 0)
            line += std::format(" | cpu {:.1f} of 7 cores", (process - last_process) / 1e9 / seconds);
        last_process = process;
        LOG_INFO("{}", line);
    }
}
} // namespace

extern "C" void ps4_perf_begin(int stage) {
    (void)stage;
    if (!enabled.load(std::memory_order_relaxed))
        return;
    begin_wall = read_clock(CLOCK_MONOTONIC);
    begin_cpu = read_clock(THREAD_CPU_CLOCK);
}

extern "C" void ps4_perf_end(int stage) {
    if (stage < 0 || stage >= PS4_PERF_COUNT || begin_wall == 0)
        return;
    const uint64_t wall = read_clock(CLOCK_MONOTONIC), cpu = read_clock(THREAD_CPU_CLOCK);
    Stage &s = stages[stage];
    s.count.fetch_add(1, std::memory_order_relaxed);
    s.wall_ns.fetch_add(wall - begin_wall, std::memory_order_relaxed);
    if (cpu >= begin_cpu)
        s.cpu_ns.fetch_add(cpu - begin_cpu, std::memory_order_relaxed);
    begin_wall = 0;
}

extern "C" void ps4_perf_shader_queue(int waiting) {
    shader_waiting.store(waiting, std::memory_order_relaxed);
}

extern "C" int ps4_perf_enabled(void) {
    return enabled.load() ? 1 : 0;
}

extern "C" void ps4_perf_set_enabled(int on) {
    enabled.store(on != 0);
    if (on && !reporter_started.exchange(true))
        std::thread(reporter).detach();
}
