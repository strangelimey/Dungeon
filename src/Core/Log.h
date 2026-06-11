// ============================================================================
// Core/Log.h — leveled logging with std::format-style messages.
//
// Usage:  log::Info("loaded {} meshes", count);
// Output goes to stdout/stderr (visible in the debug console) and to
// OutputDebugString (visible in the Visual Studio Output window).
// Thread-safe; cheap enough for load-time chatter, too hot for per-frame use.
// ============================================================================
#pragma once

#include <format>
#include <string_view>

namespace dungeon::log {

enum class Level { Debug, Info, Warn, Error };

void Write(Level level, std::string_view message);

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
