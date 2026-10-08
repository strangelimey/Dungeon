// ============================================================================
// Game/GameSettings.cpp — see GameSettings.h.
// ============================================================================
#include "Game/GameSettings.h"

#include "Assets/File.h"
#include "Core/AllocTrack.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Graphics/Lights.h" // kMaxPointLights (the Ultra-tier ceiling)
#include "Platform/Input.h"  // KeyName

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <iterator>

namespace dungeon::game {

// The Ultra budget IS the renderer's hard ceiling, and there is exactly one
// budget per quality tier — keep both in sync at compile time.
static_assert(kLightBudgets[std::size(kLightBudgets) - 1] == gfx::kMaxPointLights);
static_assert(std::size(kLightBudgets) == static_cast<size_t>(Quality::Ultra) + 1);

namespace {

// Reads key=r,g,b,a from the ini text. The color only applies if all four
// channels parse (a malformed line keeps the caller's default).
void ParseIniColor(const std::string& text, const std::string& key, Vec4& color) {
	const size_t pos = text.find(key);
	if (pos == std::string::npos) return;
	Vec4 parsed = color;
	size_t cursor = pos + key.size();
	for (int i = 0; i < 4; ++i) {
		float value = 0.0f;
		const auto result = std::from_chars(text.data() + cursor,
											text.data() + text.size(), value);
		if (result.ec != std::errc{}) return;
		(&parsed.x)[i] = std::clamp(value, 0.0f, 1.0f);
		cursor = static_cast<size_t>(result.ptr - text.data());
		if (i < 3) {
			if (cursor >= text.size() || text[cursor] != ',') return;
			++cursor;
		}
	}
	color = parsed;
}

// RETIRED DEFAULTS. Every colour is saved whole, so an ini written before a
// default changed holds the OLD default forever and the new one never reaches
// the player. A loaded value equal to a retired default is read as "never
// chosen" and gives way to today's default; a colour the player picked is left
// alone. Append a row whenever a colour default changes.
struct RetiredColor {
	std::string_view key; // the ini key, with its '='
	Vec4 old;
};
constexpr RetiredColor kRetiredColors[] = {
	{"theme_textdim=", {0.62f, 0.58f, 0.50f, 1.0f}}, // ui-updates: vanished on stone
	// ui-updates: the member colours, authored dark, sank into the stone.
	{"member_1=", {0.42f, 0.20f, 0.14f, 1.0f}},
	{"member_2=", {0.18f, 0.32f, 0.18f, 1.0f}},
	{"member_3=", {0.42f, 0.34f, 0.14f, 1.0f}},
	{"member_4=", {0.22f, 0.22f, 0.44f, 1.0f}},
};

// The ini holds three decimals, so "equal" is within half a step of that.
bool SameColor(const Vec4& a, const Vec4& b) {
	constexpr float kEps = 0.0015f;
	return std::abs(a.x - b.x) < kEps && std::abs(a.y - b.y) < kEps &&
		   std::abs(a.z - b.z) < kEps && std::abs(a.w - b.w) < kEps;
}

// ParseIniColor, then a retired default reverts to `color`'s own default.
void ParseIniColorRetiring(const std::string& text, const std::string& key, Vec4& color) {
	const Vec4 current = color;
	ParseIniColor(text, key, color);
	for (const RetiredColor& r : kRetiredColors)
		if (r.key == key && SameColor(color, r.old)) {
			log::Info("settings: {} was the retired default, now the new one", key);
			color = current;
		}
}

// Reads key=<float> from the ini text, clamped to [min, max]. A missing or
// malformed value keeps the caller's default.
void ParseIniFloat(const std::string& text, const std::string& key, float& value,
				   float min, float max) {
	const size_t pos = text.find(key);
	if (pos == std::string::npos) return;
	float parsed = value;
	if (std::from_chars(text.data() + pos + key.size(), text.data() + text.size(),
						parsed)
			.ec == std::errc{})
		value = std::clamp(parsed, min, max);
}

// Reads key=<integer> from the ini text into any integral target. A missing or
// malformed value keeps the caller's default.
template <typename T>
void ParseIniInt(const std::string& text, const std::string& key, T& value) {
	const size_t pos = text.find(key);
	if (pos == std::string::npos) return;
	T parsed = value;
	if (std::from_chars(text.data() + pos + key.size(), text.data() + text.size(),
						parsed)
			.ec == std::errc{})
		value = parsed;
}

// Reads key=<word> from the ini text — a NAME, not free text: it runs to the
// first character a folder name or a language code may not carry, so a trailing
// '\r' or the next line never ends up inside the value. A missing key keeps the
// caller's default.
void ParseIniString(const std::string& text, const std::string& key,
					std::string& value) {
	const size_t pos = text.find(key);
	if (pos == std::string::npos) return;
	const size_t start = pos + key.size();
	size_t end = start;
	while (end < text.size() &&
		   (std::isalnum(static_cast<unsigned char>(text[end])) ||
			text[end] == '-' || text[end] == '_'))
		++end;
	if (end > start) value = text.substr(start, end - start);
}

// Reads key=<the rest of its line> - for a value that is not a NAME: a monitor's
// device name ("\\.\DISPLAY2") or a GPU's identity, whose description has spaces
// and colons. The key must START its line, since such a value could hold
// another key's spelling. Trailing '\r' and spaces are not the value. A missing
// key keeps the caller's default; a present, empty one sets "" (auto).
void ParseIniLine(const std::string& text, const std::string& key, std::string& value) {
	for (size_t pos = text.find(key); pos != std::string::npos; pos = text.find(key, pos + 1)) {
		if (pos != 0 && text[pos - 1] != '\n') continue;
		const size_t start = pos + key.size();
		size_t end = text.find('\n', start);
		if (end == std::string::npos) end = text.size();
		while (end > start && (text[end - 1] == '\r' || text[end - 1] == ' ')) --end;
		value = text.substr(start, end - start);
		return;
	}
}

// Reads key=<0/1> from the ini text. A missing value keeps the caller's
// default; any non-'0' character reads as true.
void ParseIniBool(const std::string& text, const std::string& key, bool& value) {
	const size_t pos = text.find(key);
	if (pos == std::string::npos) return;
	const size_t i = pos + key.size();
	if (i < text.size()) value = text[i] != '0';
}

// Reads key=<x>,<y> (a HUD panel's top-left as window fractions). Both or
// neither: a half-written pair keeps the caller's (default-spot) values.
void ParseIniPair(const std::string& text, const std::string& key, float& x, float& y) {
	const size_t pos = text.find(key);
	if (pos == std::string::npos) return;
	const char* p = text.data() + pos + key.size();
	const char* end = text.data() + text.size();
	float px = 0.0f, py = 0.0f;
	const auto rx = std::from_chars(p, end, px);
	if (rx.ec != std::errc{} || rx.ptr >= end || *rx.ptr != ',') return;
	if (std::from_chars(rx.ptr + 1, end, py).ec != std::errc{}) return;
	x = std::clamp(px, -1.0f, 1.0f);
	y = std::clamp(py, -1.0f, 1.0f);
}

} // namespace

void GameSettings::Load() {
	auto bytes = assets::ReadBinaryFile(paths::ExecutableDir() + "\\settings.ini");
	if (!bytes) { // first run: keep the defaults
		RefreshKeyNames(); // ...and name their keys, as Parse's end does
		return;
	}
	Parse(std::string(bytes->begin(), bytes->end()));
}

void GameSettings::Parse(const std::string& text) {
	const size_t qpos = text.find("quality=");
	if (qpos != std::string::npos && qpos + 8 < text.size()) {
		const char digit = text[qpos + 8];
		if (digit >= '0' && digit <= '3')
			quality = static_cast<Quality>(digit - '0');
	}

	// The light budget follows the quality tier unless explicitly overridden.
	maxPointLights = QualityLightBudget(quality);
	const size_t mlpos = text.find("maxlights=");
	if (mlpos != std::string::npos) {
		int parsed = 0;
		if (std::from_chars(text.data() + mlpos + 10, text.data() + text.size(),
							parsed)
				.ec == std::errc{})
			maxPointLights = kLightBudgets[LightBudgetIndex(parsed)];
	}

	const size_t lpos = text.find("language=");
	if (lpos != std::string::npos) {
		size_t end = lpos + 9;
		while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) ||
									 text[end] == '-' || text[end] == '_'))
			++end;
		if (end > lpos + 9) language = text.substr(lpos + 9, end - (lpos + 9));
	}

	// Present interval: accept only a value the dropdown can show (else full
	// refresh).
	u32 interval = presentInterval;
	ParseIniInt(text, "presentinterval=", interval);
	presentInterval = kPresentIntervals[PresentIntervalIndex(interval)];

	ParseIniFloat(text, "volume=", volume, 0.0f, 1.0f);
	// The party bar's pair from before it floated (the hud_party_ keys below win).
	ParseIniFloat(text, "barscale=", hudParty.scale, 0.5f, 1.5f);
	ParseIniFloat(text, "baropacity=", hudParty.opacity, 0.0f, 1.0f);
	// The two docks' minimize flags from before the tray (their _hidden keys
	// below win): a dock minimized to its header strip is now minimized away.
	ParseIniBool(text, "hud_move_collapsed=", hudMove.hidden);
	ParseIniBool(text, "hud_magic_collapsed=", hudMagic.hidden);
	for (const HudPanelField& field : kHudPanelFields) {
		HudPanelLook& look = this->*(field.look);
		const std::string stem = std::string("hud_") + field.id;
		ParseIniPair(text, stem + "_pos=", look.x, look.y);
		ParseIniFloat(text, stem + "_scale=", look.scale, 0.5f, 1.5f);
		ParseIniFloat(text, stem + "_opacity=", look.opacity, 0.0f, 1.0f);
		// Only a panel that minimizes reads one: a stray key cannot hide a
		// window with no tray button to bring it back.
		if (field.glyph) ParseIniBool(text, stem + "_hidden=", look.hidden);
	}
	ParseIniBool(text, "hud_locked=", hudLocked);
	ParseIniInt(text, "hud_layout=", hudLayout);
	hudLayout = std::clamp(hudLayout, 0, 1);

	// Mouse-look feel. Durations are clamped to the slider ranges; the two curves
	// store the dropdown INDEX into kLookEaseOptions (validated before mapping).
	ParseIniFloat(text, "look_sensitivity=", look.sensitivity, 0.25f, 3.0f);
	ParseIniFloat(text, "look_hold=", look.returnHold, 0.0f, 2.0f);
	ParseIniFloat(text, "look_return=", look.returnTime, 0.2f, 5.0f);
	ParseIniFloat(text, "look_move=", look.moveTime, 0.05f, 1.5f);
	int snapEase = LookEaseIndex(look.snapEasing);
	ParseIniInt(text, "look_curve=", snapEase);
	if (snapEase >= 0 && snapEase < static_cast<int>(std::size(kLookEaseOptions)))
		look.snapEasing = kLookEaseOptions[snapEase].value;
	int moveEase = LookEaseIndex(look.moveEasing);
	ParseIniInt(text, "look_move_curve=", moveEase);
	if (moveEase >= 0 && moveEase < static_cast<int>(std::size(kLookEaseOptions)))
		look.moveEasing = kLookEaseOptions[moveEase].value;
	ParseIniBool(text, "uiskin=", uiSkin);
	ParseIniString(text, "ui_stone=", uiStone);
	ParseIniBool(text, "headbob=", headBob);
	ParseIniFloat(text, "bar_brightness=", barBrightness, 0.3f, 1.0f);
	ParseIniFloat(text, "bar_saturation=", barSaturation, 0.0f, 1.0f);
	ParseIniBool(text, "usemenu_execute=", useMenuExecutes);
	ParseIniInt(text, "spell_mru=", spellMruCount);
	spellMruCount = std::clamp(spellMruCount, 1, 10);
	ParseIniString(text, "project=", projectName);
	ParseIniBool(text, "map_palette_collapsed=", mapPaletteCollapsed);
	ParseIniBool(text, "map_legend_collapsed=", mapLegendCollapsed);
	ParseIniBool(text, "map_show_catalog=", mapShowCatalog);
	ParseIniInt(text, "map_tool=", mapTool);
	// Clamped where they are READ (MapEditor), which knows the group counts.
	ParseIniInt(text, "map_palette_group=", mapPaletteGrouping);
	ParseIniInt(text, "map_palette_stage=", mapPaletteStage);
	ParseIniInt(text, "map_palette_kind=", mapPaletteKind);
	ParseIniFloat(text, "map_palette_width=", mapPaletteWidth, 0.0f, 1.0f);
	ParseIniFloat(text, "map_legend_width=", mapLegendWidth, 0.0f, 1.0f);
	ParseIniBool(text, "map_overview_collapsed=", mapOverviewCollapsed);
	ParseIniBool(text, "map_key_collapsed=", mapKeyCollapsed);
	ParseIniInt(text, "map_overview_scope=", mapOverviewScope);
	ParseIniBool(text, "console_perf_expanded=", consolePerfExpanded);
	ParseIniBool(text, "console_profile_expanded=", consoleProfileExpanded);
	ParseIniBool(text, "console_threads_expanded=", consoleThreadsExpanded);
	// The rest of the line, verbatim: the encoding has spaces, colons and
	// points, which ParseIniString's token rule would stop at.
	if (const size_t g = text.find("gen_knobs="); g != std::string::npos) {
		const size_t start = g + 10;
		const size_t end = text.find_first_of("\r\n", start);
		generatorKnobs = text.substr(start, end == std::string::npos
												? std::string::npos
												: end - start);
	}

	ParseIniLine(text, "adapter_id=", adapterId);
	ParseIniLine(text, "monitor=", displayMonitor);
	ParseIniInt(text, "reswidth=", displayWidth);
	ParseIniInt(text, "resheight=", displayHeight);
	int fs = static_cast<int>(fullscreen);
	ParseIniInt(text, "fullscreen=", fs);
	if (fs >= 0 && fs <= 2) fullscreen = static_cast<gfx::FullscreenMode>(fs);

	for (const ThemeField& field : kThemeFields)
		ParseIniColorRetiring(text, std::format("theme_{}=", field.key),
							  theme.*(field.field));
	for (size_t i = 0; i < kMemberColorCount; ++i)
		ParseIniColorRetiring(text, std::format("member_{}=", i + 1), memberColors[i]);

	for (const KeyField& field : kKeyFields) {
		const std::string key = std::format("key_{}=", field.key);
		const size_t pos = text.find(key);
		if (pos == std::string::npos) continue;
		int vkey = 0;
		if (std::from_chars(text.data() + pos + key.size(),
							text.data() + text.size(), vkey)
					.ec == std::errc{} &&
			vkey >= 0x08 && vkey <= 0xFE) // keyboard range (no mouse codes)
			moveKeys.*(field.field) = vkey;
	}
	RefreshKeyNames(); // the Help line's names, read here rather than in play
}

