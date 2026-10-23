// Conker's Bad Fur Day: Recompiled for PS4: the port's log. conker.log holds this run,
// conker.prev.log the one before (a crash report is often about the run before the last).

#include "ps4_log.h"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

#include <fcntl.h>
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
    // stdout and stderr into the log, unbuffered: a crash mustn't lose the last lines. Not freopen:
    // the C library's goes through dup3, a stub on the PS4, and closed stdout instead (v00.01).
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
}

void ps4_log_line(const char *level, const std::string &text) {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    // one write per line: stdout is unbuffered, and RADV's and the runtime's printfs come from other threads
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "[%9.3f] %s ", seconds, level);
    const std::string line = prefix + text + "\n";
    const std::lock_guard lock(log_mutex);
    std::fwrite(line.data(), 1, line.size(), stdout);
}
