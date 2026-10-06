// ============================================================================
// Game/Game_MapIcons.cpp - the monster map icons, read and shown: the
// `mapicons` dev command and its survey overlay (code-review C183 / C189).
//
// A kind's map icon is its model baked once into a small target - a head shot
// in the pose the asset picker's tile of the same model shows. Two things about
// it are worth seeing from outside. WHAT each icon's bake drew - the pose and the
// box, as the bake itself recorded them - and what its skinning palettes did:
// the readout, which a harness judges (tools\EditorTest.py). And how the icons
// LOOK beside the picker's own tiles - the survey, which a person judges: the
// overlay draws the two side by side, the tile being the picker's real one
// (AssetPicker::TileImage), never a re-bake. The survey is a dev overlay over
// everything, so its frames are no steady state (Game::SteadyStateFrame).
// ============================================================================
#include "Game/Game.h"

#include "Core/Log.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace dungeon::game {

namespace {
// "w x h" of a box: its widest horizontal extent, then its height.
std::string BoxText(const Vec3& lo, const Vec3& hi) {
	return std::format("{:.3f}x{:.3f}", std::max(hi.x - lo.x, hi.z - lo.z), hi.y - lo.y);
}
} // namespace

void Game::RegisterMapIconCommands() {
	m_console.Register(
		{.name = "mapicons",
		 .group = CmdGroup::Monsters,
		 .params = "[status]\nall\nrebake\nsurvey on|off",
		 .summary = "the monster map icons: each one's pose and frame, the bake's palettes, a survey"},
		[this](const std::vector<std::string>& args) {
			const std::string sub = args.empty() ? "status" : args[0];
			if (sub == "all") {
				// Every catalog kind whose model is installed: their icons then bake
				// together, in one pass, on the next rendered frame.
				int loaded = 0, missing = 0;
				for (const CatalogEntry& e : m_project.monsters.Entries())
					(m_world->LoadMonsterKind(e.id) ? loaded : missing) += 1;
				m_world->RebakeMonsterIcons();
				m_console.Print(std::format("mapicons all: {} kinds loaded, {} without an "
											"installed model; they bake next frame",
											loaded, missing));
				return;
			}
			if (sub == "rebake") {
				m_world->RebakeMonsterIcons();
				m_console.Print("mapicons: every loaded kind bakes again next frame");
				return;
			}
			if (sub == "survey") {
				if (args.size() < 2 || (args[1] != "on" && args[1] != "off")) {
					m_console.RefuseUsage();
					return;
				}
				m_mapIconSurvey = args[1] == "on";
				int kinds = 0, icons = 0, tiles = 0;
				for (const CatalogEntry& e : m_project.monsters.Entries()) {
					++kinds;
					icons += m_world->MonsterIconFor(e.id) != nullptr;
					tiles += m_assetPicker.TileImage(e.Get("model", e.id)) != nullptr;
				}
				m_console.Print(std::format("mapicons survey {}: {} kinds, {} with a map icon, "
											"{} with a picker tile",
											args[1], kinds, icons, tiles));
				return;
			}
			if (sub != "status") {
				m_console.RefuseUsage();
				return;
			}
			// pose / frame / palette are what the bake DREW (MonsterKind::iconDrawn);
			// bind and posed (the idle's first frame) are measured afresh beside
			// them - `palette=same` says the drawn palette IS that fresh pose,
			// `other` that the bake drew some other pose (or none).
			for (const DungeonWorld::MonsterIconInfo& i : m_world->MonsterIconReport())
				m_console.Print(std::format("mapicon {} joints={} pose={} bind={} frame={} posed={} "
											"palette={} baked={}",
											i.type, i.joints, i.pose.empty() ? "rest" : i.pose,
											BoxText(i.bindLo, i.bindHi),
											BoxText(i.frameLo, i.frameHi),
											BoxText(i.posedLo, i.posedHi),
											i.paletteIsPose ? "same" : "other", i.baked ? 1 : 0));
			const DungeonWorld::MonsterIconBake& b = m_world->LastMonsterIconBake();
			m_console.Print(std::format("mapicons bake passes={} kinds={} skinned={} uploads={} "
										"reuses={}",
										b.passes, b.kinds, b.skinned, b.uploads, b.reuses));
		});
}

// One card per catalog kind, in catalog order: its map icon, then the picker's
// tile of its model, the id and the pose under them. A missing image is a dark
// square saying why - the kind not loaded (`mapicons all`), or the tile not
// baked (open the picker on the model: `assetpicker models` + `assetpicker
// only <model>...`).
void Game::DrawMapIconSurvey(float dw, float dh) {
	const ui::Font& font = m_mapView.Font();
	const ui::Theme& th = m_settings.theme;
	m_spriteBatch.DrawRect({0, 0, dw, dh}, {0.03f, 0.03f, 0.04f, 0.94f});

	const float margin = std::round(dh * 0.03f);
	const float side = std::clamp(std::round(dh * 0.13f), 64.0f, 192.0f);
	const float gap = std::round(side * 0.06f);
	const float cardW = 2.0f * side + gap;
	const float cardH = side + 2.0f * font.Height() + 3.0f * gap;
	const float gutter = std::round(side * 0.25f);
	const int cols = std::max(1, static_cast<int>((dw - 2.0f * margin + gutter) / (cardW + gutter)));

	font.Draw(m_spriteBatch, "Monster map icons (left) beside the asset picker's tile of the model (right)",
			  margin, margin, th.accent);
	const float top = margin + 2.0f * font.Height();

	int n = 0;
	for (const CatalogEntry& e : m_project.monsters.Entries()) {
		const float x = margin + static_cast<float>(n % cols) * (cardW + gutter);
		const float y = top + static_cast<float>(n / cols) * (cardH + gutter);
		++n;
		const gfx::Rect iconBox{x, y, side, side}, tileBox{x + side + gap, y, side, side};
		auto image = [&](const gfx::Rect& box, const gfx::Texture* tex, const char* why) {
			if (tex) {
				m_spriteBatch.DrawRect(box, {0.16f, 0.16f, 0.18f, 1.0f});
				m_spriteBatch.DrawSprite(box, {0, 0, 1, 1}, *tex, {1, 1, 1, 1});
			} else {
				m_spriteBatch.DrawRect(box, {0.10f, 0.10f, 0.12f, 1.0f});
				font.Draw(m_spriteBatch, why, box.x + gap, box.y + gap, th.textDim);
			}
		};
		const gfx::Texture* icon = m_world->MonsterIconFor(e.id);
		image(iconBox, icon, "not loaded");
		image(tileBox, m_assetPicker.TileImage(e.Get("model", e.id)), "no tile");
		// The clip the bake drew the icon in - a lookup, not MonsterIconReport,
		// which skins every vertex of every kind and would do so every frame.
		const std::string_view clip = m_world->MonsterIconPose(e.id);
		const std::string_view pose = !icon ? "-" : clip.empty() ? "rest pose" : clip;
		font.Draw(m_spriteBatch, e.id + "  (" + e.Get("model", e.id) + ")", x, y + side + gap,
				  th.text);
		font.Draw(m_spriteBatch, pose, x, y + side + gap + font.Height(), th.textDim);
	}
}

} // namespace dungeon::game
