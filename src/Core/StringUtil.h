#pragma once

#include <string>
#include <string_view>

namespace dungeon::str {

// UTF-8 <-> UTF-16 conversions for the Win32 boundary.
std::wstring Widen(std::string_view utf8);
std::string Narrow(std::wstring_view wide);

} // namespace dungeon::str
