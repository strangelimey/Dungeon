// ============================================================================
// UI/TextWrap.h - greedy word wrap, shared. Lifted out of the character sheet
// (it was file-local there) when the editor map's validation tooltip needed
// the same thing: one wrap, so a line measured in one place cannot break
// differently in another.
// ============================================================================
#pragma once

#include "UI/Font.h"

#include <string_view>

namespace dungeon::ui {

// Word-wraps `text` to `maxW`, handing each line and its index to `sink`, and
// returns the line count. string_view slices only - no per-frame allocation.
// Measuring a block and drawing it walk the same function, so text can never
// be measured one height and drawn another. A single word wider than `maxW`
// is kept whole on its own line rather than broken.
template <typename Sink>
int WrapLines(const Font& font, std::string_view text, float maxW, Sink&& sink) {
	int lines = 0;
	while (!text.empty()) {
		size_t end = text.size();
		while (end > 0 && font.MeasureWidth(text.substr(0, end)) > maxW) {
			const size_t space = text.rfind(' ', end - 1);
			if (space == std::string_view::npos || space == 0) break;
			end = space;
		}
		sink(text.substr(0, end), lines);
		++lines;
		const size_t next = text.find_first_not_of(' ', end);
		text = next == std::string_view::npos ? std::string_view{} : text.substr(next);
	}
	return lines;
}

} // namespace dungeon::ui