namespace {
// Main thread only, like every caller of Save.
u64 g_saveCount = 0;
} // namespace

u64 GameSettings::SaveCount() { return g_saveCount; }

std::string GameSettings::Text() const {
	std::string text = std::format(
		"quality={}\nmaxlights={}\npresentinterval={}\nlanguage={}\nvolume={:.2f}\n",
		static_cast<int>(quality), maxPointLights, presentInterval, language, volume);
	for (const ThemeField& field : kThemeFields) {
		const Vec4& c = theme.*(field.field);
		text += std::format("theme_{}={:.3f},{:.3f},{:.3f},{:.3f}\n", field.key,
							c.x, c.y, c.z, c.w);
	}
	for (size_t i = 0; i < kMemberColorCount; ++i) {
		const Vec4& c = memberColors[i];
		text += std::format("member_{}={:.3f},{:.3f},{:.3f},{:.3f}\n", i + 1,
							c.x, c.y, c.z, c.w);
	}
	for (const KeyField& field : kKeyFields)
		text += std::format("key_{}={}\n", field.key, moveKeys.*(field.field));
	text += std::format(
		"look_sensitivity={:.3f}\nlook_hold={:.3f}\nlook_return={:.3f}\nlook_move={:.3f}\nlook_curve={}\nlook_move_curve={}\n",
		look.sensitivity, look.returnHold, look.returnTime, look.moveTime,
		LookEaseIndex(look.snapEasing), LookEaseIndex(look.moveEasing));
	text += std::format("uiskin={}\n", uiSkin ? 1 : 0);
	text += std::format("ui_stone={}\n", uiStone);
	text += std::format("headbob={}\n", headBob ? 1 : 0);
	text += std::format("bar_brightness={:.2f}\nbar_saturation={:.2f}\n", barBrightness,
						barSaturation);
	text += std::format("usemenu_execute={}\n", useMenuExecutes ? 1 : 0);
	text += std::format("spell_mru={}\n", spellMruCount);
	text += std::format("project={}\n", projectName);
	text += std::format(
		"map_palette_collapsed={}\nmap_legend_collapsed={}\nmap_show_catalog={}\nmap_tool={}\n",
		mapPaletteCollapsed ? 1 : 0, mapLegendCollapsed ? 1 : 0,
		mapShowCatalog ? 1 : 0, mapTool);
	text += std::format("map_palette_group={}\nmap_palette_stage={}\nmap_palette_kind={}\n",
						mapPaletteGrouping, mapPaletteStage, mapPaletteKind);
	text += std::format("map_palette_width={:.4f}\nmap_legend_width={:.4f}\n", mapPaletteWidth,
						mapLegendWidth);
	text += std::format("map_overview_collapsed={}\nmap_key_collapsed={}\nmap_overview_scope={}\n",
						mapOverviewCollapsed ? 1 : 0, mapKeyCollapsed ? 1 : 0, mapOverviewScope);
	text += std::format("console_perf_expanded={}\nconsole_profile_expanded={}\nconsole_threads_expanded={}\n",
						consolePerfExpanded ? 1 : 0, consoleProfileExpanded ? 1 : 0,
						consoleThreadsExpanded ? 1 : 0);
	for (const HudPanelField& field : kHudPanelFields) {
		const HudPanelLook& look = this->*(field.look);
		text += std::format("hud_{0}_pos={1:.4f},{2:.4f}\nhud_{0}_scale={3:.2f}\nhud_{0}_opacity={4:.2f}\n",
							field.id, look.x, look.y, look.scale, look.opacity);
		if (field.glyph) text += std::format("hud_{}_hidden={}\n", field.id, look.hidden ? 1 : 0);
	}
	text += std::format("hud_locked={}\nhud_layout={}\n", hudLocked ? 1 : 0, hudLayout);
	text += std::format("gen_knobs={}\n", generatorKnobs);
	text += std::format(
		"adapter_id={}\nmonitor={}\nreswidth={}\nresheight={}\nfullscreen={}\n",
		adapterId, displayMonitor, displayWidth, displayHeight,
		static_cast<int>(fullscreen));
	return text;
}

