// Conker's Bad Fur Day: Recompiled for PS4: startup, game memory.

#include "ps4_platform.h"
#include "ps4_log.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <string>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unwind.h>

extern "C" {
// Declared by hand: several OpenOrbis prototypes are wrong (as in HENkaku's mem.cpp).
size_t sceKernelGetDirectMemorySize();
int32_t sceKernelAllocateDirectMemory(off_t search_start, off_t search_end, size_t length, size_t alignment, int32_t memory_type, off_t *physical_out);
int32_t sceKernelMapDirectMemory(void **address, size_t length, int32_t protection, int32_t flags, off_t direct_memory_start, size_t alignment);
int32_t sceKernelReserveVirtualRange(void **address, size_t length, int32_t flags, size_t alignment);
int32_t sceKernelMunmap(void *address, size_t length);
int32_t sceUserServiceInitialize(void *params);
void ps4_boot_trace(const char *stage); // port/ps4_crashlog.cpp
}

namespace {
constexpr size_t REGION = 2u << 20;      // backed 2 MiB at a time, 2 MiB aligned (one large page)
constexpr int32_t WB_ONION = 0;          // SCE_KERNEL_WB_ONION
constexpr int32_t MAP_FIXED_FLAG = 0x10; // SCE_KERNEL_MAP_FIXED
constexpr int32_t PROT_CPU_RW = 0x3;

std::mutex vm_mutex;
uint8_t *vm_base = nullptr;
size_t vm_size = 0;
std::vector<bool> region_backed;
size_t backed_total = 0;
} // namespace

extern "C" int ps4_backtrace(void **frames, int max); // below

extern "C" void ps4_platform_init(void) {
    ps4_boot_trace("main");
    ps4_log_init();
    mkdir(ps4_data_dir(), 0777);
    // ScePad and sceAudioOut want the user service up
    sceUserServiceInitialize(nullptr);
    // the installed package's version (package-conker.sh writes it): an old install looks like a new bug
    char version[16] = "?";
    if (FILE *file = std::fopen("/app0/version.txt", "r")) {
        if (std::fgets(version, sizeof(version), file))
            version[std::strcspn(version, "\r\n")] = '\0';
        std::fclose(file);
    }
    LOG_INFO("Conker's Bad Fur Day: Recompiled for PS4 v{}, data in {}", version, ps4_data_dir());
    // the save states' backtrace must stop at the thread's entry (v00.08 didn't: the game crawled)
    std::thread([] {
        void *frames[64];
        const auto start = std::chrono::steady_clock::now();
        const int count = ps4_backtrace(frames, 64);
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        LOG_INFO("backtrace check: {} frames in {} us{}", count, us, us < 2000 ? "" : " (too slow: past the thread's entry?)");
    }).join();
    ps4_perf_set_enabled(ps4_setting_get("perf_log", 1));
    ps4_hot_profiler_set_enabled(ps4_setting_get("profiler", 1));
}

// ---- the port's settings: /data/Conker/ps4.ini, "key=number" lines, read once and rewritten whole
namespace {
std::mutex settings_mutex;
bool settings_loaded = false;
std::vector<std::pair<std::string, int>> settings;

std::string settings_path() {
    return std::string(ps4_data_dir()) + "/ps4.ini";
}

void load_settings() {
    if (settings_loaded)
        return;
    settings_loaded = true;
    FILE *file = std::fopen(settings_path().c_str(), "r");
    if (!file)
        return;
    char line[128];
    while (std::fgets(line, sizeof(line), file)) {
        char *equals = std::strchr(line, '=');
        if (!equals)
            continue;
        *equals = '\0';
        settings.emplace_back(line, std::atoi(equals + 1));
    }
    std::fclose(file);
}
} // namespace

extern "C" int ps4_setting_get(const char *key, int fallback) {
    const std::lock_guard lock(settings_mutex);
    load_settings();
    for (const auto &[name, value] : settings) {
        if (name == key)
            return value;
    }
    return fallback;
}

extern "C" void ps4_setting_set(const char *key, int value) {
    const std::lock_guard lock(settings_mutex);
    load_settings();
    bool found = false;
    for (auto &[name, current] : settings) {
        if (name == key) {
            current = value;
            found = true;
        }
    }
    if (!found)
        settings.emplace_back(key, value);
    const std::string path = settings_path(), part = path + ".part";
    FILE *file = std::fopen(part.c_str(), "w");
    if (!file)
        return;
    for (const auto &[name, current] : settings)
        std::fprintf(file, "%s=%d\n", name.c_str(), current);
    std::fclose(file);
    std::rename(part.c_str(), path.c_str());
}

// ---- threads
extern "C" {
void ps4_watch_thread(const char *name); // port/ps4_sampler.cpp: the name in crash and freeze reports
void *ps4_pthread_self() __asm__("scePthreadSelf");
int32_t ps4_pthread_set_prio(void *thread, int prio) __asm__("scePthreadSetprio");
int32_t ps4_pthread_set_affinity(void *thread, uint64_t mask) __asm__("scePthreadSetaffinity");
}

extern "C" void ps4_thread_name(const char *name) {
    ps4_watch_thread(name);
}

