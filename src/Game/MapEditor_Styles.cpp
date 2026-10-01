// ============================================================================
// Game/MapEditor_Styles.cpp - the palette's STYLES section (docs/tool-
// refinement-plan.md Phase 5; a style is described in Game/Style.h, the shared
// library in Game/StyleLibrary.h).
//
// Two groups: THIS WORLD's styles, and the LIBRARY's that this world lacks. A
// world style's row arms it as the current style - the one the Monsters
// section ranks by, and (Phase 6) the one the shape brushes build in. A
// library row adds the style to the world, with the themes and surfaces it
// needs; it then lists under This world like any other.
// ============================================================================
#include "Game/MapEditor.h"

#include "Core/Loc.h"
#include "Game/DungeonWorld.h"
#include "Game/MapColors.h"
#include "Game/MapView.h"
#include "Game/Style.h"
#include "Game/StyleLibrary.h"

#include <algorithm>

namespace dungeon::game {

namespace {
// A library style is not the world's yet: it draws without the room's swatch.
const Vec4 kLibrarySwatch{0.30f, 0.30f, 0.34f, 1.0f};
} // namespace

std::vector<MapEditor::PaletteItem> MapEditor::StyleSectionItems() const {
	const Project& proj = m_world->GetProject();
	const std::string mine = loc::Tr("map.st.world");
	std::vector<PaletteItem> out;
	for (const CatalogEntry& e : proj.styles.Entries()) {
		PaletteItem it;
		it.label = e.Display();
		it.id = e.id;
		it.group = mine;
		it.swatch = kFloor;
		// Its ROOM's floor, as the Themes section shows that theme.
		if (const CatalogEntry* room = proj.themes.Find(e.Get("room", ""))) {
			const std::string floor =
				DungeonWorld::ThemeMembersOf(*room)[static_cast<size_t>(Surface::Floor)];
			if (!floor.empty()) it.icon = SurfaceItem(PaletteCat::Floors, floor).icon;
		}
		out.push_back(std::move(it));
	}
	if (m_library) {
		const std::string lib = loc::Tr("map.st.library");
		for (const CatalogEntry& e : m_library->styles.Entries()) {
			if (proj.styles.Contains(e.id)) continue; // the world has its own copy
			PaletteItem it;
			it.label = e.Display();
			it.id = e.id;
			it.group = lib;
			it.swatch = kLibrarySwatch;
			it.ref = kLibraryRef;
			out.push_back(std::move(it));
		}
	}
	return out;
}

std::vector<std::string> MapEditor::StyleMonsters(const std::string& id) const {
	std::vector<std::string> out;
	if (id.empty()) return out;
	const CatalogEntry* e = m_world->GetProject().styles.Find(id);
	if (!e) return out;
	for (const style::Pick& p : style::ParseMonsters(e->Get("monsters", "")))
		out.push_back(p.id);
	return out;
}

bool MapEditor::UseStyleRow(const std::string& id) {
	for (const PaletteItem& it : StyleSectionItems()) {
		if (it.id != id) continue;
		if (it.ref == kLibraryRef) {
			if (onAddStyle) onAddStyle(it.id);
		} else {
			m_style = m_style == it.id ? std::string() : it.id; // again = off
		}
		return true;
	}
	return false;
}

} // namespace dungeon::game
