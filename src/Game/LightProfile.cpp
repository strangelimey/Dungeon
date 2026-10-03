// ============================================================================
// Game/LightProfile.cpp - see LightProfile.h.
// ============================================================================
#include "Game/LightProfile.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace dungeon::game::light {

namespace {

constexpr const char* kPulseNames[] = {"steady", "flicker", "breathe", "strobe", "storm"};

// Leading/trailing blanks off a view.
std::string_view Trim(std::string_view s) {
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
		s.remove_suffix(1);
	return s;
}

bool ParseFloat(std::string_view text, float& out) {
	text = Trim(text);
	if (text.empty()) return false;
	float v = 0.0f;
	const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
	if (ec != std::errc{} || end != text.data() + text.size()) return false;
	out = v;
	return true;
}

// "r, g, b" (the catalogs' colour form, as blast_color); spaces also separate.
bool ParseColor(std::string_view text, Vec3& out) {
	float c[3];
	int n = 0;
	size_t i = 0;
	while (i < text.size() && n < 3) {
		while (i < text.size() && (text[i] == ' ' || text[i] == ',' || text[i] == '\t')) ++i;
		size_t j = i;
		while (j < text.size() && text[j] != ' ' && text[j] != ',' && text[j] != '\t') ++j;
		if (j > i && !ParseFloat(text.substr(i, j - i), c[n++])) return false;
		i = j;
	}
	if (n != 3 || !Trim(text.substr(std::min(i, text.size()))).empty()) return false;
	out = {c[0], c[1], c[2]};
	return true;
}

bool ParseBool(std::string_view text, bool& out) {
	text = Trim(text);
	if (text == "1" || text == "true" || text == "yes") { out = true; return true; }
	if (text == "0" || text == "false" || text == "no") { out = false; return true; }
	return false;
}

} // namespace

const char* PulseName(Pulse pulse) {
	const auto i = static_cast<size_t>(pulse);
	return i < std::size(kPulseNames) ? kPulseNames[i] : "steady";
}

bool ParsePulse(std::string_view word, Pulse& out) {
	word = Trim(word);
	for (size_t i = 0; i < std::size(kPulseNames); ++i)
		if (word == kPulseNames[i]) {
			out = static_cast<Pulse>(i);
			return true;
		}
	return false;
}

Profile Parse(std::string id, const std::function<std::string(std::string_view)>& get,
			  std::vector<std::string>* problems) {
	Profile p;
	p.id = std::move(id);
	const auto bad = [&](std::string_view key, const std::string& text) {
		if (problems)
			problems->push_back(std::format("lights.cat [{}]: `{} = {}` is not readable; "
											"the default is kept",
											p.id, key, text));
	};
	const auto number = [&](std::string_view key, float& field) {
		const std::string text = get(key);
		if (!text.empty() && !ParseFloat(text, field)) bad(key, text);
	};
	const auto flag = [&](std::string_view key, bool& field) {
		const std::string text = get(key);
		if (!text.empty() && !ParseBool(text, field)) bad(key, text);
	};

	if (const std::string text = get("color"); !text.empty()) {
		if (Trim(text) == "source") p.sourceColor = true;
		else if (!ParseColor(text, p.color)) bad("color", text);
	}
	number("intensity", p.intensity);
	number("radius", p.radius);
	if (const std::string text = get("pulse"); !text.empty() && !ParsePulse(text, p.pulse))
		bad("pulse", text);
	number("pulse_rate", p.pulseRate);
	number("pulse_depth", p.pulseDepth);
	number("wander", p.wander);
	flag("shadow", p.shadow);
	flag("long_fade", p.longFade);
	return p;
}

float PulseAt(Pulse pulse, float rate, float depth, float time, float phase) {
	const float t = time * rate;
	switch (pulse) {
	case Pulse::Flicker:
		// The fires' own flicker (it was hand-written into UpdateLights): two
		// incommensurate sines multiplied, so it never visibly repeats.
		return (1.0f - depth) + depth * std::sin(t * 11.0f + phase) * std::sin(t * 7.3f + phase);
	case Pulse::Breathe:
		// A rune's breath: the tablet's emissive and its light share this
		// frequency (DungeonWorld::RunePulse), so they rise and fall together.
		return 1.0f + depth * std::sin(t * 3.0f + phase);
	case Pulse::Strobe: {
		// A short full beat (the first 15% of each period), dimmed between.
		const float period = t + phase * 0.159155f; // phase / 2pi, in periods
		const float within = period - std::floor(period);
		return within < 0.15f ? 1.0f : 1.0f - depth;
	}
	case Pulse::Storm: {
		// Three sines multiplied and sharpened: almost always near zero, rarely
		// and unevenly close to one - a flash out of a dim glow.
		const float s = std::sin(t * 1.7f + phase) * std::sin(t * 2.9f + phase * 2.0f) *
						std::sin(t * 4.3f + phase * 0.5f);
		const float spike = s > 0.0f ? std::pow(s, 6.0f) * 4.0f : 0.0f;
		return (1.0f - depth) + depth * std::min(spike, 1.0f);
	}
	case Pulse::Steady:
	default:
		return 1.0f;
	}
}

Vec3 WanderAt(float wander, float time, float phase) {
	if (wander <= 0.0f) return {};
	// The fires' wander, as it was: a vertical bob of 0.6 x the sideways drift,
	// each axis its own pair of sines with its own phase.
	return {wander * std::sin(time * 7.3f + phase) * std::sin(time * 3.1f + phase * 2.0f),
			wander * 0.6f * std::sin(time * 9.1f + phase * 1.3f),
			wander * std::sin(time * 6.7f + phase * 0.7f) * std::sin(time * 2.6f + phase)};
}

Sample Evaluate(const Profile& profile, float time, float phase) {
	return {profile.intensity *
				PulseAt(profile.pulse, profile.pulseRate, profile.pulseDepth, time, phase),
			WanderAt(profile.wander, time, phase)};
}

const Profile& Fallback() {
	static const Profile kFallback = [] {
		Profile p;
		p.id = "(fallback)";
		p.color = {1.0f, 0.62f, 0.28f};
		p.intensity = 1.8f;
		p.radius = 3.0f;
		p.pulse = Pulse::Flicker;
		p.pulseDepth = 0.1f;
		return p;
	}();
	return kFallback;
}

} // namespace dungeon::game::light
