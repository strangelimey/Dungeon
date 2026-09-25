// ============================================================================
// Game/Threat.cpp — see Threat.h.
// ============================================================================
#include "Game/Threat.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>

namespace dungeon::game::threat {

namespace {

// P(a - b > k) for two independent d100s, as the continuous triangle on
// [-100, 100] that their difference follows. Clamped to the combat model's own
// 5%..95% band, so nothing is ever certain.
double Beats(double k) {
	double p;
	if (k >= 100.0) p = 0.0;
	else if (k <= -100.0) p = 1.0;
	else if (k >= 0.0) p = (100.0 - k) * (100.0 - k) / 20000.0;
	else p = 1.0 - (100.0 + k) * (100.0 + k) / 20000.0;
	return std::clamp(p, 0.05, 0.95);
}

// The average PHYSICAL resist (slash / pierce / bash) from a `resists` field
// such as "pierce 0.5, slash 0.25, bash -0.5". Physical, because that is what
// a party mostly deals; the elements are a specialist's answer, not the norm.
double PhysicalResist(const std::string& field) {
	std::string text = field;
	std::replace(text.begin(), text.end(), ',', ' ');
	std::istringstream in(text);
	std::string type;
	double value = 0, sum = 0;
	while (in >> type >> value)
		if (type == "slash" || type == "pierce" || type == "bash") sum += value;
	return sum / 3.0;
}

} // namespace

Parts Of(const CatalogEntry& m) {
	const double hp = std::max(1.0, static_cast<double>(m.GetFloat("hp", 10.0f)));
	const double damage = std::max(0.0, static_cast<double>(m.GetFloat("damage", 3.0f)));
	const double accuracy = m.GetFloat("accuracy", 60.0f);
	const double defense = m.GetFloat("defense", 10.0f);
	const double offense = std::clamp(static_cast<double>(m.GetFloat("offense", 1.0f)), 0.0, 1.0);
	const double armor = std::max(0.0, static_cast<double>(m.GetFloat("armor", 0.0f)));
	const double attackcd = std::max(0.1, static_cast<double>(m.GetFloat("attackcd", 1.5f)));

	Parts p;
	// The stance: `offense` of the accuracy presses the attack, the rest guards.
	const double press = accuracy * offense;
	const double guard = defense + accuracy * (1.0 - offense);
	p.hit = Beats(kRefDefense - press);
	p.beHit = Beats(guard - kRefAccuracy);
	p.offence = p.hit * damage / attackcd;
	// Flat soak takes `armor` off every blow; a resist takes its fraction of
	// what is left. Both become "how many raw points does it take to kill".
	const double soak = kRefBlow / std::max(1.0, kRefBlow - armor);
	const double resist = 1.0 / std::clamp(1.0 - PhysicalResist(m.Get("resists", "")), 0.25, 4.0);
	p.toughness = hp * soak * resist / p.beHit;
	p.threat = std::sqrt(p.offence * p.toughness);
	return p;
}

} // namespace dungeon::game::threat
