// Conker's Bad Fur Day: Recompiled for PS4: the port's log. conker.log holds this run,
// conker.prev.log the one before (a crash report is often about the run before the last).

#include "ps4_log.h"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

#include <unistd.h>

extern "C" const char *ps4_data_dir(); // port/ps4_crashlog.cpp

namespace {
std::mutex log_mutex;
const auto start = std::chrono::steady_clock::now();
}

void ps4_log_init() {
    const std::string dir = ps4_data_dir();
    const std::string path = dir + "/conker.log";
    std::rename(path.c_str(), (dir + "/conker.prev.log").c_str());
    // stdout and stderr into the log, unbuffered: a crash mustn't lose the last lines
    if (std::freopen(path.c_str(), "w", stdout)) {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        dup2(fileno(stdout), fileno(stderr));
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
}

void ps4_log_line(const char *level, const std::string &text) {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const std::lock_guard lock(log_mutex);
    std::fprintf(stdout, "[%9.3f] %s %s\n", seconds, level, text.c_str());
}
