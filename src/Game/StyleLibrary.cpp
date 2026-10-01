// ============================================================================
// Game/StyleLibrary.cpp - see StyleLibrary.h.
// ============================================================================
#include "Game/StyleLibrary.h"

#include "Game/Project.h"
#include "Game/Style.h"

#include <algorithm>
#include <array>

namespace dungeon::game {

namespace {
// The three surfaces a theme is made of: its field and the catalog behind it.
struct Member {
	const char* field;
	const char* catalogKey;
	Catalog StyleLibrary::*library;
	Catalog Project::*project;
};
constexpr std::array<Member, 3> kMembers{{
	{"wall", "walls", &StyleLibrary::walls, &Project::walls},
	{"floor", "floors", &StyleLibrary::floors, &Project::floors},
	{"ceiling", "ceilings", &StyleLibrary::ceilings, &Project::ceilings},
}};

// A theme member is ONE id; a legacy list keeps its first word (the themes.cat
// rule, DungeonWorld::ThemeMembersOf).
std::string FirstWord(const std::string& v) {
	const size_t b = v.find_first_not_of(" \t");
	if (b == std::string::npos) return {};
	return v.substr(b, v.find_first_of(" \t", b) - b);
}

// Each file's header, written above its entries when the library is saved.
struct File {
	const char* name;
	Catalog StyleLibrary::*catalog;
	const char* header;
};
constexpr File kFiles[] = {
	{"styles.cat", &StyleLibrary::styles,
	 "The style LIBRARY (docs/tool-refinement-plan.md Phase 5): styles any world can "
	 "add from the palette's Styles section. A style is a room theme, a corridor "
	 "theme, generator knobs, a corridor width, tags and a weighted monster list "
	 "(Game/Style.h). Adding one copies it and whatever it points at that the world "
	 "lacks; \"Save to library\" in a style's editor writes one back here."},
	{"themes.cat", &StyleLibrary::themes,
	 "The themes the library's styles name (one floor / wall / ceiling each, as in a "
	 "world's themes.cat). Copied into a world with the style that names them."},
	{"walls.cat", &StyleLibrary::walls, "Wall types the library's themes are made of."},
	{"floors.cat", &StyleLibrary::floors, "Floor types the library's themes are made of."},
	{"ceilings.cat", &StyleLibrary::ceilings, "Ceiling types the library's themes are made of."},
};
} // namespace

void StyleLibrary::Load(const std::string& folder) {
	m_folder = folder;
	for (const File& f : kFiles) {
		(this->*f.catalog) = {};
		(this->*f.catalog).Load(folder + "\\" + f.name);
	}
}

bool StyleLibrary::Save() const {
	bool ok = !m_folder.empty();
	for (const File& f : kFiles)
		ok = (this->*f.catalog).Save(m_folder + "\\" + f.name, f.header) && ok;
	return ok;
}

std::vector<std::string> StyleLibrary::StyleThemes(const CatalogEntry& style) {
	std::vector<std::string> out;
	for (const char* field : {"room", "corridor"}) {
		const std::string t = FirstWord(style.Get(field, ""));
		if (!t.empty() && std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
	}
	return out;
}

bool StyleLibrary::AddTo(const std::string& id, Project& proj, AddResult& out) const {
	out = {};
	const CatalogEntry* style = styles.Find(id);
	if (!style) return false;
	if (proj.styles.Contains(id)) {
		out.already = true;
		return true;
	}
	// What it points at, the world lacking it: surfaces first, then the themes
	// made of them, then the style that names the themes - so nothing is ever in
	// the world before what it names.
	for (const std::string& themeId : StyleThemes(*style)) {
		if (proj.themes.Contains(themeId)) continue; // the world's own wins
		const CatalogEntry* theme = themes.Find(themeId);
		if (!theme) continue; // the checker names a style's missing theme
		for (const Member& m : kMembers) {
			const std::string member = FirstWord(theme->Get(m.field, ""));
			if (member.empty() || (proj.*m.project).Contains(member)) continue;
			if (const CatalogEntry* e = (this->*m.library).Find(member)) {
				(proj.*m.project).Add(*e);
				out.copied.push_back({m.catalogKey, member});
			}
		}
		proj.themes.Add(*theme);
		out.copied.push_back({"themes", themeId});
	}
	proj.styles.Add(*style);
	out.copied.push_back({"styles", id});
	for (const style::Pick& p : style::ParseMonsters(style->Get("monsters", "")))
		if (!proj.monsters.Contains(p.id)) out.missingMonsters.push_back(p.id);
	return true;
}

bool StyleLibrary::SaveFrom(const std::string& id, const Project& proj, std::vector<Copy>& out) {
	out.clear();
	const CatalogEntry* style = proj.styles.Find(id);
	if (!style) return false;
	for (const std::string& themeId : StyleThemes(*style)) {
		const CatalogEntry* theme = proj.themes.Find(themeId);
		if (!theme) continue;
		for (const Member& m : kMembers) {
			const std::string member = FirstWord(theme->Get(m.field, ""));
			if (member.empty()) continue;
			if (const CatalogEntry* e = (proj.*m.project).Find(member)) {
				(this->*m.library).Add(*e);
				out.push_back({m.catalogKey, member});
			}
		}
		themes.Add(*theme);
		out.push_back({"themes", themeId});
	}
	styles.Add(*style);
	out.push_back({"styles", id});
	return true;
}

} // namespace dungeon::game
