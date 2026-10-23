// Conker's Bad Fur Day: Recompiled for PS4: the port's log (/data/Conker/conker.log).
// stdout and stderr go to the same file (ps4_log_init), so the runtime's, RT64's and the host's
// own printf lines land there too.
#pragma once

#include <format>
#include <string>

void ps4_log_init();
void ps4_log_line(const char *level, const std::string &text);

#define PS4_LOG(level, ...) ps4_log_line(level, std::format(__VA_ARGS__))
#define LOG_INFO(...) PS4_LOG("I", __VA_ARGS__)
#define LOG_WARN(...) PS4_LOG("W", __VA_ARGS__)
#define LOG_ERROR(...) PS4_LOG("E", __VA_ARGS__)
