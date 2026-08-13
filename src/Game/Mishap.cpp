// ============================================================================
// Game/Mishap.cpp — see Mishap.h.
// ============================================================================
#include "Game/Mishap.h"

#include "Core/Log.h"

#include <array>
#include <cctype>
#include <cstdlib>

namespace dungeon::game::mishap {

namespace {
// The one place a token and its Kind are paired. Both directions read this, so
// they cannot drift — a Kind added without a token here fails to compile out of
// the switch-free lookups below rather than silently parsing as nothing.
struct Named {
	std::string_view token;
	Kind kind;
};
constexpr std::array<Named, 6> kTokens{{
	{"recover", Kind::Recover},
	{"stumble", Kind::Stumble},
	{"drop", Kind::Drop},
	{"fling", Kind::Fling},
	{"self_hit", Kind::SelfHit},
	{"wild", Kind::Wild},
}};
} // namespace

bool KindFromToken(std::string_view token, Kind& out) {
	for (const Named& n : kTokens)
		if (n.token == token) {
			out = n.kind;
			return true;
		}
	return false;
}

std::string_view TokenFor(Kind kind) {
	for (const Named& n : kTokens)
		if (n.kind == kind) return n.token;
	return "?";
}

void Parse(std::string_view spec, std::vector<Entry>& out,
		   std::string_view where) {
	size_t start = 0;
	while (start < spec.size()) {
		size_t end = spec.find_first_of(",;", start);
		if (end == std::string_view::npos) end = spec.size();
		const std::string_view entry = spec.substr(start, end - start);
		start = end + 1;
		// Split on whitespace: <token> [value]. Same shape as ParseProcs, on
		// purpose — an author who has written one has written the other.
		std::array<std::string_view, 2> token{};
		size_t n = 0, i = 0;
		while (i < entry.size() && n < token.size()) {
			while (i < entry.size() &&
				   std::isspace(static_cast<unsigned char>(entry[i])))
				++i;
			const size_t from = i;
			while (i < entry.size() &&
				   !std::isspace(static_cast<unsigned char>(entry[i])))
				++i;
			if (i > from) token[n++] = entry.substr(from, i - from);
		}
		if (n == 0) continue; // blank entry (a trailing comma) — not an error
		Entry e;
		if (!KindFromToken(token[0], e.kind)) {
			// REPORTED, never guessed. A mistyped consequence that fell back to
			// something harmless would leave a fumble table that looks authored
			// and does something else.
			log::Warn("{}: unknown fumble consequence '{}'", where, token[0]);
			continue;
		}
		if (n >= 2) e.value = std::strtof(std::string(token[1]).c_str(), nullptr);
		// The three that TAKE a number are useless without one, and an author
		// who omitted it meant something. The valueless three ignore it.
		if (n < 2 && (e.kind == Kind::Recover || e.kind == Kind::Stumble ||
					  e.kind == Kind::SelfHit)) {
			log::Warn("{}: fumble consequence '{}' needs a value", where,
					  token[0]);
			continue;
		}
		out.push_back(e);
	}
}

bool Severe(int face, int severeFace) {
	// Face 0 is "nothing was recorded" (the field's default), NOT a very bad
	// roll. Without this guard every non-fumbling event would read as severe
	// for any knob >= 0.
	return face > 0 && face <= severeFace;
}

std::vector<Entry> DefaultFumble(float recoverMul) {
	// The tempo consequence, and only that: a fumble is a 5%-per-swing event,
	// so what happens on MOST of them has to be survivable enough to shrug at.
	return {Entry{Kind::Recover, recoverMul}};
}

std::vector<Entry> DefaultSevere() {
	// ...and the bottom of the band puts your weapon on the floor. A no-op for
	// anything swinging bare hands or claws, which is what lets monsters and an
	// unarmed member share this table harmlessly.
	return {Entry{Kind::Drop, 0.0f}};
}

} // namespace dungeon::game::mishap
