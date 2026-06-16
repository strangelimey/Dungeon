// ============================================================================
// Game/GameSettings.cpp — see GameSettings.h.
// ============================================================================
#include "Game/GameSettings.h"

#include "Assets/File.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Graphics/Lights.h" // kMaxPointLights (the Ultra-tier ceiling)
#include "Platform/Input.h"  // KeyName

#include <algorithm>
#include <cctype>
#include <charconv>
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

// Reads key=<0/1> from the ini text. A missing value keeps the caller's
// default; any non-'0' character reads as true.
void ParseIniBool(const std::string& text, const std::string& key, bool& value) {
	const size_t pos = text.find(key);
	if (pos == std::string::npos) return;
	const size_t i = pos + key.size();
	if (i < text.size()) value = text[i] != '0';
}

} // namespace

void GameSettings::Load() {
	auto bytes = assets::ReadBinaryFile(paths::ExecutableDir() + "\\settings.ini");
	if (!bytes) return; // first run: keep the defaults
	const std::string text(bytes->begin(), bytes->end());

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
	ParseIniFloat(text, "barscale=", partyBarScale, 0.5f, 1.5f);
	ParseIniFloat(text, "baropacity=", partyBarOpacity, 0.0f, 1.0f);
	ParseIniBool(text, "map_palette_collapsed=", mapPaletteCollapsed);
	ParseIniBool(text, "map_legend_collapsed=", mapLegendCollapsed);
	ParseIniBool(text, "map_player_key_collapsed=", mapPlayerKeyCollapsed);

	ParseIniInt(text, "adapter=", adapterLuid);
	ParseIniInt(text, "output=", displayOutput);
	ParseIniInt(text, "reswidth=", displayWidth);
	ParseIniInt(text, "resheight=", displayHeight);
	int fs = static_cast<int>(fullscreen);
	ParseIniInt(text, "fullscreen=", fs);
	if (fs >= 0 && fs <= 2) fullscreen = static_cast<gfx::FullscreenMode>(fs);

	for (const ThemeField& field : kThemeFields)
		ParseIniColor(text, std::format("theme_{}=", field.key),
					  theme.*(field.field));
	for (const BarField& field : kBarFields)
		ParseIniColor(text, std::format("bar_{}=", field.key),
					  barColors.*(field.field));

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
}

void GameSettings::Save() const {
	std::string text = std::format(
		"quality={}\nmaxlights={}\npresentinterval={}\nlanguage={}\nvolume={:.2f}\nbarscale={:.2f}\nbaropacity={:.2f}\n",
		static_cast<int>(quality), maxPointLights, presentInterval, language, volume,
		partyBarScale, partyBarOpacity);
	for (const ThemeField& field : kThemeFields) {
		const Vec4& c = theme.*(field.field);
		text += std::format("theme_{}={:.3f},{:.3f},{:.3f},{:.3f}\n", field.key,
							c.x, c.y, c.z, c.w);
	}
	for (const BarField& field : kBarFields) {
		const Vec4& c = barColors.*(field.field);
		text += std::format("bar_{}={:.3f},{:.3f},{:.3f},{:.3f}\n", field.key,
							c.x, c.y, c.z, c.w);
	}
	for (const KeyField& field : kKeyFields)
		text += std::format("key_{}={}\n", field.key, moveKeys.*(field.field));
	text += std::format(
		"map_palette_collapsed={}\nmap_legend_collapsed={}\nmap_player_key_collapsed={}\n",
		mapPaletteCollapsed ? 1 : 0, mapLegendCollapsed ? 1 : 0,
		mapPlayerKeyCollapsed ? 1 : 0);
	text += std::format(
		"adapter={}\noutput={}\nreswidth={}\nresheight={}\nfullscreen={}\n",
		adapterLuid, displayOutput, displayWidth, displayHeight,
		static_cast<int>(fullscreen));
	if (!assets::WriteBinaryFile(paths::ExecutableDir() + "\\settings.ini",
								 text.data(), text.size()))
		log::Warn("Could not write settings.ini");
}

const char* GameSettings::MeshSuffix() const {
	switch (quality) {
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

std::string GameSettings::MoveKeysHelp() const {
	return loc::Format("log.movekeys", KeyName(moveKeys.forward),
					   KeyName(moveKeys.back), KeyName(moveKeys.strafeLeft),
					   KeyName(moveKeys.strafeRight), KeyName(moveKeys.turnLeft),
					   KeyName(moveKeys.turnRight));
}

} // namespace dungeon::game
