#include "Core/StringUtil.h"

#include <Windows.h>

namespace dungeon::str {

std::wstring Widen(std::string_view utf8) {
	if (utf8.empty()) return {};
	const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
										static_cast<int>(utf8.size()), nullptr, 0);
	std::wstring out(static_cast<size_t>(len), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
						out.data(), len);
	return out;
}

std::string Narrow(std::wstring_view wide) {
	if (wide.empty()) return {};
	const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
										static_cast<int>(wide.size()), nullptr, 0,
										nullptr, nullptr);
	std::string out(static_cast<size_t>(len), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
						out.data(), len, nullptr, nullptr);
	return out;
}

} // namespace dungeon::str
