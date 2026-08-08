// ============================================================================
// Core/Log.h — leveled logging with std::format-style messages.
//
// Usage:  log::Info("loaded {} meshes", count);
// Output goes to stdout/stderr (visible in the debug console) and to
// OutputDebugString (visible in the Visual Studio Output window).
// Thread-safe; cheap enough for load-time chatter, too hot for per-frame use.
//
// ENCODING: messages are UTF-8, because that is what std::format hands back for
// the source literals we write (an em-dash in a log line is an em-dash). The
// FILE and OutputDebugString take that correctly. A Windows CONSOLE does not:
// it decodes bytes in its output code page, which defaults to the OEM one (437
// here), so "—" arrives as "ΓÇö". UseUtf8Console tells it otherwise, and every
// entry point that shows a console calls it.
// ============================================================================
#pragma once

#include <format>
#include <string_view>

namespace dungeon::log {

enum class Level { Debug, Info, Warn, Error };

void Write(Level level, std::string_view message);

// Sets the attached console's output code page to UTF-8. Call once from an
// entry point, before any logging. Deliberately NOT done inside Write: a tool
// run from someone's shell inherits THEIR console, and a logging call quietly
// mutating it is the kind of side effect that is impossible to trace back. An
// entry point choosing it for its own output is a decision; a library doing it
// behind your back is a surprise.
void UseUtf8Console();

template <typename... Args>
void Debug(std::format_string<Args...> fmt, Args&&... args) {
	Write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void Info(std::format_string<Args...> fmt, Args&&... args) {
	Write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void Warn(std::format_string<Args...> fmt, Args&&... args) {
	Write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void Error(std::format_string<Args...> fmt, Args&&... args) {
	Write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace dungeon::log