void GameSettings::Save() const {
	++g_saveCount;
	// Persisting a change the player just MADE - a dock minimized, a HUD panel
	// dropped where they dragged it - lands inside a frame the allocation guard
	// arms (ui-panels P3a found the second; the first was there already). It
	// formats the whole file and writes it, so it cannot be allocation-free, and
	// it runs on one click or one release, never per frame: it excuses itself,
	// as reporting code does. Anything calling Save EVERY frame would still be a
	// bug - the count of excused allocations shows it.
	const alloc::Excused excuse;
	const std::string text = Text();
	if (!assets::WriteBinaryFile(paths::ExecutableDir() + "\\settings.ini",
								 text.data(), text.size()))
		log::Warn("Could not write settings.ini");
}

const char* GameSettings::MeshSuffix() const {
	return MeshSuffixFor(quality);
}

const char* GameSettings::MeshSuffixFor(Quality q) {
	switch (q) {
	case Quality::Low:   return "low";
	case Quality::High:
	case Quality::Ultra: return "high"; // Ultra = high meshes + 4K textures
	default:             return "med";
	}
}

const char* GameSettings::TextureSuffix() const {
	switch (quality) {
	case Quality::Ultra: return "4k";
	case Quality::High:  return "2k";
	default:             return "1k";
	}
}

