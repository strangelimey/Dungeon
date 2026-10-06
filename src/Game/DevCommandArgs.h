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

// The same guard, refusing with the command's REGISTERED params (every form)
// instead of a hand-written usage line - so `help` and the arity error cannot
// drift apart. Prefer this one; the message form is for a sub-verb whose usage
// is narrower than the whole command's.
inline bool Need(DevConsole& console, const std::vector<std::string>& args, size_t n) {
	if (args.size() < n) {
		console.RefuseUsage();
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

// Parses one symbol-id arg (fire, project, explode, ...); on a bad token REFUSES
// with the shared usage line - every id, from the table - and returns false, so a
// command can `if (!...) return;`. A refusal, not a print: a `learn 0 fyre`
// that taught nothing is a script measuring a caster it did not make (C442).
inline bool ParseSymbolArg(DevConsole& console, const std::string& arg, SpellSymbol& out) {
	if (ParseSymbol(arg, out)) return true;
	std::string ids;
	for (u32 i = 0; i < kSymbolCount; ++i) {
		if (i) ids += '/';
		ids += SymbolId(static_cast<SpellSymbol>(i));
	}
	console.Refuse("symbol must be " + ids);
	return false;
}

} // namespace dungeon::game::devargs
