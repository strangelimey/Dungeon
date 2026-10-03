// ============================================================================
// Game/Trail.cpp - see Trail.h.
// ============================================================================
#include "Game/Trail.h"

#include "Game/LightProfile.h"

#include <format>

namespace dungeon::game::trail {

namespace {
constexpr const char* kShapeNames[] = {"spark", "ember", "mote", "puff", "drip"};

std::string_view Trim(std::string_view s) {
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
		s.remove_suffix(1);
	return s;
}
} // namespace

const char* ShapeName(Shape shape) {
	const auto i = static_cast<size_t>(shape);
	return i < std::size(kShapeNames) ? kShapeNames[i] : "spark";
}

bool ParseShape(std::string_view word, Shape& out) {
	word = Trim(word);
	for (size_t i = 0; i < std::size(kShapeNames); ++i)
		if (word == kShapeNames[i]) {
			out = static_cast<Shape>(i);
			return true;
		}
	return false;
}

Spec ShapeDefaults(Shape shape) {
	Spec s;
	s.shape = shape;
	switch (shape) {
	case Shape::Spark:
		s.life = 0.3f;
		s.size = 0.04f;
		s.spread = 1.2f;
		s.fall = 4.0f;
		break;
	case Shape::Ember:
		s.life = 0.7f;
		s.size = 0.045f;
		s.spread = 0.35f;
		s.fall = -0.8f;
		s.flicker = 0.6f;
		break;
	case Shape::Mote:
		s.life = 0.9f;
		s.size = 0.04f;
		s.spread = 0.3f;
		s.fall = 0.0f;
		s.swirl = 5.0f;
		break;
	case Shape::Puff:
		s.life = 0.8f;
		s.size = 0.1f;
		s.spread = 0.25f;
		s.fall = -0.2f;
		s.swell = true;
		break;
	case Shape::Drip:
		s.life = 0.5f;
		s.size = 0.03f;
		s.spread = 0.2f;
		s.fall = 7.0f;
		break;
	}
	return s;
}

Profile Parse(std::string id, const std::function<std::string(std::string_view)>& get,
			  std::vector<std::string>* problems) {
	Profile p;
	p.id = std::move(id);
	const auto bad = [&](std::string_view key, const std::string& text) {
		if (problems)
			problems->push_back(std::format("trails.cat [{}]: `{} = {}` is not readable; "
											"the default is kept",
											p.id, key, text));
	};
	Shape shape = Shape::Spark;
	if (const std::string text = get("shape"); !text.empty() && !ParseShape(text, shape))
		bad("shape", text);
	p.spec = ShapeDefaults(shape);
	const auto number = [&](std::string_view key, float& field) {
		const std::string text = get(key);
		if (!text.empty() && !light::ReadFloat(text, field)) bad(key, text);
	};
	number("rate", p.spec.rate);
	number("life", p.spec.life);
	number("size", p.spec.size);
	number("spread", p.spec.spread);
	number("fall", p.spec.fall);
	number("flicker", p.spec.flicker);
	number("swirl", p.spec.swirl);
	if (const std::string text = get("swell"); !text.empty() && !light::ReadBool(text, p.spec.swell))
		bad("swell", text);
	if (const std::string text = get("color"); !text.empty()) {
		if (Trim(text) == "source") p.spec.hasColor = false;
		else if (light::ReadColor(text, p.spec.color)) p.spec.hasColor = true;
		else bad("color", text);
	}
	// A life of nothing would divide by zero in the fade.
	if (p.spec.life < 0.05f) p.spec.life = 0.05f;
	return p;
}

} // namespace dungeon::game::trail
