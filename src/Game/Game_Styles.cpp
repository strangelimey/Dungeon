// ============================================================================
// Game/Game_Styles.cpp - styles and the shared library (docs/tool-refinement-
// plan.md Phase 5): adding a library style to the world, saving a world style
// to the library, and the console's way at both.
//
// The copying itself is StyleLibrary's (Game/StyleLibrary.h); this file saves
// what changed and says so, through onMessage and the log.
// ============================================================================
#include "Game/Game.h"

#include "Core/Loc.h"
#include "Core/Log.h"
#include "Game/DevCommandArgs.h"
#include "Game/Style.h"

#include <format>
#include <string>

namespace dungeon::game {

using devargs::Need;

namespace {
std::string CopyList(const std::vector<StyleLibrary::Copy>& copies) {
	std::string out;
	for (const StyleLibrary::Copy& c : copies)
		out += (out.empty() ? "" : ",") + c.catalogKey + ":" + c.id;
	return out.empty() ? "-" : out;
}

std::string JoinIds(const std::vector<std::string>& ids) {
	std::string out;
	for (const std::string& id : ids) out += (out.empty() ? "" : ",") + id;
	return out.empty() ? "-" : out;
}
} // namespace

StyleLibrary::AddResult Game::AddStyleFromLibrary(const std::string& id) {
	StyleLibrary::AddResult r;
	if (!m_library.AddTo(id, m_project, r)) {
		if (m_world->onMessage) m_world->onMessage(loc::FormatLine("map.style.nolib", id));
		return r;
	}
	const CatalogEntry* style = m_project.styles.Find(id);
	const std::string name = style ? style->Display() : id;
	if (r.already) {
		if (m_world->onMessage) m_world->onMessage(loc::FormatLine("map.style.already", name));
		return r;
	}
	// A catalog change takes no undo step but changes what the checker and the
	// palette read, so it counts as an edit (the type editor's rule).
	m_world->NoteEdit();
	if (!m_project.Save()) log::Warn("style add: failed to save the catalogs");
	log::Info("style add '{}': copied {}; missing monsters {}", id, CopyList(r.copied),
			  JoinIds(r.missingMonsters));
	if (m_world->onMessage) {
		// The count is what CAME ACROSS besides the style itself.
		m_world->onMessage(loc::FormatLine("map.style.added", name, r.copied.size() - 1));
		if (!r.missingMonsters.empty())
			m_world->onMessage(loc::FormatLine("map.style.missing", JoinIds(r.missingMonsters)));
	}
	return r;
}

bool Game::SaveStyleToLibrary(const std::string& id, std::vector<StyleLibrary::Copy>& out) {
	if (!m_library.SaveFrom(id, m_project, out)) return false;
	const bool ok = m_library.Save();
	if (!ok) log::Warn("style save: failed to write the library ({})", m_library.Folder());
	log::Info("style save '{}' to the library: {}", id, CopyList(out));
	const CatalogEntry* style = m_project.styles.Find(id);
	if (m_world->onMessage)
		m_world->onMessage(loc::FormatLine(ok ? "map.style.saved" : "map.style.savefailed",
										   style ? style->Display() : id));
	return ok;
}

void Game::RegisterStyleCommands() {
	// Every style, the world's and the library's, one machine-readable line each:
	// `style <id> <world|library> [current] room= corridor= width= monsters=`.
	m_console.Register(
		"styles", "list the world's styles and the library's",
		[this](const std::vector<std::string>&) {
			const auto line = [&](const CatalogEntry& e, const char* where) {
				m_console.Print(std::format(
					"style {} {}{} room={} corridor={} width={} monsters={}", e.id, where,
					e.id == m_mapEditor.CurrentStyle() ? " current" : "", e.Get("room", "-"),
					e.Get("corridor", "-"), e.Get("corridor_width", "1"),
					style::FormatMonsters(style::ParseMonsters(e.Get("monsters", "")))));
			};
			for (const CatalogEntry& e : m_project.styles.Entries()) line(e, "world");
			for (const CatalogEntry& e : m_library.styles.Entries())
				if (!m_project.styles.Contains(e.id)) line(e, "library");
		});
	m_console.Register(
		"style",
		"styles: style use <id>|off (the current style) | style add <id> (from the "
		"library) | style save <id> (to the library) | style row <id> (a palette click)",
		[this](const std::vector<std::string>& args) {
			if (!Need(m_console, args, 1, "usage: style use|add|save|row <id>")) return;
			const std::string& verb = args[0];
			if (verb == "use") {
				if (!Need(m_console, args, 2, "usage: style use <id>|off")) return;
				if (args[1] != "off" && !m_project.styles.Contains(args[1])) {
					m_console.Refuse(std::format("style: the world has no style '{}'", args[1]));
					return;
				}
				m_mapEditor.SetCurrentStyle(args[1] == "off" ? std::string() : args[1]);
			} else if (verb == "row") {
				if (!Need(m_console, args, 2, "usage: style row <id>")) return;
				if (!m_mapEditor.UseStyleRow(args[1])) {
					m_console.Refuse(std::format("style: no palette row '{}'", args[1]));
					return;
				}
			} else if (verb == "add") {
				if (!Need(m_console, args, 2, "usage: style add <id>")) return;
				const StyleLibrary::AddResult r = AddStyleFromLibrary(args[1]);
				m_console.Print(std::format(
					"style add {}: {} copied={} missing={}", args[1],
					r.already ? "already" : r.copied.empty() ? "refused" : "added",
					CopyList(r.copied), JoinIds(r.missingMonsters)));
				return;
			} else if (verb == "save") {
				if (!Need(m_console, args, 2, "usage: style save <id>")) return;
				std::vector<StyleLibrary::Copy> copied;
				const bool ok = SaveStyleToLibrary(args[1], copied);
				m_console.Print(std::format("style save {}: {} copied={}", args[1],
											ok ? "saved" : "refused", CopyList(copied)));
				return;
			} else {
				m_console.Refuse("style: use, add, save or row");
				return;
			}
			const std::string& cur = m_mapEditor.CurrentStyle();
			m_console.Print(std::format("style current {}", cur.empty() ? "-" : cur));
		});
}

} // namespace dungeon::game
