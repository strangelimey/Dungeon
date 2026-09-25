// ============================================================================
// Game/GenerateKnobs.cpp — see GenerateKnobs.h.
// ============================================================================
#include "Game/GenerateKnobs.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace dungeon::game::generate {

namespace {

constexpr const char* kShape = "map.gen.tab.shape";
constexpr const char* kComplexity = "map.gen.tab.complexity";
constexpr const char* kPopulation = "map.gen.tab.population";

constexpr const char* kTabs[] = {kShape, kComplexity, kPopulation};

// Captureless lambdas decay to the plain function pointers Knob holds, which
// keeps the table a constant with no per-row allocation.
constexpr Knob kKnobs[] = {
	{"width", "map.gen.width", kShape, KnobKind::Int, 8, 128,
	 [](const Params& p) -> double { return p.width; },
	 [](Params& p, double v) { p.width = static_cast<int>(v); }},
	{"height", "map.gen.height", kShape, KnobKind::Int, 8, 128,
	 [](const Params& p) -> double { return p.height; },
	 [](Params& p, double v) { p.height = static_cast<int>(v); }},
	// P2: the main path and its side branches, as COUNTS. (These replaced
	// `rooms` and `branching`; an old settings line still naming those just has
	// them ignored by Decode, and the defaults stand in.)
	{"path", "map.gen.path", kShape, KnobKind::Int, 1, 30,
	 [](const Params& p) -> double { return p.path; },
	 [](Params& p, double v) { p.path = static_cast<int>(v); }},
	{"branches", "map.gen.branches", kShape, KnobKind::Int, 0, 12,
	 [](const Params& p) -> double { return p.branches; },
	 [](Params& p, double v) { p.branches = static_cast<int>(v); }},
	{"branchmin", "map.gen.branchmin", kShape, KnobKind::Int, 1, 10,
	 [](const Params& p) -> double { return p.branchMin; },
	 [](Params& p, double v) { p.branchMin = static_cast<int>(v); }},
	{"branchmax", "map.gen.branchmax", kShape, KnobKind::Int, 1, 10,
	 [](const Params& p) -> double { return p.branchMax; },
	 [](Params& p, double v) { p.branchMax = static_cast<int>(v); }},
	{"seed", "map.gen.seed", kShape, KnobKind::Seed, 0, 4294967295.0,
	 [](const Params& p) -> double { return p.seed; },
	 [](Params& p, double v) { p.seed = static_cast<u32>(v); }},
	// P3: complexity, one knob each (Michael: "a separate setting for each").
	{"loops", "map.gen.loops", kComplexity, KnobKind::Int, 0, 10,
	 [](const Params& p) -> double { return p.loops; },
	 [](Params& p, double v) { p.loops = static_cast<int>(v); }},
	{"winding", "map.gen.winding", kComplexity, KnobKind::Float, 0, 1,
	 [](const Params& p) -> double { return p.winding; },
	 [](Params& p, double v) { p.winding = static_cast<float>(v); }},
	{"irregular", "map.gen.irregular", kComplexity, KnobKind::Float, 0, 1,
	 [](const Params& p) -> double { return p.irregular; },
	 [](Params& p, double v) { p.irregular = static_cast<float>(v); }},
	{"deadends", "map.gen.deadends", kComplexity, KnobKind::Int, 0, 10,
	 [](const Params& p) -> double { return p.deadEnds; },
	 [](Params& p, double v) { p.deadEnds = static_cast<int>(v); }},
	{"locks", "map.gen.locks", kPopulation, KnobKind::Int, 0, 8,
	 [](const Params& p) -> double { return p.locks; },
	 [](Params& p, double v) { p.locks = static_cast<int>(v); }},
	{"difficulty", "map.gen.difficulty", kPopulation, KnobKind::Float, 0, 1,
	 [](const Params& p) -> double { return p.difficulty; },
	 [](Params& p, double v) { p.difficulty = static_cast<float>(v); }},
	{"reward", "map.gen.reward", kPopulation, KnobKind::Float, 0, 1,
	 [](const Params& p) -> double { return p.reward; },
	 [](Params& p, double v) { p.reward = static_cast<float>(v); }},
};

} // namespace

std::span<const Knob> Knobs() { return kKnobs; }

std::span<const char* const> KnobTabs() { return kTabs; }

void SetKnob(const Knob& k, Params& p, double v) {
	if (!std::isfinite(v)) return;
	v = std::clamp(v, k.lo, k.hi);
	// Fractions keep two places: the dialog shows two, and a value written to a
	// file should read back as what the slider said, not 0.4999237.
	v = k.kind == KnobKind::Float ? std::round(v * 100.0) / 100.0 : std::round(v);
	k.set(p, v);
}

std::string Encode(const Params& p) {
	std::string out;
	for (const Knob& k : kKnobs) {
		if (!out.empty()) out += ' ';
		// Whole numbers print as integers: {:g} would put a large seed in
		// exponent form, and a seed that does not round-trip is a lost level.
		const double v = k.get(p);
		out += k.kind == KnobKind::Float
				   ? std::format("{}:{:g}", k.key, v)
				   : std::format("{}:{}", k.key, static_cast<long long>(v));
	}
	return out;
}

void Decode(std::string_view text, Params& p) {
	size_t pos = 0;
	while (pos < text.size()) {
		const size_t end = std::min(text.find(' ', pos), text.size());
		const std::string_view pair = text.substr(pos, end - pos);
		pos = end + 1;
		const size_t colon = pair.find(':');
		if (colon == std::string_view::npos) continue;
		const std::string_view key = pair.substr(0, colon);
		const std::string_view val = pair.substr(colon + 1);
		double v = 0.0;
		const auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), v);
		if (ec != std::errc() || ptr != val.data() + val.size()) continue;
		for (const Knob& k : kKnobs)
			if (key == k.key) SetKnob(k, p, v);
	}
}

} // namespace dungeon::game::generate
