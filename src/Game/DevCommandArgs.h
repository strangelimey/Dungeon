// ============================================================================
// Game/DevCommandArgs.h — the dev-console commands' shared arg helpers.
//
// Moved out of Game_DevCommands.cpp's anonymous namespace when its command
// table split by concern (Game_DevWorld / _DevDiagnostics / _DevParty /
// _DevEval / _DevDungeons), so every file registering commands guards and
// parses its arguments the same way. Include only from those files; each
// pulls in the names it uses with a using-declaration.
// ============================================================================
#pragma once

#include "Game/DevConsole.h"
#include "Game/Spells.h"

#include <string>
#include <vector>

namespace dungeon::game::devargs {

// Shared dev-console arg helpers (Game registers ~30 commands; these carry the
// repeated guards/parsing so each command body is just its action).

// Arg-count guard: prints `usage` and returns false when fewer than n args.
// REFUSES rather than prints, so a script that mis-called a command fails
// instead of measuring whatever the world happened to hold — one change here
// covers every command's arity error (docs/eval-audit.md F11).
inline bool Need(DevConsole& console, const std::vector<std::string>& args, size_t n,
				 const char* usage) {
	if (args.size() < n) {
		console.Refuse(usage);
		return false;
	}
	return true;
}

// Joins args into one space-separated string (save-slot names may have spaces).
inline std::string JoinArgs(const std::vector<std::string>& args) {
	std::string out;
	for (const std::string& a : args)
		out += (out.empty() ? "" : " ") + a;
	return out;
}

// A toggle command's argument: "on"/"1" enable, anything else disables.
inline bool ArgOn(const std::string& a) { return a == "on" || a == "1"; }

// Parses one symbol-id arg (fire/earth/air/water); on a bad token prints the
// shared usage line and returns false, so a command can `if (!...) return;`.
inline bool ParseSymbolArg(DevConsole& console, const std::string& arg, SpellSymbol& out) {
	if (ParseSymbol(arg, out)) return true;
	console.Print("symbol must be fire/earth/air/water");
	return false;
}

} // namespace dungeon::game::devargs
