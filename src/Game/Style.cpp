// ============================================================================
// Game/Style.cpp - see Style.h.
// ============================================================================
#include "Game/Style.h"

#include <charconv>
#include <format>

namespace dungeon::game::style {

namespace {
std::string_view Trim(std::string_view s) {
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
	return s;
}
} // namespace

std::vector<Pick> ParseMonsters(std::string_view text) {
	std::vector<Pick> out;
	while (!text.empty()) {
		const size_t comma = text.find(',');
		std::string_view part = Trim(text.substr(0, comma));
		text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
		if (part.empty()) continue;
		Pick p;
		const size_t space = part.find_first_of(" \t");
		p.id = std::string(part.substr(0, space));
		if (space != std::string_view::npos) {
			const std::string_view w = Trim(part.substr(space));
			float v = 1.0f;
			const auto [end, ec] = std::from_chars(w.data(), w.data() + w.size(), v);
			p.weight = ec == std::errc{} && end == w.data() + w.size() ? v : 1.0f;
		}
		if (p.weight <= 0.0f) continue;
		bool seen = false;
		for (const Pick& q : out) seen = seen || q.id == p.id;
		if (!seen) out.push_back(std::move(p));
	}
	return out;
}

std::string FormatMonsters(const std::vector<Pick>& picks) {
	std::string out;
	for (const Pick& p : picks) {
		if (!out.empty()) out += ", ";
		out += p.id;
		if (p.weight != 1.0f) out += std::format(" {:g}", p.weight);
	}
	return out;
}

int RenameMonster(std::vector<Pick>& picks, std::string_view from, std::string_view to) {
	int hits = 0;
	for (Pick& p : picks)
		if (p.id == from) {
			p.id = std::string(to);
			++hits;
		}
	return hits;
}

} // namespace dungeon::game::style
