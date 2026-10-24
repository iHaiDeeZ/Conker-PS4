// Conker's Bad Fur Day: Recompiled for PS4: the performance log.
//
// Timers, not sampling: the signal-based sampler (port/ps4_sampler.cpp) froze SMW's game thread
// in a libkernel wait, so the stages that matter time themselves. Every 5 s, while it's on, one
// line in conker.log:
//   perf: game 14.8 fps | workload 14.8/s 41.2 ms (cpu 38.0) | present 14.8/s 2.1 ms (cpu 1.9) |
//         audio 30.0/s 1.2 ms | ui 59.8/s 3.3 ms | shaders 4 built, 812 ms each, 23 waiting | cpu 3.1 of 7 cores
// "game" is the display lists the game sent (its own frame rate), the others are per item: wall
// time (waits included) and the thread's CPU time within it.
//
// With it comes the freeze watchdog: when the VI tick or the game's frames stop for 10 s, every
// thread's place and stack go to freeze.log, twice 3 s apart (spinning or stuck), once per freeze.
// It allocates nothing and logs nothing (a freeze can be a lock the log or the heap needs).

#include "ps4_platform.h"
#include "ps4_log.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <thread>

#include <cstdio>

#include <time.h>

extern "C" void ps4_dump_thread_stacks(const char *path, const char *reason); // port/ps4_sampler.cpp

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
Stage parts[PS4_PERF_RT64_PART_COUNT];
std::atomic<uint64_t> values[PS4_PERF_RT64_VALUE_COUNT];
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
    const char *names[PS4_PERF_COUNT] = { "game", "audio", "workload", "present", "shaders", "ui", "vi" };
    uint64_t last[PS4_PERF_COUNT][3] = {};
    uint64_t last_parts[PS4_PERF_RT64_PART_COUNT][2] = {};
    uint64_t last_values[PS4_PERF_RT64_VALUE_COUNT] = {};
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

        // RT64's frame in parts, per frame drawn
        uint64_t now_values[PS4_PERF_RT64_VALUE_COUNT];
        for (int i = 0; i < PS4_PERF_RT64_VALUE_COUNT; i++)
            now_values[i] = values[i].load();
        const uint64_t frames = now_values[PS4_PERF_RT64_FRAMES] - last_values[PS4_PERF_RT64_FRAMES];
        if (frames > 0) {
            const char *part_names[PS4_PERF_RT64_PART_COUNT] = { "rsp", "setup", "record", "submit+wait" };
            std::string rt64 = std::format("rt64: {:.1f} frames/s, {:.0f} draws and {:.1f} framebuffer pairs each |",
                frames / seconds, double(now_values[PS4_PERF_RT64_DRAWS] - last_values[PS4_PERF_RT64_DRAWS]) / frames,
                double(now_values[PS4_PERF_RT64_PAIRS] - last_values[PS4_PERF_RT64_PAIRS]) / frames);
            for (int i = 0; i < PS4_PERF_RT64_PART_COUNT; i++) {
                const uint64_t wall_ns = parts[i].wall_ns.load(), cpu_ns = parts[i].cpu_ns.load();
                rt64 += std::format(" {} {:.1f} ms (cpu {:.1f})", part_names[i], (wall_ns - last_parts[i][0]) / 1e6 / frames, (cpu_ns - last_parts[i][1]) / 1e6 / frames);
                last_parts[i][0] = wall_ns;
                last_parts[i][1] = cpu_ns;
            }
            rt64 += std::format(" | gpu {:.1f} ms", (now_values[PS4_PERF_RT64_GPU_NS] - last_values[PS4_PERF_RT64_GPU_NS]) / 1e6 / frames);
            LOG_INFO("{}", rt64);
        }
        for (int i = 0; i < PS4_PERF_RT64_VALUE_COUNT; i++)
            last_values[i] = now_values[i];
    }
}
void watchdog() {
    ps4_thread_name("freeze watchdog");
    char path[128], previous[128];
    std::snprintf(path, sizeof(path), "%s/freeze.log", ps4_data_dir());
    std::snprintf(previous, sizeof(previous), "%s/freeze.prev.log", ps4_data_dir());
    std::rename(path, previous);
    uint64_t last_vi = 0, last_frames = 0;
    int vi_still = 0, frames_still = 0;
    bool reported = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!enabled.load(std::memory_order_relaxed)) {
            vi_still = frames_still = 0;
            continue;
        }
        const uint64_t vi = stages[PS4_PERF_VI].count.load(), frames = stages[PS4_PERF_DISPLAY_LIST].count.load();
        vi_still = vi == last_vi ? vi_still + 1 : 0;
        frames_still = frames == last_frames ? frames_still + 1 : 0;
        last_vi = vi;
        last_frames = frames;
        // the game's first frame comes after its intro's loading
        const bool frozen = vi_still >= 10 || (frames != 0 && frames_still >= 10);
        if (frozen && !reported) {
            reported = true;
            char reason[128];
            std::snprintf(reason, sizeof(reason), "freeze: no VI tick for %d s, no game frame for %d s (first look)", vi_still, frames_still);
            ps4_dump_thread_stacks(path, reason);
            std::this_thread::sleep_for(std::chrono::seconds(3));
            std::snprintf(reason, sizeof(reason), "freeze: second look, 3 s later");
            ps4_dump_thread_stacks(path, reason);
        } else if (!frozen) {
            reported = false;
        }
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

extern "C" void ps4_perf_clock(uint64_t *wall, uint64_t *cpu) {
    const bool on = enabled.load(std::memory_order_relaxed);
    *wall = on ? read_clock(CLOCK_MONOTONIC) : 0;
    *cpu = on ? read_clock(THREAD_CPU_CLOCK) : 0;
}

extern "C" void ps4_perf_part(int part, uint64_t wall, uint64_t cpu) {
    if (part < 0 || part >= PS4_PERF_RT64_PART_COUNT || wall == 0)
        return;
    const uint64_t now_wall = read_clock(CLOCK_MONOTONIC), now_cpu = read_clock(THREAD_CPU_CLOCK);
    parts[part].wall_ns.fetch_add(now_wall - wall, std::memory_order_relaxed);
    if (now_cpu >= cpu)
        parts[part].cpu_ns.fetch_add(now_cpu - cpu, std::memory_order_relaxed);
}

extern "C" void ps4_perf_value(int value, uint64_t amount) {
    if (value >= 0 && value < PS4_PERF_RT64_VALUE_COUNT && enabled.load(std::memory_order_relaxed))
        values[value].fetch_add(amount, std::memory_order_relaxed);
}

extern "C" int ps4_perf_enabled(void) {
    return enabled.load() ? 1 : 0;
}

extern "C" void ps4_perf_set_enabled(int on) {
    enabled.store(on != 0);
    if (on && !reporter_started.exchange(true)) {
        std::thread(reporter).detach();
        std::thread(watchdog).detach();
    }
}
