// ============================================================================
// Game/Game_DevDungeons.cpp — the dungeon tier's dev-console commands (W10).
//
// Their own file because Game_DevCommands.cpp is past three thousand lines;
// this is where the dungeon tier's commands go from here on. Every one of them
// drives the SAME function the editor's mouse path calls, because the harness
// cannot click and a command with its own copy of a rule is a second editor
// wearing the first one's name.
// ============================================================================
#include "Game/Game.h"

#include "Core/Log.h"

#include <cstdlib>
#include <format>
#include <string>

namespace dungeon::game {

void Game::RegisterDungeonCommands() {
	m_console.Register(
		"dungeons",
		"the dungeon tier: dungeons | what <id> | delete <id> <id again> | "
		"rename <id> <new> | dialog [<id>|delete|confirm <text>|off]",
		[this](const std::vector<std::string>& a) {
			if (a.empty()) {
				for (const CatalogEntry& e : m_project.dungeons.Entries()) {
					std::string levels;
					for (const std::string& stem : m_project.DungeonLevels(e.id))
						levels += (levels.empty() ? "" : " ") + stem;
					m_console.Print(std::format("  {} '{}': {}", e.id,
												e.Get("display", e.id),
												levels.empty() ? "(no levels)" : levels));
				}
				m_console.Print(std::format("{} dungeon(s), {} level(s) in the manifest",
											m_project.dungeons.Entries().size(),
											m_project.levels.size()));
				return;
			}
			// What a delete WOULD do, and whether it is allowed — asked without
			// doing it, so a harness can read the refusal before any mutation.
			if (a[0] == "what" && a.size() >= 2) {
				const std::string why = DungeonDeleteRefusal(a[1]);
				m_console.Print(why.empty() ? std::format("delete '{}': allowed", a[1])
											: std::format("delete '{}': refused - {}", a[1], why));
				for (const std::string& line : DescribeDungeon(a[1]))
					m_console.Print("  " + line);
				return;
			}
			// The typed confirmation in its console form: the id TWICE, exact,
			// the `worlds delete` rule — a single word is a typo away from a
			// different dungeon.
			if (a[0] == "delete") {
				if (a.size() < 3 || a[1] != a[2]) {
					m_console.Print("usage: dungeons delete <id> <id again> "
									"(exact, case-sensitive)");
					return;
				}
				if (const std::string why = DungeonDeleteRefusal(a[1]); !why.empty()) {
					m_console.Print(std::format("delete '{}': refused - {}", a[1], why));
					return;
				}
				m_console.Print(DeleteDungeon(a[1])
									? std::format("deleted '{}'", a[1])
									: std::format("delete '{}': FAILED - see the log", a[1]));
				return;
			}
			// The type editor's title rename, which is where a dungeon is renamed
			// (W11) — the same RenameType, doorways and opening included.
			if (a[0] == "rename" && a.size() >= 3) {
				std::string problem;
				const bool ok = RenameType("dungeons", a[1], a[2], problem);
				m_console.Print(ok ? std::format("renamed dungeon '{}' -> '{}'", a[1], a[2])
								   : std::format("rename '{}': refused{}{}", a[1],
												 problem.empty() ? "" : " - ", problem));
				return;
			}
			// The type editor's own path, step by step: open it on a dungeon,
			// click its Delete, type into the confirmation. Each step reports
			// where the dialog stands, which is what a harness reads.
			if (a[0] == "dialog") {
				if (a.size() >= 2 && a[1] == "off") {
					m_typeDialog.Close();
				} else if (a.size() >= 2 && a[1] == "delete") {
					m_typeDialog.ClickDelete();
				} else if (a.size() >= 2 && a[1] == "confirm") {
					m_typeDialog.ConfirmDelete(a.size() >= 3 ? a[2] : std::string());
				} else if (a.size() >= 2) {
					OpenTypeEditor(MapEditor::PaletteCat::Dungeons, a[1]);
				}
				// The click deferred its rebuild (it fires inside the tree walk,
				// and the dialog's Update does not run while the console is up) —
				// apply it so an audit after this sees the view it produced.
				m_typeDialog.ApplyPending();
				const bool open = m_typeDialog.IsOpen() &&
								  m_typeDialog.CatalogKey() == "dungeons";
				m_console.Print(std::format(
					"dungeons dialog: {} '{}'{} - {}", open ? "open" : "closed",
					open ? m_typeDialog.Id() : std::string(),
					open && m_typeDialog.Confirming() ? " confirming" : "",
					open ? m_typeDialog.Notice() : std::string()));
				return;
			}
			m_console.Print("usage: dungeons | what <id> | delete <id> <id again> | "
							"rename <id> <new> | dialog [<id>|delete|confirm <text>|off]");
		});

	// The Level dialog's inline rename, from the console (W11): files, stairs,
	// the dungeon's list, the opening, the harness level and every doorway.
	m_console.Register(
		"levelrename", "rename a level everywhere it is named: levelrename <old> <new>",
		[this](const std::vector<std::string>& a) {
			if (a.size() < 2) {
				m_console.Print("usage: levelrename <old> <new>");
				return;
			}
			std::string why;
			m_console.Print(RenameLevel(a[0], a[1], &why)
								? std::format("renamed level '{}' -> '{}'", a[0], a[1])
								: std::format("rename level '{}': refused - {}", a[0], why));
		});

	// A stair placed from the console, as the Stairs brush places it (both
	// halves, the far one on the next or previous level in manifest order) and
	// as ONE undo step. It exists so the dungeon delete's stair refusal can be
	// driven: the case is "a stair from OUTSIDE leads in", and nothing else a
	// script can reach authors one.
	m_console.Register(
		"stairadd", "place a stair pair: stairadd <type> <x> <z> [level]",
		[this](const std::vector<std::string>& a) {
			if (a.size() < 3) {
				m_console.Print("usage: stairadd <type> <x> <z> [level]");
				return;
			}
			const std::string stem = a.size() >= 4 ? a[3] : m_world->CurrentLevel();
			const int x = std::atoi(a[1].c_str()), z = std::atoi(a[2].c_str());
			m_world->BeginUndoStep();
			const bool ok = m_world->AddStairAt(stem, a[0], x, z);
			m_world->CommitUndoStep(ok);
			m_console.Print(ok ? std::format("stair {} placed on {} at {},{}", a[0], stem, x, z)
							   : std::format("stair {} NOT placed on {} at {},{}", a[0],
											 stem, x, z));
		});
}

} // namespace dungeon::game
