#include "Core/Log.h"

#include "Core/StringUtil.h"

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <mutex>

namespace dungeon::log {

namespace {
std::mutex g_mutex;

const char* Prefix(Level level) {
    switch (level) {
    case Level::Debug: return "[debug] ";
    case Level::Info:  return "[info ] ";
    case Level::Warn:  return "[warn ] ";
    case Level::Error: return "[ERROR] ";
    }
    return "";
}
} // namespace

void Write(Level level, std::string_view message) {
    std::lock_guard lock(g_mutex);
    std::string line = Prefix(level);
    line.append(message);
    line.push_back('\n');
    std::fputs(line.c_str(), level >= Level::Warn ? stderr : stdout);
    OutputDebugStringW(str::Widen(line).c_str());
}

} // namespace dungeon::log