extern "C" void ps4_thread_role(int role) {
    void *self = ps4_pthread_self();
    int prio = 700;
    switch (role) {
    case PS4_ROLE_BACKGROUND:
        prio = 767;
        // cores 4-6: never in the way of the game's and the renderer's threads
        ps4_pthread_set_affinity(self, 0x70);
        break;
    case PS4_ROLE_LOW:
        prio = 720;
        break;
    case PS4_ROLE_HIGH:
        prio = 660;
        break;
    case PS4_ROLE_HIGHER:
        prio = 640;
        break;
    case PS4_ROLE_HIGHEST:
        prio = 620;
        break;
    default:
        break;
    }
    ps4_pthread_set_prio(self, prio);
}

// The crash log's app part (port/ps4_crashlog.cpp; create-fself needs it defined).
extern "C" void ps4_crash_guest_info() {}

// backtrace() for the runtime's save states (where each game thread waits, at every wait): the PS4's C
// library has no execinfo, so through the unwinder. It stops at the thread's outermost app frame
// (port/ps4_runtime.cpp: ps4_thread_entry): one step further is system code without unwind tables in
// the eboot, and libunwind looks for those by reading all of .eh_frame, uncached, every time (v00.07:
// the game ran at ~0.4 fps inside it). The frame is recognised by its function's start (the FDE
// libunwind already found for it); v00.08 compared stack addresses, which never matched.
extern "C" void *ps4_thread_entry(void *raw);

extern "C" int ps4_backtrace(void **frames, int max) {
    struct Walk {
        void **frames;
        int max;
        int count;
    } walk{ frames, max, 0 };
    _Unwind_Backtrace(
        [](_Unwind_Context *context, void *arg) -> _Unwind_Reason_Code {
            Walk &w = *static_cast<Walk *>(arg);
            if (w.count >= w.max)
                return _URC_END_OF_STACK;
            w.frames[w.count++] = reinterpret_cast<void *>(_Unwind_GetIP(context));
            if (_Unwind_GetRegionStart(context) == reinterpret_cast<uintptr_t>(&ps4_thread_entry))
                return _URC_END_OF_STACK;
            return _URC_NO_REASON;
        },
        &walk);
    return walk.count;
}

extern "C" void *ps4_vm_reserve(size_t size) {
    size = (size + REGION - 1) & ~(REGION - 1);
    // Above the kernel's own allocations at 0x200000000, away from the heap (0x1000000000) and
    // RADV's GPU windows (0x4000000000-0x4600000000). A hint whose range isn't free fails
    // instead of searching on, so try a few.
    for (const uintptr_t hint : { 0x2000000000ULL, 0x2400000000ULL, 0x3000000000ULL, 0x1800000000ULL, 0x800000000ULL }) {
        void *address = reinterpret_cast<void *>(hint);
        const int32_t ret = sceKernelReserveVirtualRange(&address, size, 0, REGION);
        if (ret == 0) {
            const std::lock_guard lock(vm_mutex);
            vm_base = static_cast<uint8_t *>(address);
            vm_size = size;
            region_backed.assign(size / REGION, false);
            LOG_INFO("PS4: {} MiB of game address space at {}", size >> 20, address);
            return address;
        }
        LOG_WARN("PS4: reserving {} MiB at 0x{:X} failed: {:08X}", size >> 20, hint, static_cast<uint32_t>(ret));
    }
    return nullptr;
}

extern "C" int ps4_vm_commit(void *address, size_t size) {
    const std::lock_guard lock(vm_mutex);
    uint8_t *begin = static_cast<uint8_t *>(address);
    if (vm_base == nullptr || begin < vm_base || begin + size > vm_base + vm_size) {
        LOG_ERROR("PS4: {} (+0x{:X}) is outside the game's address space", address, size);
        return -1;
    }
    const size_t first = (begin - vm_base) / REGION;
    const size_t last = (begin + size - vm_base + REGION - 1) / REGION;
    for (size_t region = first; region < last; region++) {
        if (region_backed[region])
            continue;
        off_t phys = 0;
        int32_t ret = sceKernelAllocateDirectMemory(0, static_cast<off_t>(sceKernelGetDirectMemorySize()), REGION, REGION, WB_ONION, &phys);
        if (ret != 0) {
            LOG_ERROR("PS4: out of direct memory ({} MiB of game memory backed, {:08X})", backed_total >> 20, static_cast<uint32_t>(ret));
            return -1;
        }
        uint8_t *const want = vm_base + region * REGION;
        void *at = want;
        ret = sceKernelMapDirectMemory(&at, REGION, PROT_CPU_RW, MAP_FIXED_FLAG, phys, REGION);
        if (ret != 0 || at != want) {
            // over the reservation failed: in its place
            sceKernelMunmap(want, REGION);
            at = want;
            ret = sceKernelMapDirectMemory(&at, REGION, PROT_CPU_RW, MAP_FIXED_FLAG, phys, REGION);
        }
        if (ret != 0 || at != want) {
            LOG_ERROR("PS4: mapping game memory at {} failed: {:08X}", static_cast<void *>(want), static_cast<uint32_t>(ret));
            return -1;
        }
        // direct memory comes as it was left: the game expects zeros (one thread, at startup)
        std::memset(want, 0, REGION);
        region_backed[region] = true;
        backed_total += REGION;
    }
    return 0;
}
