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
//
// REPORTING EXCUSES ITSELF, HERE (code-review C215). A log line formats a
// std::string, and the steady-state allocation guard (Core/AllocTrack) counts
// that like any allocation - so every reporter used to have to wrap itself in
// alloc::Excused, several did not, and a warning in a guarded frame (a typo'd
// on_hit id, on every blow) failed the guard or aborted it under strict. Write
// and the four templates below hold the excuse themselves, around the
// formatting too. What a caller builds BEFORE the call - an argument it formats,
// a console line - is still its own to excuse.
// ============================================================================
#pragma once

#include "Core/AllocTrack.h"

#include <format>
#include <string>
#include <string_view>

namespace dungeon::log {

enum class Level { Debug, Info, Warn, Error };

// Excuses its own allocations (see the banner).
void Write(Level level, std::string_view message);

// The file this process logs to: <exe dir>\<exe name>.log (dungeon.log for the
// game, diagtest.log for DiagTest). A tool that reads its own log back asks here
// rather than rebuilding the path, so the two cannot drift apart. The file is
// opened by the first Write, so before that it may not exist yet.
const std::string& FilePath();

// Sets the attached console's output code page to UTF-8. Call once from an
// entry point, before any logging. Deliberately NOT done inside Write: a tool
// run from someone's shell inherits THEIR console, and a logging call quietly
// mutating it is the kind of side effect that is impossible to trace back. An
// entry point choosing it for its own output is a decision; a library doing it
// behind your back is a surprise.
void UseUtf8Console();

// Each holds an alloc::Excused over the formatting as well as the write (see the
// banner): the string is built before Write's own excuse begins.
template <typename... Args>
void Debug(std::format_string<Args...> fmt, Args&&... args) {
	const alloc::Excused excuse;
	Write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void Info(std::format_string<Args...> fmt, Args&&... args) {
	const alloc::Excused excuse;
	Write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void Warn(std::format_string<Args...> fmt, Args&&... args) {
	const alloc::Excused excuse;
	Write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void Error(std::format_string<Args...> fmt, Args&&... args) {
	const alloc::Excused excuse;
	Write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace dungeon::log
