// ============================================================================
// Game/DevConsole_Commands.cpp - the registry's reading side: `help` and the
// type-ahead (see DevConsole.h).
//
// Both answer the same question - "what commands are there, and what do they
// take?" - from the same four fields every registration carries (name, group,
// params, summary), so the listing and the box above the prompt can never
// describe a command two different ways.
// ============================================================================
#include "Game/DevConsole_Panel.h"

#include "Core/Assert.h"
#include "UI/Controls.h" // ui::DrawBorder

#include <Windows.h> // VK_* codes

#include <algorithm>
#include <array>
#include <format>

namespace dungeon::game {

using namespace devcon;

namespace {

// Each group's `help` key and heading, in CmdGroup order.
struct GroupInfo {
	std::string_view key;
	std::string_view title;
};
constexpr std::array<GroupInfo, static_cast<size_t>(CmdGroup::Count)> kGroups{{
	{"console", "Console"},
	{"settings", "Settings"},
	{"rendering", "Rendering"},
	{"save", "Save and load"},
	{"party", "Party and movement"},
	{"characters", "Characters and items"},
	{"combat", "Combat and magic"},
	{"monsters", "Monsters and AI"},
	{"sim", "Simulation and harness"},
	{"levels", "Levels and editor"},
	{"types", "Catalog types"},
	{"world", "World map"},
	{"diagnostics", "Diagnostics"},
	{"threads", "Threads"},
	{"profiling", "Profiling"},
}};

const GroupInfo& Group(CmdGroup g) { return kGroups[static_cast<size_t>(g)]; }

// The listing's column caps, in characters (the console font is Mono). A name
// or a first form longer than its cap still prints whole; it just pushes its
// summary onto the next line rather than out of the column.
constexpr size_t kNameCap = 16;
constexpr size_t kParamsCap = 34;
// Rows the type-ahead list shows before it says "+N more".
constexpr int kSuggestRows = 10;

std::string Lower(std::string_view s) {
	std::string out(s);
	std::ranges::transform(out, out.begin(), [](char c) {
		return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	});
	return out;
}

// The '\n'-separated forms of a params string. None for a command that takes
// no arguments.
std::vector<std::string_view> Forms(std::string_view params) {
	std::vector<std::string_view> out;
	while (!params.empty()) {
		const size_t nl = params.find('\n');
		out.push_back(params.substr(0, nl));
		if (nl == std::string_view::npos) break;
		params.remove_prefix(nl + 1);
	}
	return out;
}

std::string Pad(std::string_view s, size_t width) {
	std::string out(s);
	if (out.size() < width) out.append(width - out.size(), ' ');
	return out;
}

} // namespace

// --- the registry ------------------------------------------------------------

void DevConsole::Register(const CmdInfo& info,
						  std::function<void(const std::vector<std::string>&)> fn) {
	DN_ASSERT(!info.name.empty(), "console command registered with no name");
	DN_ASSERT(Lower(info.name) == info.name,
			  std::format("console command '{}' must be lowercase (Execute lowercases "
						  "what is typed)",
						  info.name));
	// Execute runs the FIRST match, so a second registration is dead code that
	// looks alive in `help` - the per-member `threat` was, for months.
	DN_ASSERT(!FindCommand(info.name),
			  std::format("console command '{}' registered twice", info.name));
	DN_ASSERT(!info.summary.empty() && info.summary.find('\n') == std::string_view::npos,
			  std::format("console command '{}' needs a one-line summary", info.name));
	DN_ASSERT(info.group < CmdGroup::Count,
			  std::format("console command '{}' has no group", info.name));
	// Only the FIRST form may be empty (the bare command); an empty one anywhere
	// else is a stray newline and would print as a blank usage line.
	{
		const std::vector<std::string_view> forms = Forms(info.params);
		for (size_t i = 1; i < forms.size(); ++i)
			DN_ASSERT(!forms[i].empty(),
					  std::format("console command '{}' has an empty params form", info.name));
		DN_ASSERT(!(forms.size() == 1 && forms[0].empty()),
				  std::format("console command '{}': params \"\" means no arguments, not "
							  "an empty form",
							  info.name));
	}
	m_commands.push_back({std::string(info.name), info.group, std::string(info.params),
						  std::string(info.summary), std::move(fn)});
}

const DevConsole::Command* DevConsole::FindCommand(std::string_view name) const {
	for (const Command& cmd : m_commands)
		if (cmd.name == name) return &cmd;
	return nullptr;
}

void DevConsole::RefuseUsage() {
	if (!m_running) {
		Refuse("usage: (no command is running)");
		return;
	}
	const std::vector<std::string_view> forms = Forms(m_running->params);
	if (forms.empty()) {
		Refuse(std::format("usage: {}  (takes no arguments)", m_running->name));
		return;
	}
	// One refusal, however many forms: the flag is what a script counts.
	Refuse(std::format("usage: {} {}", m_running->name, forms[0]));
	for (size_t i = 1; i < forms.size(); ++i)
		Print(std::format("       {} {}", m_running->name, forms[i]));
}

// --- help ------------------------------------------------------------------

void DevConsole::RegisterHelp() {
	Register({.name = "help",
			  .group = CmdGroup::Console,
			  .params = "[group|command|word]",
			  .summary = "list commands: all, one group, one command in full, or a search"},
			 [this](const std::vector<std::string>& args) { Help(args); });
}

void DevConsole::Help(const std::vector<std::string>& args) {
	std::string groupKeys;
	for (const GroupInfo& g : kGroups)
		groupKeys += (groupKeys.empty() ? "" : " ") + std::string(g.key);

	if (args.empty()) {
		std::vector<const Command*> all;
		for (const Command& cmd : m_commands) all.push_back(&cmd);
		Print(std::format("{} commands. Start typing one to see matches (Tab or Enter "
						  "completes).",
						  all.size()));
		PrintCommandTable(all);
		// LAST, because the newest line is the one on screen: after a listing this
		// long, how to narrow it is what you need to read first.
		Print("help <group> for one group:  " + groupKeys);
		Print("help <command> for one command in full; help <word> searches names and summaries");
		return;
	}

	std::string word;
	for (const std::string& a : args) word += (word.empty() ? "" : " ") + Lower(a);

	// A group first, then a command - and a word that is BOTH (`party`, `world`)
	// gets both, the command's detail last so it is the part on screen.
	const Command* named = FindCommand(word);
	for (size_t g = 0; g < kGroups.size(); ++g) {
		if (kGroups[g].key != word && Lower(kGroups[g].title) != word) continue;
		std::vector<const Command*> in;
		for (const Command& cmd : m_commands)
			if (static_cast<size_t>(cmd.group) == g) in.push_back(&cmd);
		PrintCommandTable(in);
		if (named) PrintCommandDetail(*named);
		return;
	}
	if (named) {
		PrintCommandDetail(*named);
		return;
	}

	// A search: the name OR the summary, so `help spawn` finds what places a
	// monster whatever it happens to be called.
	std::vector<const Command*> hits;
	for (const Command& cmd : m_commands)
		if (cmd.name.find(word) != std::string::npos ||
			Lower(cmd.summary).find(word) != std::string::npos)
			hits.push_back(&cmd);
	if (hits.empty()) {
		Print(std::format("nothing matches '{}'. Groups: {}", word, groupKeys));
		return;
	}
	Print(std::format("{} match{} for '{}':", hits.size(), hits.size() == 1 ? "" : "es", word));
	PrintCommandTable(hits);
}

void DevConsole::PrintCommandTable(const std::vector<const Command*>& cmds) {
	// Column widths from what is being shown, capped, and SHARED across the
	// groups so the whole listing is one grid.
	size_t nameW = 0, paramsW = 0;
	for (const Command* c : cmds) {
		nameW = std::max(nameW, std::min(c->name.size(), kNameCap));
		const std::vector<std::string_view> forms = Forms(c->params);
		if (!forms.empty()) paramsW = std::max(paramsW, std::min(forms[0].size(), kParamsCap));
	}
	const std::string indent = "  ";
	const size_t summaryCol = indent.size() + nameW + 2 + paramsW + 2;

	auto row = [&](std::string_view name, std::string_view params, std::string_view summary) {
		OutLine line;
		line.style = LineStyle::Row;
		line.text = indent + Pad(name, nameW) + "  ";
		line.nameEnd = static_cast<u16>(line.text.size());
		line.text += params;
		if (!summary.empty()) {
			line.text = Pad(line.text, summaryCol - 2) + "  ";
		}
		line.paramsEnd = static_cast<u16>(line.text.size());
		line.text += summary;
		PrintLine(std::move(line));
	};

	std::vector<const Command*> sorted = cmds;
	std::ranges::sort(sorted, [](const Command* a, const Command* b) {
		if (a->group != b->group) return a->group < b->group;
		return a->name < b->name;
	});

	CmdGroup current = CmdGroup::Count;
	for (const Command* c : sorted) {
		if (c->group != current) {
			current = c->group;
			const GroupInfo& g = Group(current);
			std::string head = std::format("-- {} (help {}) ", g.title, g.key);
			if (head.size() < 64) head.append(64 - head.size(), '-');
			PrintLine({.text = std::move(head), .style = LineStyle::Header});
		}
		const std::vector<std::string_view> forms = Forms(c->params);
		const std::string_view first = forms.empty() ? std::string_view{} : forms[0];
		if (c->name.size() <= nameW && first.size() <= paramsW) {
			row(c->name, first, c->summary);
		} else {
			// Too wide for its columns: the summary drops to its own line, still in
			// the summary column, rather than anything being cut.
			row(c->name, first, {});
			row({}, std::string(paramsW, ' '), c->summary);
		}
		for (size_t i = 1; i < forms.size(); ++i) row({}, forms[i], {});
	}
}

void DevConsole::PrintCommandDetail(const Command& cmd) {
	const GroupInfo& g = Group(cmd.group);
	PrintLine({.text = std::format("{}  ({}, help {})", cmd.name, g.title, g.key),
			   .style = LineStyle::Header});
	Print("  " + cmd.summary);
	const std::vector<std::string_view> forms = Forms(cmd.params);
	if (forms.empty()) {
		Print(std::format("  usage: {}  (takes no arguments)", cmd.name));
		return;
	}
	for (size_t i = 0; i < forms.size(); ++i) {
		OutLine line;
		line.style = LineStyle::Row;
		line.text = i == 0 ? "  usage: " : "         ";
		line.text += cmd.name;
		line.nameEnd = static_cast<u16>(line.text.size());
		line.text += " ";
		line.text += forms[i];
		line.paramsEnd = static_cast<u16>(line.text.size());
		PrintLine(std::move(line));
	}
}

// --- type-ahead --------------------------------------------------------------

bool DevConsole::EditingName() const {
	return !m_input.empty() && m_input.find(' ') == std::string::npos;
}

void DevConsole::RefreshSuggestions() {
	if (m_input == m_suggestFor) return;
	m_suggestFor = m_input;
	m_suggest.clear();
	m_suggestPrefixCount = 0;
	if (EditingName()) {
		const std::string word = Lower(m_input);
		// Prefix matches first - they are what the word is most likely the start
		// of - then anything merely CONTAINING it, so `guard` also offers
		// allocguard and pipelineguard. Alphabetical within each.
		std::vector<const Command*> contains;
		for (const Command& cmd : m_commands) {
			if (cmd.name.starts_with(word)) m_suggest.push_back(&cmd);
			else if (cmd.name.find(word) != std::string::npos) contains.push_back(&cmd);
		}
		auto byName = [](const Command* a, const Command* b) { return a->name < b->name; };
		std::ranges::sort(m_suggest, byName);
		std::ranges::sort(contains, byName);
		m_suggestPrefixCount = static_cast<int>(m_suggest.size());
		m_suggest.insert(m_suggest.end(), contains.begin(), contains.end());
	}
	m_suggestSel = std::clamp(m_suggestSel, 0, std::max(0, static_cast<int>(m_suggest.size()) - 1));
}

bool DevConsole::SuggestListVisible() const {
	return m_suggestOpen && EditingName() && !m_suggest.empty();
}

void DevConsole::AcceptSuggestion() {
	if (m_suggest.empty()) return;
	m_input = m_suggest[static_cast<size_t>(m_suggestSel)]->name + " ";
	m_suggestOpen = false;
	m_suggestSel = 0;
	m_historyIndex = -1;
	m_caretBlink = 0.0f;
}

bool DevConsole::UpdateSuggest(const Input& input) {
	if (!EditingName()) return false;

	// Tab always belongs to the type-ahead while a name is being typed, even with
	// the list shut (after a recall): it completes to the highlighted match.
	if (input.WasKeyPressed(VK_TAB)) {
		AcceptSuggestion();
		return true;
	}
	if (!SuggestListVisible()) return false;

	const int n = static_cast<int>(m_suggest.size());
	if (input.WasKeyPressed(VK_UP)) {
		m_suggestSel = (m_suggestSel + n - 1) % n;
		return true;
	}
	if (input.WasKeyPressed(VK_DOWN)) {
		m_suggestSel = (m_suggestSel + 1) % n;
		return true;
	}
	if (input.WasKeyPressed(VK_ESCAPE)) {
		m_suggestOpen = false;
		return true;
	}
	return false;
}

bool DevConsole::SuggestTakesEnter() {
	RefreshSuggestions(); // the line as it stands at THIS Enter, not the frame's end
	if (!EditingName() || !SuggestListVisible()) return false;
	// Typed a whole name and left it highlighted: run it, as Enter always did.
	const Command* pick = m_suggest[static_cast<size_t>(m_suggestSel)];
	if (pick->name == Lower(m_input)) return false;
	// Otherwise Enter takes the highlighted command into the line. One that
	// takes no arguments has nothing left to type, so it runs on the same
	// press rather than asking for a second.
	AcceptSuggestion();
	return !pick->params.empty();
}

void DevConsole::DrawSuggest(gfx::SpriteBatch& batch, float width, float inputY, float line,
							 float pad, float labelX) {
	// The two shapes the box takes: the LIST while the first word is typed, the
	// HINT (that command's params) once it is followed by a space.
	struct Row {
		std::string text;
		size_t nameEnd = 0, paramsEnd = 0;
		bool selected = false;
		bool weak = false; // a contains-match, drawn dimmer than a prefix one
	};
	std::vector<Row> rows;
	std::string footer;

	const float charW = std::max(1.0f, m_font->MeasureWidth("M"));
	const size_t maxChars =
		static_cast<size_t>(std::max(8.0f, (width - labelX * 2.0f - pad * 2.0f) / charW));

	if (SuggestListVisible()) {
		const int n = static_cast<int>(m_suggest.size());
		const int shown = std::min(n, kSuggestRows);
		// Scrolled so the selection is always on screen.
		const int first = std::clamp(m_suggestSel - shown + 1, 0, n - shown);
		size_t nameW = 0, paramsW = 0;
		for (int i = first; i < first + shown; ++i) {
			const Command* c = m_suggest[static_cast<size_t>(i)];
			nameW = std::max(nameW, c->name.size());
			const std::vector<std::string_view> forms = Forms(c->params);
			if (!forms.empty()) paramsW = std::max(paramsW, std::min(forms[0].size(), kParamsCap));
		}
		for (int i = first; i < first + shown; ++i) {
			const Command* c = m_suggest[static_cast<size_t>(i)];
			const std::vector<std::string_view> forms = Forms(c->params);
			std::string params(forms.empty() ? std::string_view{} : forms[0]);
			// The list is a preview: a long first form is cut here and shown whole
			// by the hint once the command is taken.
			if (params.size() > kParamsCap) params = params.substr(0, kParamsCap - 3) + "...";
			if (forms.size() > 1 && params.size() + 4 <= kParamsCap) params += " ...";
			Row r;
			r.text = Pad(c->name, nameW) + "  ";
			r.nameEnd = r.text.size();
			r.text += Pad(params, paramsW) + "  ";
			r.paramsEnd = r.text.size();
			r.text += c->summary;
			r.selected = i == m_suggestSel;
			r.weak = i >= m_suggestPrefixCount;
			rows.push_back(std::move(r));
		}
		if (n > shown)
			footer = std::format("{} of {} - keep typing to narrow, Up/Down to move", shown, n);
		else
			footer = "Tab or Enter completes, Up/Down move, Esc closes";
	} else if (!m_input.empty() && !EditingName()) {
		const size_t sp = m_input.find(' ');
		const Command* c = FindCommand(Lower(std::string_view(m_input).substr(0, sp)));
		if (!c) return;
		const std::vector<std::string_view> forms = Forms(c->params);
		if (forms.empty()) {
			Row r;
			r.text = c->name + "  ";
			r.nameEnd = r.paramsEnd = r.text.size();
			r.text += c->summary + "  (takes no arguments)";
			rows.push_back(std::move(r));
		}
		for (size_t i = 0; i < forms.size(); ++i) {
			Row r;
			r.text = c->name + " ";
			r.nameEnd = r.text.size();
			r.text += forms[i];
			r.paramsEnd = r.text.size();
			if (i == 0) r.text += "    " + c->summary;
			rows.push_back(std::move(r));
		}
	}
	if (rows.empty()) return;

	size_t longest = footer.size();
	for (Row& r : rows) {
		if (r.text.size() > maxChars) r.text = r.text.substr(0, maxChars);
		longest = std::max(longest, r.text.size());
	}
	longest = std::min(longest, maxChars);

	const int lines = static_cast<int>(rows.size()) + (footer.empty() ? 0 : 1);
	const float boxW = charW * static_cast<float>(longest) + pad * 2.0f;
	const float boxH = line * static_cast<float>(lines) + pad;
	const float boxBottom = inputY - pad * 0.5f;
	const gfx::Rect box{labelX - pad, boxBottom - boxH, boxW, boxH};
	batch.DrawRect(box, kPerfBg);
	ui::DrawBorder(batch, box, kBorder);

	float y = box.y + pad * 0.5f;
	for (const Row& r : rows) {
		const std::string_view t = r.text;
		const size_t a = std::min(r.nameEnd, t.size());
		const size_t b = std::clamp(r.paramsEnd, a, t.size());
		// The panel's own grey is too close to the box to find at a glance, so the
		// selection is a lifted band plus an accent bar at its left edge.
		if (r.selected) {
			batch.DrawRect({box.x + 1.0f, y, box.w - 2.0f, line}, kSelectBg);
			batch.DrawRect({box.x + 1.0f, y, 3.0f, line}, kAccent);
		}
		m_font->Draw(batch, t.substr(0, a), labelX, y, r.weak ? kDim : kAccent);
		m_font->Draw(batch, t.substr(a, b - a), labelX + charW * static_cast<float>(a), y, kText);
		m_font->Draw(batch, t.substr(b), labelX + charW * static_cast<float>(b), y, kDim);
		y += line;
	}
	if (!footer.empty()) m_font->Draw(batch, footer, labelX, y, kDim);
}

} // namespace dungeon::game
