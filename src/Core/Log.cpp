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

// The file sink: <exename>.log next to the exe, truncated per run. Opened
// lazily on the first Write (under g_mutex) and flushed per line, so the
// tail survives a crash/abort — the whole point of a debugging log. RAII
// like every C-API boundary; process exit closes it either way.
//
// Named after the RUNNING EXE, not hardcoded to dungeon.log. Every tool links
// Core and so shares this sink, and they all sit in the same build\<cfg>\bin —
// so with a fixed name an AssetBaker run silently truncated the GAME's log and
// wrote its own output over it. That destroyed the evidence mid-debug once
// (2026-08-05), and CLAUDE.md points at dungeon.log as THE diagnostic.
// The game's file keeps its documented name; the tools get assetbaker.log,
// bc7test.log, threadstress.log.
FILE* LogFile() {
	static const std::unique_ptr<FILE, decltype(&fclose)> file(
		fopen((paths::ExecutableDir() + "\\" + paths::ExecutableName() + ".log").c_str(),
			  "w"),
		&fclose);
	return file.get();
}
} // namespace

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
