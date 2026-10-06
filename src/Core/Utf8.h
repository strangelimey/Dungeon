// ============================================================================
// Core/Utf8.h - walking, counting and building UTF-8 text, kept PURE.
//
// Every string the engine shows is UTF-8: what Font draws, what a .lang file
// holds, what a save writes - and since code-review C383, what the keyboard
// types (Input::TypedChars). Anything that EDITS text therefore has to work in
// whole characters. A Backspace that pops one byte of a two-byte letter leaves
// a stray lead byte the font draws as '?', and a length limit counted in bytes
// stops a Russian name at half the letters an English one gets.
//
// Header-only and Windows-free, so RollTest checks it directly and the party
// rules (Game/PartyRules.h) count with it. str::Widen / str::Narrow
// (Core/StringUtil.h) are the Win32 boundary; this is everything after it.
//
// MALFORMED TEXT IS WALKED, NEVER TRUSTED: a byte no sequence starts with, or a
// sequence cut short, is one character of its own, so a walk always advances
// and a bad byte never swallows the good character after it. Valid() is the
// strict test, for text about to be kept (a name).
// ============================================================================
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace dungeon::utf8 {

// Whether `b` continues a sequence (10xxxxxx) rather than starting one.
constexpr bool IsContinuation(unsigned char b) { return (b & 0xC0) == 0x80; }

// How many bytes the sequence lead byte `b` starts: 1 for ASCII AND for any
// byte no sequence starts with (a continuation out of place, 0xF8..0xFF).
constexpr size_t SequenceLength(unsigned char b) {
	if (b < 0xC0) return 1;
	if (b < 0xE0) return 2;
	if (b < 0xF0) return 3;
	if (b < 0xF8) return 4;
	return 1;
}

// The whole character at byte `i` of `s` (i < s.size()): the bytes its lead
// byte announces, cut short where one that should continue it does not.
constexpr std::string_view CharAt(std::string_view s, size_t i) {
	const size_t want = SequenceLength(static_cast<unsigned char>(s[i]));
	size_t n = 1;
	while (n < want && i + n < s.size() && IsContinuation(static_cast<unsigned char>(s[i + n])))
		++n;
	return s.substr(i, n);
}

// How many characters `s` holds, by the CharAt walk.
constexpr size_t Length(std::string_view s) {
	size_t n = 0;
	for (size_t i = 0; i < s.size(); i += CharAt(s, i).size()) ++n;
	return n;
}

// The first `count` characters of `s` (all of it when it holds fewer) - a cut
// that never lands inside a character.
constexpr std::string_view Prefix(std::string_view s, size_t count) {
	size_t i = 0;
	for (; i < s.size() && count > 0; --count) i += CharAt(s, i).size();
	return s.substr(0, i);
}

// Where the last character of `s` starts - the same character the CharAt walk
// would end on, malformed or not. 0 for empty text.
constexpr size_t LastCharStart(std::string_view s) {
	if (s.empty()) return 0;
	size_t p = s.size() - 1;
	for (int k = 0; k < 3 && p > 0 && IsContinuation(static_cast<unsigned char>(s[p])); ++k) --p;
	return CharAt(s, p).size() == s.size() - p ? p : s.size() - 1;
}

// Removes the last whole character - Backspace. False when `s` was empty.
// Allocation-free (erase keeps the capacity).
inline bool PopBack(std::string& s) {
	if (s.empty()) return false;
	s.erase(LastCharStart(s));
	return true;
}

// Appends the UTF-8 encoding of code point `cp`. A surrogate half or a value
// past U+10FFFF is not a character: nothing is appended and it returns false.
inline bool Append(std::string& out, char32_t cp) {
	if (cp < 0x80) {
		out.push_back(static_cast<char>(cp));
	} else if (cp < 0x800) {
		out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else if (cp >= 0xD800 && cp <= 0xDFFF) {
		return false;
	} else if (cp < 0x10000) {
		out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else if (cp <= 0x10FFFF) {
		out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else {
		return false;
	}
	return true;
}

// Whether `s` is well-formed UTF-8: every sequence whole, none in an overlong
// form, no surrogate half and nothing past U+10FFFF.
constexpr bool Valid(std::string_view s) {
	constexpr char32_t kLeast[5] = {0, 0, 0x80, 0x800, 0x10000}; // by sequence length
	for (size_t i = 0; i < s.size();) {
		const unsigned char lead = static_cast<unsigned char>(s[i]);
		const size_t n = SequenceLength(lead);
		if (n == 1) {
			if (lead >= 0x80) return false;
			++i;
			continue;
		}
		if (i + n > s.size()) return false;
		char32_t cp = lead & (0x7Fu >> n);
		for (size_t k = 1; k < n; ++k) {
			const unsigned char c = static_cast<unsigned char>(s[i + k]);
			if (!IsContinuation(c)) return false;
			cp = (cp << 6) | (c & 0x3Fu);
		}
		if (cp < kLeast[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
		i += n;
	}
	return true;
}

} // namespace dungeon::utf8
