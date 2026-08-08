#include "Core/Log.h"

#include "Core/Paths.h"
#include "Core/StringUtil.h"

#include <Windows.h>

#include <cstdio>
#include <memory>
#include <mutex>
#include <print>

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

// The file sink: dungeon.log next to the exe, truncated per run. Opened
// lazily on the first Write (under g_mutex) and flushed per line, so the
// tail survives a crash/abort — the whole point of a debugging log. RAII
// like every C-API boundary; process exit closes it either way.
FILE* LogFile() {
	static const std::unique_ptr<FILE, decltype(&fclose)> file(
		fopen((paths::ExecutableDir() + "\\dungeon.log").c_str(), "w"),
		&fclose);
	return file.get();
}
} // namespace

void UseUtf8Console() { SetConsoleOutputCP(CP_UTF8); }

void Write(Level level, std::string_view message) {
	std::lock_guard lock(g_mutex);
	std::string line = Prefix(level);
	line.append(message);
	line.push_back('\n');
	std::print(level >= Level::Warn ? stderr : stdout, "{}", line);
	OutputDebugStringW(str::Widen(line).c_str());
	if (FILE* f = LogFile()) {
		std::fwrite(line.data(), 1, line.size(), f);
		std::fflush(f);
	}
}

} // namespace dungeon::log