int GameSettings::LightBudgetIndex(int value) {
	int best = 0;
	for (int i = 1; i < static_cast<int>(std::size(kLightBudgets)); ++i)
		if (std::abs(kLightBudgets[i] - value) < std::abs(kLightBudgets[best] - value))
			best = i;
	return best;
}

int GameSettings::PresentIntervalIndex(u32 interval) {
	for (int i = 0; i < static_cast<int>(std::size(kPresentIntervals)); ++i)
		if (kPresentIntervals[i] == interval) return i;
	return 0; // unknown value → full refresh
}

const char* GameSettings::QualityLabel() const {
	switch (quality) {
	case Quality::Low:   return "Low";
	case Quality::High:  return "High";
	case Quality::Ultra: return "Ultra";
	default:             return "Medium";
	}
}

void GameSettings::RefreshKeyNames() {
	const int keys[] = {moveKeys.forward, moveKeys.back, moveKeys.strafeLeft,
						moveKeys.strafeRight, moveKeys.turnLeft, moveKeys.turnRight};
	static_assert(std::size(keys) == std::tuple_size_v<decltype(keyNames)>);
	for (size_t i = 0; i < keyNames.size(); ++i) keyNames[i] = KeyName(keys[i]);
	keyNamesFor = moveKeys;
}

loc::Line GameSettings::MoveKeysHelp() const {
	// NOT REPORTING, so no excuse (code-review C217 - it used to claim one): the
	// HUD's Help button prints this for the PLAYER on a click inside a guarded
	// frame. The names were read when the keys were bound; this only formats
	// them, into a Line, which allocates nothing.
	if (keyNamesFor != moveKeys)
		// A binding that moved without RefreshKeyNames: still the right text, read
		// afresh - and in an armed frame the guard names this line, which is how a
		// rebind path that forgot the refresh would show.
		return loc::FormatLine("log.movekeys", KeyName(moveKeys.forward),
							   KeyName(moveKeys.back), KeyName(moveKeys.strafeLeft),
							   KeyName(moveKeys.strafeRight), KeyName(moveKeys.turnLeft),
							   KeyName(moveKeys.turnRight));
	return loc::FormatLine("log.movekeys", keyNames[0], keyNames[1], keyNames[2],
						   keyNames[3], keyNames[4], keyNames[5]);
}

} // namespace dungeon::game
