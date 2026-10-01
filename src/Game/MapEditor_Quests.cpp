// ============================================================================
// Game/MapEditor_Quests.cpp - the palette's QUEST ITEMS & FLAGS section
// (docs/tool-refinement-plan.md Phase 4; see the block in MapEditor.h).
//
// Michael asked for a local and a global "quest item area", holding items and
// flags. Local is the DUNGEON (his answer), so the section is two groups: this
// dungeon's - the one the viewed level belongs to - and the world's. Another
// dungeon's flags are not listed; browse a level there to see them.
//
// The rows are of two catalogs: flags (the section's own) and quest items (the
// item catalogs, marked by PaletteItem::ref). A flag has nothing to place, so
// its row opens its editor; an item row arms that item's brush like any item
// row, and says where the item lies - with a link there.
// ============================================================================
#include "Game/MapEditor.h"

#include "Core/Loc.h"
#include "Game/DungeonWorld.h"
#include "Game/MapColors.h"
#include "Game/MapView.h"

#include <string>

namespace dungeon::game {

namespace {
// A flag's swatch says whether it is on in the game being played - the editor
// is a live view, so pulling a lever lights its flag here.
const Vec4 kFlagOn{0.95f, 0.74f, 0.26f, 1.0f};
const Vec4 kFlagOff{0.30f, 0.30f, 0.34f, 1.0f};

// The item catalog holding `id` ("items", "weapons", "armor").
const char* ItemCatalogKey(const Project& proj, const std::string& id) {
	if (proj.items.Contains(id)) return "items";
	if (proj.weapons.Contains(id)) return "weapons";
	return "armor";
}
} // namespace

std::vector<MapEditor::PaletteItem> MapEditor::QuestSectionItems() const {
	const Project& proj = m_world->GetProject();
	const CatalogEntry* here = proj.DungeonOfLevel(m_view.ViewedLevel());
	const std::string local = here ? loc::Format("map.qf.local", here->Display()) : std::string();
	const std::string world = loc::Tr("map.qf.world");
	// The group a scope lists under: this dungeon's, the world's, or none (some
	// other dungeon's - not listed here).
	auto groupOf = [&](const std::string& dungeon) -> const std::string* {
		if (dungeon.empty()) return &world;
		if (here && dungeon == here->id) return &local;
		return nullptr;
	};

	std::vector<PaletteItem> out;
	for (const CatalogEntry& e : proj.flags.Entries()) {
		const std::string* group = groupOf(e.Get("dungeon", ""));
		if (!group) continue;
		PaletteItem it;
		it.label = e.Display();
		it.id = e.id;
		it.group = *group;
		it.swatch = m_world->FlagOn(e.id) ? kFlagOn : kFlagOff;
		out.push_back(std::move(it));
	}

	// The quest items, each with where it lies: the census walks every level as
	// it is now (live, stashed or on disk) and is cached per edit.
	const std::vector<DungeonWorld::LevelCensus>& census = m_world->Census();
	for (const CatalogEntry* e : proj.AllItems()) {
		if (!DungeonWorld::IsQuestItem(e)) continue;
		// The flag it sets, by its key (a hand-written `flag = k=v` sets k).
		std::string scope;
		const std::string flag = e->Get("flag", "");
		if (const CatalogEntry* f = proj.flags.Find(flag.substr(0, flag.find('='))))
			scope = f->Get("dungeon", "");
		const std::string* group = groupOf(scope);
		if (!group) continue;
		PaletteItem it;
		it.id = e->id;
		it.group = *group;
		it.swatch = kItem;
		it.ref = ItemCatalogKey(proj, e->id);
		int placed = 0;
		for (const DungeonWorld::LevelCensus& lc : census)
			for (const DungeonWorld::LevelCensus::Placed& p : lc.questPlaced)
				if (p.type == e->id && placed++ == 0) {
					it.gotoLevel = lc.stem;
					it.gotoX = p.x;
					it.gotoZ = p.z;
				}
		// Named as the game names it (`name` is the item's loc key), so the row
		// reads "Sunken Relic", not its id.
		const std::string name = e->Find("name") ? loc::Tr(e->Get("name", "")) : e->Display();
		it.label = placed == 0 ? loc::Format("map.qf.unplaced", name)
				   : placed == 1
					   ? loc::Format("map.qf.at", name, it.gotoLevel, it.gotoX, it.gotoZ)
					   : loc::Format("map.qf.many", name, placed);
		out.push_back(std::move(it));
	}
	return out;
}

void MapEditor::QuestRowClick(const PaletteRow& row, float mx, float my) {
	const std::vector<PaletteItem> items = CategoryItems(row.cat);
	if (row.index < 0 || row.index >= static_cast<int>(items.size())) return;
	const PaletteItem& it = items[static_cast<size_t>(row.index)];
	UseQuestRow(it, !it.gotoLevel.empty() && GoToRect(row.rect).Contains(mx, my));
}

bool MapEditor::UseQuestRow(const std::string& id, bool link) {
	for (const PaletteItem& it : QuestSectionItems())
		if (it.id == id) {
			UseQuestRow(it, link);
			return true;
		}
	return false;
}

void MapEditor::UseQuestRow(const PaletteItem& it, bool link) {
	if (it.ref.empty()) { // a flag: nothing to place, so its editor
		if (onConfigure) onConfigure(PaletteCat::Flags, it.id);
		return;
	}
	if (link && !it.gotoLevel.empty()) {
		if (onGoTo) onGoTo(it.gotoLevel, it.gotoX, it.gotoZ);
		return;
	}
	// An item: its brush, armed in its OWN section's terms (that is what places
	// it), without switching the palette away from here. Again = put it down.
	const PaletteCat cat = RowCat(PaletteCat::Flags, it);
	if (ArmedCat() == cat && ArmedId() == it.id) Disarm();
	else Arm(cat, it.id);
}

} // namespace dungeon::game
