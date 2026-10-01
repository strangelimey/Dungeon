// ============================================================================
// Game/StyleLibrary.h - the SHARED library of styles (docs/tool-refinement-
// plan.md Phase 5; a style itself is described in Game/Style.h).
//
// Michael: styles live "both" in a shared library and per world. The library is
// a folder outside projects/ (assets/library, beside assets/templates), so no
// world list offers it, holding a styles.cat plus what those styles point at:
// the themes they name and the surface types those themes are made of.
//
// ADDING a library style to a world copies the style AND whatever it points at
// that the world lacks - a theme, a surface type - and nothing the world
// already has (the world's own wins, so an add can never repaint anything).
// Monsters are NOT copied: a monster is a model, a rig and a page of balance,
// not a line of data, so one the world lacks is reported as missing instead.
// After the add the world's copy is its own: there is no live link back, the
// template rule. A second add of the same style is a no-op.
//
// SAVING a world's style to the library goes the other way, and REPLACES the
// library's entries of the same ids - it is an explicit "this is the version I
// want to keep".
// ============================================================================
#pragma once

#include "Game/Catalog.h"

#include <string>
#include <vector>

namespace dungeon::game {

struct Project;

class StyleLibrary {
public:
	// Reads the library's catalogs from `folder` (a missing file = empty, the
	// catalogs' own rule).
	void Load(const std::string& folder);
	// Writes every catalog back to the folder it was loaded from.
	bool Save() const;
	const std::string& Folder() const { return m_folder; }

	Catalog styles, themes, walls, floors, ceilings;

	// One entry copied, by its catalog key ("styles", "themes", "walls", ...).
	struct Copy {
		std::string catalogKey, id;
	};
	struct AddResult {
		bool already = false;        // the world had the style: nothing was done
		std::vector<Copy> copied;    // what came across, surfaces first
		std::vector<std::string> missingMonsters; // listed, not copied
	};
	// Adds library style `id` to `proj`'s catalogs (in memory; the caller
	// saves). False when the library has no such style.
	bool AddTo(const std::string& id, Project& proj, AddResult& out) const;
	// Copies `proj`'s style `id`, its themes and their surface types into the
	// library (in memory; the caller saves). False when the world has no such
	// style.
	bool SaveFrom(const std::string& id, const Project& proj, std::vector<Copy>& out);

	// The themes.cat ids a style names (room, corridor), each once, in order.
	static std::vector<std::string> StyleThemes(const CatalogEntry& style);

private:
	std::string m_folder;
};

} // namespace dungeon::game
