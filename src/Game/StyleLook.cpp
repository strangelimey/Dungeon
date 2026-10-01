// ============================================================================
// Game/StyleLook.cpp - see StyleLook.h.
// ============================================================================
#include "Game/StyleLook.h"

#include "Game/Carve.h"
#include "Game/Project.h"
#include "Game/Style.h"

#include <algorithm>
#include <format>

namespace dungeon::game::stylelook {

namespace {
constexpr const char* kSurface[3] = {"wall", "floor", "ceiling"};
} // namespace

const CatalogEntry* Find(const Project& project, const std::string& id) {
	return id.empty() ? nullptr : project.styles.Find(id);
}

std::vector<std::string> Tags(const CatalogEntry& style) { return ParseTags(style.Get("tags", "")); }

void Monsters(const Project& project, const CatalogEntry& style, std::vector<std::string>& ids,
			  std::vector<float>& weights) {
	ids.clear();
	weights.clear();
	for (const style::Pick& pick : style::ParseMonsters(style.Get("monsters", ""))) {
		const CatalogEntry* m = project.monsters.Find(pick.id);
		if (!m || CatalogBool(m, "hidden", false)) continue;
		ids.push_back(pick.id);
		weights.push_back(pick.weight);
	}
}

std::array<std::vector<std::string>, 3>
Palettes(const Look& look, const std::array<std::vector<std::string>, 3>& donor) {
	std::array<std::vector<std::string>, 3> out;
	for (size_t s = 0; s < 3; ++s) out[s] = look.palettes[s].empty() ? donor[s] : look.palettes[s];
	return out;
}

Look Lay(const Project& project, const CatalogEntry& style, const std::vector<u8>& floor, int w,
		 int h) {
	Look look;
	// The two themes, room first; one the project lacks is skipped (the checker
	// names it), so a half-broken style still paints what it can.
	std::array<const CatalogEntry*, 2> theme{}; // [Role::Room], [Role::Corridor]
	theme[static_cast<size_t>(carve::Role::Room)] = project.themes.Find(style.Get("room", ""));
	theme[static_cast<size_t>(carve::Role::Corridor)] = project.themes.Find(style.Get("corridor", ""));
	// Its members per surface, the ones whose surface type the world has.
	const Catalog* surfaces[3] = {&project.walls, &project.floors, &project.ceilings};
	std::array<std::array<std::string, 3>, 2> member{};
	for (size_t t = 0; t < 2; ++t) {
		if (!theme[t]) continue;
		for (size_t s = 0; s < 3; ++s) {
			const std::string id = theme[t]->Get(kSurface[s], "");
			if (id.empty() || !surfaces[s]->Contains(id)) continue;
			member[t][s] = id;
			if (std::find(look.palettes[s].begin(), look.palettes[s].end(), id) == look.palettes[s].end())
				look.palettes[s].push_back(id);
		}
	}
	const carve::Dressing dress = carve::Dress(floor, w, h);
	const auto lay = [&](const carve::Square& q, size_t s) {
		const size_t t = static_cast<size_t>(q.role);
		if (!theme[t] || member[t][s].empty()) return;
		look.records += std::format("theme {} {} {} {}\n", kSurface[s], q.x, q.z, theme[t]->id);
	};
	for (const carve::Square& q : dress.open) {
		lay(q, 1); // floor
		lay(q, 2); // ceiling
	}
	for (const carve::Square& q : dress.walls) lay(q, 0);
	return look;
}

} // namespace dungeon::game::stylelook
