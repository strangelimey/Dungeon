// ============================================================================
// Game/GameUI.cpp — see GameUI.h.
// ============================================================================
#include "Game/GameUI.h"

#include "Core/Loc.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"

#include <algorithm>
#include <format>
#include <iterator>

namespace dungeon::game {

namespace {

// Font pixel heights at the 900px-tall design window (the layouts in
// BuildMenu/BuildHud are authored against the same design size). UpdateFonts
// rescales them against the live window height so text tracks the UI.
constexpr float kFontDesignWindowH = 900.0f;
constexpr float kHudFontH = 17.0f;
constexpr float kMenuFontH = 28.0f;
constexpr float kSheetFontH = 22.0f;
constexpr float kTitleFontH = 64.0f;
// Re-bakes wait for the window height to hold still this long, so an
// interactive resize drag doesn't drain the GPU on every size change.
constexpr float kFontSettleDelay = 0.25f;

// Widget bounds are normalized fractions of their container (see Widget.h).
// Layouts here are still authored in design pixels for readability and
// converted with this helper; they scale with the live window size.
gfx::Rect Norm(const gfx::Rect& designPx, const gfx::Rect& container) {
	return {(designPx.x - container.x) / container.w,
			(designPx.y - container.y) / container.h, designPx.w / container.w,
			designPx.h / container.h};
}

} // namespace

GameUI::GameUI(Window& window, gfx::GraphicsDevice& device,
			   gfx::SpriteBatch& spriteBatch, audio::AudioEngine& audio,
			   const SoundBank& sounds, GameSettings& settings,
			   std::vector<Character>& characters)
	: m_window(window), m_device(device), m_spriteBatch(spriteBatch),
	  m_audio(audio), m_sounds(sounds), m_settings(settings),
	  m_characters(characters), m_hudUi(device, "", kHudFontH),
	  m_menuUi(device, "", kMenuFontH), m_settingsUi(device, "", kMenuFontH),
	  m_pauseUi(device, "", kMenuFontH), m_sheetUi(device, "", kSheetFontH),
	  m_titleFont(device, "", kTitleFontH) {}

void GameUI::BuildStaticUi() {
	ApplyTheme();
	BuildMenu();
	BuildPauseMenu();
	BuildCharacterSheet();
}

void GameUI::LoadTitleArt() {
	m_titleBackground = LoadTextureFile(m_device, paths::Asset("textures\\title_bg"));
}

void GameUI::ApplyTheme() {
	for (ui::UIContext* ctx :
		 {&m_hudUi, &m_menuUi, &m_settingsUi, &m_pauseUi, &m_sheetUi})
		ctx->SetTheme(m_settings.theme);
}

void GameUI::Click(float volume) { m_audio.Play(m_sounds.click, volume); }

// ============================================================================
// Landing page — title plus a MenuList; entries highlight on mouse hover or
// keyboard selection. Start New Game and Settings are wired up; Continue /
// Load / Save wait on the save system.
// ============================================================================
void GameUI::BuildMenu() {
	const float w = static_cast<float>(m_window.Width());
	const float h = static_cast<float>(m_window.Height());
	const gfx::Rect window{0, 0, w, h};

	const float menuW = 420.0f;
	const float itemH = 58.0f;
	auto* menu = m_menuUi.Add<ui::MenuList>(
		Norm({(w - menuW) * 0.5f, h * 0.42f, menuW, itemH * 4}, window),
		1.0f / 4.0f); // item height: one quarter of the list

	menu->AddItem(loc::Tr("menu.continue")); // not implemented yet
	menu->AddItem(loc::Tr("menu.start"), [this] {
		Click(0.6f);
		onStartNewGame();
	});
	menu->AddItem(loc::Tr("menu.load"));     // not implemented yet
	menu->AddItem(loc::Tr("menu.settings"), [this] {
		Click();
		m_menuPage = MenuPage::Settings;
	});

	// Settings page: Game / Video / Audio tabs over a shared page area, with
	// a Back button beneath. Tab children are fractions of the PAGE (the
	// area below the strip); each row leaves room for the 28px settings font.
	// Sized generously — settings keep accumulating. The block spans from
	// just under the subtitle (ends ~246 design px) to just above the Back
	// button, which itself must clear the 900px design window.
	const float tabsW = 760.0f;
	const float tabsH = 540.0f;
	const float stripH = 48.0f;
	const float tabsX = (w - tabsW) * 0.5f;
	const float tabsY = h * 0.29f;
	auto* tabs = m_settingsUi.Add<ui::TabControl>(
		Norm({tabsX, tabsY, tabsW, tabsH}, window), stripH / tabsH);
	const size_t tabGame = tabs->AddTab(loc::Tr("settings.tab.game"));
	const size_t tabControls = tabs->AddTab(loc::Tr("settings.tab.controls"));
	const size_t tabVideo = tabs->AddTab(loc::Tr("settings.tab.video"));
	const size_t tabAudio = tabs->AddTab(loc::Tr("settings.tab.audio"));
	const size_t tabUi = tabs->AddTab(loc::Tr("settings.tab.ui"));
	const gfx::Rect page{0, 0, tabsW, tabsH - stripH}; // child design space
	const float pad = 24.0f;
	const float rowW = page.w - 2 * pad;

	// Game: language. The language list is whatever assets/lang holds;
	// selecting one defers to Game (settings save + string reload +
	// RebuildForLanguage at the top of the next frame — rebuilding here
	// would destroy this dropdown mid-callback).
	tabs->AddChild<ui::Label>(tabGame, Norm({pad, pad, rowW, 28}, page),
							  loc::Tr("settings.language"))
		->dim = true;
	m_languages = loc::ScanLanguages(paths::Asset("lang"));
	std::vector<std::string> languageNames;
	int languageIndex = 0;
	for (size_t i = 0; i < m_languages.size(); ++i) {
		languageNames.push_back(m_languages[i].name);
		if (m_languages[i].code == m_settings.language)
			languageIndex = static_cast<int>(i);
	}
	tabs->AddChild<ui::DropDown>(
		tabGame, Norm({pad, pad + 38, rowW, 40}, page), std::move(languageNames),
		languageIndex, [this](int index) {
			Click();
			if (index >= 0 && index < static_cast<int>(m_languages.size()) &&
				m_languages[static_cast<size_t>(index)].code != m_settings.language)
				onLanguageSelected(m_languages[static_cast<size_t>(index)].code);
		});

	// Controls: movement key bindings (kKeyFields). Click a key box, press
	// the new key; binding a key another action already uses hands that
	// action the old key (swap) so the set stays conflict-free. Each rebind
	// goes straight into the Party (onKeysChanged) and persists.
	tabs->AddChild<ui::Label>(tabControls, Norm({pad, pad, rowW, 28}, page),
							  loc::Tr("settings.movement_keys"));
	m_keyBinds.clear();
	for (size_t i = 0; i < std::size(kKeyFields); ++i) {
		const KeyField& field = kKeyFields[i];
		auto* bind = tabs->AddChild<ui::KeyBind>(
			tabControls,
			Norm({pad, pad + 52 + 48.0f * static_cast<float>(i), rowW, 36}, page),
			loc::Tr(field.labelKey), m_settings.moveKeys.*(field.field),
			[this, member = field.field](int vkey) {
				Click();
				MoveKeys& keys = m_settings.moveKeys;
				const int old = keys.*member;
				for (size_t j = 0; j < std::size(kKeyFields); ++j) {
					int MoveKeys::*other = kKeyFields[j].field;
					if (other != member && keys.*other == vkey) {
						keys.*other = old;
						m_keyBinds[j]->SetKey(old);
					}
				}
				keys.*member = vkey;
				onKeysChanged();
				m_settings.Save();
			});
		bind->capturePrompt = loc::Tr("settings.press_a_key");
		m_keyBinds.push_back(bind);
	}

	// Video: quality tier (hot-swaps meshes/textures in place).
	tabs->AddChild<ui::Label>(tabVideo, Norm({pad, pad, rowW, 28}, page),
							  loc::Tr("settings.quality"))
		->dim = true;
	tabs->AddChild<ui::DropDown>(
		tabVideo, Norm({pad, pad + 38, rowW, 40}, page),
		std::vector<std::string>{
			loc::Tr("settings.quality.low"), loc::Tr("settings.quality.medium"),
			loc::Tr("settings.quality.high"), loc::Tr("settings.quality.ultra")},
		static_cast<int>(m_settings.quality), [this](int index) {
			Click();
			onQualitySelected(index);
		});

	// Audio: master volume (the slider draws its own label above the track).
	// Live while dragging; persisted once on release.
	auto* volume = tabs->AddChild<ui::Slider>(
		tabAudio, Norm({pad, pad + 38, rowW, 22}, page), loc::Tr("settings.volume"),
		0.0f, 1.0f, m_settings.volume, [this](float v) {
			m_settings.volume = v;
			m_audio.SetMasterVolume(v);
		});
	volume->onRelease = [this] { m_settings.Save(); };

	// UI → Party Bar: scale resizes the bar live (about its top center) and
	// opacity fades the slot backgrounds. Both apply while dragging and
	// persist on release; safe before the HUD exists (the panel list is empty
	// until the first game load).
	tabs->AddChild<ui::Label>(tabUi, Norm({pad, pad, rowW, 28}, page),
							  loc::Tr("settings.party_bar"));
	auto* barScale = tabs->AddChild<ui::Slider>(
		tabUi, Norm({pad, pad + 70, rowW, 22}, page), loc::Tr("settings.bar_scale"),
		0.5f, 1.5f, m_settings.partyBarScale, [this](float v) {
			m_settings.partyBarScale = v;
			ApplyPartyBarScale();
		});
	barScale->onRelease = [this] { m_settings.Save(); };
	auto* barOpacity = tabs->AddChild<ui::Slider>(
		tabUi, Norm({pad, pad + 126, rowW, 22}, page),
		loc::Tr("settings.bar_opacity"), 0.0f,
		1.0f, m_settings.partyBarOpacity, [this](float v) {
			m_settings.partyBarOpacity = v;
			for (CharacterPanel* panel : m_partyPanels)
				panel->backgroundOpacity = v;
		});
	barOpacity->onRelease = [this] { m_settings.Save(); };

	// UI → Theme Colors (kThemeFields) and Resource Bars (kBarFields): color
	// pickers, three per row. Theme edits recolor every context live
	// (ApplyTheme); bar edits show on the HUD widgets' next draw (they point
	// at the settings' barColors). Both persist once when a picker's popup
	// closes.
	const float colW = (rowW - 2 * 16.0f) / 3.0f;
	auto pickerCell = [&](size_t index, float top) {
		return Norm({pad + (colW + 16.0f) * static_cast<float>(index % 3),
					 top + 44.0f * static_cast<float>(index / 3), colW, 36.0f},
					page);
	};
	tabs->AddChild<ui::Label>(tabUi, Norm({pad, pad + 172, rowW, 28}, page),
							  loc::Tr("settings.theme_colors"));
	size_t themeIndex = 0;
	for (const ThemeField& field : kThemeFields) {
		auto* picker = tabs->AddChild<ui::ColorPicker>(
			tabUi, pickerCell(themeIndex++, pad + 216.0f), loc::Tr(field.labelKey),
			m_settings.theme.*(field.field),
			[this, member = field.field](const Vec4& color) {
				m_settings.theme.*member = color;
				ApplyTheme();
			});
		picker->onClose = [this] { m_settings.Save(); };
	}
	tabs->AddChild<ui::Label>(tabUi, Norm({pad, pad + 368, rowW, 28}, page),
							  loc::Tr("settings.resource_bars"));
	size_t barIndex = 0;
	for (const BarField& field : kBarFields) {
		auto* picker = tabs->AddChild<ui::ColorPicker>(
			tabUi, pickerCell(barIndex++, pad + 412.0f), loc::Tr(field.labelKey),
			m_settings.barColors.*(field.field),
			[this, member = field.field](const Vec4& color) {
				m_settings.barColors.*member = color;
			});
		picker->onClose = [this] { m_settings.Save(); };
	}

	const float backW = 220.0f;
	m_settingsUi.Add<ui::Button>(
		Norm({(w - backW) * 0.5f, tabsY + tabsH + 28, backW, 44}, window),
		loc::Tr("menu.back"), [this] {
			Click();
			m_menuPage = MenuPage::Main;
		});
}

// In-game pause menu (Esc while playing): same look as the landing list,
// drawn over the frozen scene under a dark wash (RenderPauseOverlay).
// Settings routes to the same shared page as the landing menu; Save/Load
// wait on the save system.
void GameUI::BuildPauseMenu() {
	const float w = static_cast<float>(m_window.Width());
	const float h = static_cast<float>(m_window.Height());
	const gfx::Rect window{0, 0, w, h};

	const float menuW = 420.0f;
	const float itemH = 58.0f;
	auto* menu = m_pauseUi.Add<ui::MenuList>(
		Norm({(w - menuW) * 0.5f, h * 0.42f, menuW, itemH * 5}, window),
		1.0f / 5.0f);
	menu->AddItem(loc::Tr("menu.save")); // not implemented yet
	menu->AddItem(loc::Tr("menu.load")); // not implemented yet
	menu->AddItem(loc::Tr("menu.settings"), [this] {
		Click();
		m_menuPage = MenuPage::Settings;
	});
	menu->AddItem(loc::Tr("menu.exit"), [this] {
		Click();
		onQuit();
	});
	menu->AddItem(loc::Tr("menu.back"), [this] {
		Click();
		onResume();
	});
}

// Character details page (clicking a party-bar portrait): the sheet widget
// draws the page itself; prev/next buttons cycle the roster and Back (or
// Esc) resumes play. Like the pause menu it overlays the frozen scene.
void GameUI::BuildCharacterSheet() {
	const float w = static_cast<float>(m_window.Width());
	const float h = static_cast<float>(m_window.Height());
	const gfx::Rect window{0, 0, w, h};

	const float sheetW = 620.0f;
	const float sheetH = 520.0f;
	const float sx = (w - sheetW) * 0.5f;
	const float sy = (h - sheetH) * 0.5f - 24.0f;
	m_sheet = m_sheetUi.Add<CharacterSheet>(Norm({sx, sy, sheetW, sheetH}, window),
											&m_titleFont, &m_settings.barColors);

	const float btnY = sy + sheetH + 16.0f;
	m_sheetUi.Add<ui::Button>(Norm({sx, btnY, 64, 40}, window), "<", [this] {
		const size_t count = m_characters.size();
		onOpenSheet((m_sheetIndex + count - 1) % count);
	});
	m_sheetUi.Add<ui::Button>(Norm({sx + sheetW - 64, btnY, 64, 40}, window), ">",
							  [this] {
								  onOpenSheet((m_sheetIndex + 1) %
											  m_characters.size());
							  });
	m_sheetUi.Add<ui::Button>(Norm({(w - 180.0f) * 0.5f, btnY, 180, 40}, window),
							  loc::Tr("menu.back"), [this] {
								  Click();
								  onResume();
							  });
}

// Rebuilds every page in the active language (loc:: was just reloaded). The
// builders re-Add into cleared contexts, so all the raw widget pointers
// (m_sheet, m_keyBinds, m_log, ...) are re-pointed here. Rebuilding the HUD
// clears the message log; the movement help line is restored so the log
// isn't empty mid-game. Deferred to the top of a frame by Game — never run
// this from inside a widget callback.
void GameUI::RebuildForLanguage() {
	m_menuUi.Clear();
	m_settingsUi.Clear();
	m_pauseUi.Clear();
	m_sheetUi.Clear();
	BuildMenu();
	BuildPauseMenu();
	BuildCharacterSheet();
	if (!m_characters.empty()) m_sheet->SetCharacter(m_characters[m_sheetIndex]);
	if (m_log) {
		m_hudUi.Clear();
		BuildHud();
		AddLogLine(m_settings.MoveKeysHelp());
		ResetHudStatus();
	}
}

void GameUI::ShowSheet(size_t index) {
	m_sheetIndex = index;
	m_sheet->SetCharacter(m_characters[index]);
}

void GameUI::RefreshSheet() { m_sheet->SetCharacter(m_characters[m_sheetIndex]); }

// ============================================================================
// HUD — authored in design pixels from the initial window size, stored as
// window fractions (Norm), so it scales with the screen. Widgets the game
// updates later are kept as raw pointers (m_log, m_compass, m_position); the
// UIContext owns all widgets.
// ============================================================================
void GameUI::BuildHud() {
	const float w = static_cast<float>(m_window.Width());
	const float h = static_cast<float>(m_window.Height());
	const gfx::Rect window{0, 0, w, h};

	// Party bar across the top: one clickable slot per member (portrait,
	// name, health/stamina/mana). Clicking opens the character sheet. The
	// slot rects come from ApplyPartyBarScale (called below) so the Settings →
	// UI scale slider can resize the bar at runtime; the layout always
	// reserves four slots so a short roster keeps its slot size.
	m_hudDesignW = w;
	m_hudDesignH = h;
	m_partyPanels.clear();
	for (size_t i = 0; i < m_characters.size() && i < 4; ++i) {
		auto* panel = m_hudUi.Add<CharacterPanel>(
			gfx::Rect{}, &m_characters[i], &m_titleFont, &m_settings.barColors,
			[this, i] { onOpenSheet(i); });
		panel->backgroundOpacity = m_settings.partyBarOpacity;
		m_partyPanels.push_back(panel);
	}
	const float barH = 96.0f; // design height at scale 1
	const float belowBar = 16.0f + barH + 16.0f; // top edge for the side panels

	// Widgets under the bar register here so ApplyPartyBarScale can shift
	// them when the bar grows or shrinks (design Y recovered from the
	// fraction Norm just stored).
	m_belowBarWidgets.clear();
	auto below = [this, h](ui::Widget* widget) {
		m_belowBarWidgets.push_back({widget, widget->bounds.y * h});
	};

	// Message log, bottom-left.
	m_log = m_hudUi.Add<ui::TextOutput>(Norm({16, h - 200, 520, 184}, window));

	// Status labels, below the party bar on the left. Their text arrives via
	// SetHudStatus before the HUD's first visible frame.
	below(m_hudUi.Add<ui::Panel>(Norm({16, belowBar, 240, 64}, window)));
	m_compass = m_hudUi.Add<ui::Label>(Norm({28, belowBar + 10, 220, 20}, window), "");
	below(m_compass);
	m_position = m_hudUi.Add<ui::Label>(Norm({28, belowBar + 34, 220, 20}, window), "");
	m_position->dim = true;
	below(m_position);

	// Options panel (torchlight + wait/help), on the left under the status
	// panel — the right edge belongs to the control panel.
	const float optTop = belowBar + 76;
	below(m_hudUi.Add<ui::Panel>(Norm({16, optTop, 240, 144}, window)));
	below(m_hudUi.Add<ui::Label>(Norm({30, optTop + 10, 212, 20}, window),
								 loc::Tr("hud.options")));

	auto* torchLabel = m_hudUi.Add<ui::Label>(
		Norm({30, optTop + 40, 212, 20}, window), loc::Tr("hud.torchlight"));
	torchLabel->dim = true;
	below(torchLabel);
	below(m_hudUi.Add<ui::DropDown>(
		Norm({30, optTop + 64, 212, 26}, window),
		std::vector<std::string>{loc::Tr("torch.warm"), loc::Tr("torch.cold"),
								 loc::Tr("torch.eerie")},
		m_torchPalette, [this](int index) {
			Click();
			m_torchPalette = index;
			onTorchPalette(index);
		}));

	below(m_hudUi.Add<ui::Button>(Norm({30, optTop + 104, 101, 28}, window),
								  loc::Tr("hud.wait"), [this] {
									  Click();
									  m_log->AddLine(loc::Tr("log.wait"));
								  }));
	below(m_hudUi.Add<ui::Button>(Norm({141, optTop + 104, 101, 28}, window),
								  loc::Tr("hud.help"), [this] {
									  Click();
									  m_log->AddLine(m_settings.MoveKeysHelp());
									  m_log->AddLine(loc::Tr("log.scroll_hint"));
								  }));

	// Control panel down the right edge, Dungeon Master style: movement
	// arrows on top, then each member's two hands, then the (future) magic
	// area. The arrows feed the same discrete Party actions as the bound
	// keys — the world's own step/turn/bump feedback covers the audio, so
	// they skip the UI click.
	const float panelW = 250.0f;
	const float px = w - panelW - 16.0f;
	const float pad = 14.0f;
	const float innerW = panelW - 2 * pad;
	const float panelBottom = h - 16.0f; // the panel runs to the window bottom
	below(m_hudUi.Add<ui::Panel>(
		Norm({px, belowBar, panelW, panelBottom - belowBar}, window)));

	// Movement arrows: turn left / forward / turn right over strafe left /
	// back / strafe right (the classic six), square, three across.
	const struct {
		const char* glyph;
		MoveAction action;
	} moves[] = {
		{"«", MoveAction::TurnLeft}, {"^", MoveAction::Forward},
		{"»", MoveAction::TurnRight}, {"<", MoveAction::StrafeLeft},
		{"v", MoveAction::Back},      {">", MoveAction::StrafeRight},
	};
	const float moveW = (innerW - 2 * 8.0f) / 3.0f;
	for (size_t i = 0; i < std::size(moves); ++i) {
		below(m_hudUi.Add<ui::Button>(
			Norm({px + pad + (moveW + 8.0f) * static_cast<float>(i % 3),
				  belowBar + 12 + (moveW + 8.0f) * static_cast<float>(i / 3),
				  moveW, moveW},
				 window),
			moves[i].glyph,
			[this, action = moves[i].action] { onMoveAction(action); }));
	}

	// Hand pairs, two members side by side per row (so the four-member
	// roster makes a 2x2 grid of sets): the name over a left and a right
	// hand box, square. The slots stay empty until items exist; clicking one
	// logs that.
	const float setW = (innerW - 8.0f) / 2.0f;
	const float handW = (setW - 4.0f) / 2.0f;
	const float setH = 20 + handW + 8;
	const float handsTop = belowBar + 12 + 2 * moveW + 8 + 14;
	for (size_t i = 0; i < m_characters.size() && i < 4; ++i) {
		const float setX = px + pad + (setW + 8.0f) * static_cast<float>(i % 2);
		const float setTop = handsTop + setH * static_cast<float>(i / 2);
		auto* name = m_hudUi.Add<ui::Label>(
			Norm({setX, setTop, setW, 18}, window), m_characters[i].name);
		name->dim = true;
		below(name);
		for (int hand = 0; hand < 2; ++hand) {
			below(m_hudUi.Add<HandSlot>(
				Norm({setX + (handW + 4.0f) * static_cast<float>(hand),
					  setTop + 20, handW, handW},
					 window),
				&m_characters[i], [this, i] {
					Click();
					m_log->AddLine(
						loc::Format("log.hands_empty", m_characters[i].name));
				}));
		}
	}
	const size_t handRows = (std::min<size_t>(m_characters.size(), 4) + 1) / 2;
	const float magicTop = handsTop + setH * static_cast<float>(handRows);

	// Reserved magic area (spellcasting comes later) — it takes whatever the
	// panel has left down to the window bottom.
	below(m_hudUi.Add<ui::Label>(Norm({px + pad, magicTop + 8, innerW, 20}, window),
								 loc::Tr("hud.magic")));
	below(m_hudUi.Add<ui::Panel>(
		Norm({px + pad, magicTop + 32, innerW, panelBottom - pad - (magicTop + 32)},
			 window)));
	auto* magicNone = m_hudUi.Add<ui::Label>(
		Norm({px + pad + 10, magicTop + 44, innerW - 20, 20}, window),
		loc::Tr("hud.magic_none"));
	magicNone->dim = true;
	below(magicNone);

	ApplyPartyBarScale();
}

// Re-derives the party-bar slot rects from the settings scale — anchored to
// the top center, so scale 1 reproduces the design layout above (16px
// margins, 10px gaps, 96px tall) — and shifts the status/options widgets by
// the bar's growth so they stay 16px clear of its bottom edge. No-op until
// BuildHud has run (the settings sliders exist from boot, the HUD only after
// a game load).
void GameUI::ApplyPartyBarScale() {
	if (m_partyPanels.empty()) return;
	const float w = m_hudDesignW;
	const float h = m_hudDesignH;
	const gfx::Rect window{0, 0, w, h};
	const float s = m_settings.partyBarScale;
	// The bar already spans the full window at scale 1, so width is pinned
	// there: scales above 1 grow the bar vertically only (the portraits and
	// resource bars follow the slot height).
	const float ws = std::min(s, 1.0f);
	const float slotGap = 10.0f * ws;
	const float slotW = (w - 2 * 16.0f - 3 * 10.0f) / 4.0f * ws;
	const float barX = (w - 4 * slotW - 3 * slotGap) * 0.5f;
	for (size_t i = 0; i < m_partyPanels.size(); ++i)
		m_partyPanels[i]->bounds =
			Norm({barX + static_cast<float>(i) * (slotW + slotGap), 16.0f, slotW,
				  96.0f * s},
				 window);

	const float shift = 96.0f * (s - 1.0f);
	for (auto& [widget, designY] : m_belowBarWidgets)
		widget->bounds.y = (designY + shift) / h;
}

// ============================================================================
// Per-frame updates
// ============================================================================

// Keeps fonts in step with the window height so text scales with the
// normalized UI. Re-bakes are debounced until the height has settled for
// kFontSettleDelay (each one drains the GPU), then run between frames (never
// while a command list records); until then text simply renders at the old
// size inside the already-scaled widgets.
void GameUI::UpdateFonts(float dt) {
	const float windowH = static_cast<float>(m_window.Height());
	if (windowH != m_fontWindowH) {
		m_fontWindowH = windowH;
		m_fontSettle = 0.0f;
	} else if (m_fontSettle < kFontSettleDelay &&
			   (m_fontSettle += dt) >= kFontSettleDelay) {
		const float fontScale = windowH / kFontDesignWindowH;
		m_hudUi.GetFont().SetHeight(kHudFontH * fontScale);
		m_menuUi.GetFont().SetHeight(kMenuFontH * fontScale);
		m_settingsUi.GetFont().SetHeight(kMenuFontH * fontScale);
		m_pauseUi.GetFont().SetHeight(kMenuFontH * fontScale);
		m_sheetUi.GetFont().SetHeight(kSheetFontH * fontScale);
		m_titleFont.SetHeight(kTitleFontH * fontScale);
	}
}

void GameUI::UpdateMenu(const Input& input) {
	MenuContext().Update(input, WindowW(), WindowH());
}

void GameUI::UpdatePause(const Input& input) {
	PauseContext().Update(input, WindowW(), WindowH());
}

void GameUI::UpdateSheet(const Input& input) {
	m_sheetUi.Update(input, WindowW(), WindowH());
}

void GameUI::UpdateHud(const Input& input) {
	m_hudUi.Update(input, WindowW(), WindowH());
}

void GameUI::SetHudStatus(int facing, int gridX, int gridZ) {
	if (facing != m_lastFacing) {
		m_lastFacing = facing;
		m_compass->text =
			loc::Format("hud.facing", loc::Tr(Party::FacingName(facing)));
	}
	if (gridX != m_lastGridX || gridZ != m_lastGridZ) {
		m_lastGridX = gridX;
		m_lastGridZ = gridZ;
		m_position->text = loc::Format("hud.position", gridX, gridZ);
	}
}

void GameUI::ResetHudStatus() { m_lastFacing = m_lastGridX = m_lastGridZ = -1; }

bool GameUI::CloseSettingsPage() {
	if (m_menuPage != MenuPage::Settings) return false;
	m_menuPage = MenuPage::Main;
	return true;
}

void GameUI::ResetToMainPage() { m_menuPage = MenuPage::Main; }

bool GameUI::KeyCaptureActive() const {
	for (const ui::KeyBind* bind : m_keyBinds)
		if (bind->IsCapturing()) return true;
	return false;
}

void GameUI::AddLogLine(const std::string& line) { m_log->AddLine(line); }

void GameUI::ClearLog() { m_log->Clear(); }

// ============================================================================
// Rendering — all 2D, inside the caller's SpriteBatch Begin/End.
// ============================================================================

// Progress bar + current step name, shared by both loading screens.
void GameUI::DrawLoadProgress(const LoadQueue& queue, float barY) {
	const float w = DeviceW();
	const float h = DeviceH();
	const ui::Theme& theme = m_menuUi.GetTheme();

	const gfx::Rect bar{w * 0.3f, barY, w * 0.4f, h * (14.0f / kFontDesignWindowH)};
	m_spriteBatch.DrawRect(bar, theme.control);
	m_spriteBatch.DrawRect({bar.x, bar.y, bar.w * queue.Progress(), bar.h},
						   theme.accent);
	ui::DrawBorder(m_spriteBatch, bar, theme.panelBorder);

	const std::string_view step = queue.CurrentLabel();
	ui::Font& font = m_menuUi.GetFont();
	const float stepW = font.MeasureWidth(step);
	font.Draw(m_spriteBatch, step, (w - stepW) * 0.5f, bar.y + bar.h * 2.0f,
			  theme.textDim);
}

// Title face, horizontally centered at y in the accent color — every title
// screen draws "DUNGEON" this way.
void GameUI::DrawCenteredTitle(const std::string& text, float y) {
	const float titleW = m_titleFont.MeasureWidth(text);
	m_titleFont.Draw(m_spriteBatch, text, (DeviceW() - titleW) * 0.5f, y,
					 m_menuUi.GetTheme().accent);
}

void GameUI::RenderLoadingScreen(const LoadQueue& queue) {
	const float h = DeviceH();
	DrawCenteredTitle(loc::Tr("title"), h * 0.32f);
	DrawLoadProgress(queue, h * 0.52f);
}

// Shown between "Start New Game" and Playing: the title art again, washed
// darker than the menu so the bar and step names read clearly.
void GameUI::RenderGameLoadingScreen(const LoadQueue& queue) {
	const float w = DeviceW();
	const float h = DeviceH();
	const ui::Theme& theme = m_menuUi.GetTheme();

	m_spriteBatch.DrawSprite({0, 0, w, h}, {0, 0, 1, 1}, *m_titleBackground,
							 {1, 1, 1, 1});
	m_spriteBatch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.55f});

	DrawCenteredTitle(loc::Tr("title"), h * 0.16f);

	const std::string subtitle = loc::Tr("loading.descending");
	ui::Font& font = m_menuUi.GetFont();
	const float subW = font.MeasureWidth(subtitle);
	font.Draw(m_spriteBatch, subtitle, (w - subW) * 0.5f,
			  h * (0.16f + 74.0f / kFontDesignWindowH), theme.textDim);

	DrawLoadProgress(queue, h * 0.56f);
}

void GameUI::RenderMenuOverlay() {
	const float w = DeviceW();
	const float h = DeviceH();
	const ui::Theme& theme = m_menuUi.GetTheme();

	// Baked title art, stretched to the window, with a light darkening wash
	// so the menu text stays readable over the bright portal.
	m_spriteBatch.DrawSprite({0, 0, w, h}, {0, 0, 1, 1}, *m_titleBackground,
							 {1, 1, 1, 1});
	m_spriteBatch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.30f});

	// Title + subtitle.
	DrawCenteredTitle(loc::Tr("title"), h * 0.16f);

	const std::string subtitle = loc::Tr(
		m_menuPage == MenuPage::Settings ? "menu.subtitle_settings" : "menu.subtitle");
	ui::Font& font = m_menuUi.GetFont();
	const float subW = font.MeasureWidth(subtitle);
	font.Draw(m_spriteBatch, subtitle, (w - subW) * 0.5f,
			  h * (0.16f + 74.0f / kFontDesignWindowH), theme.textDim);

	MenuContext().Render(m_spriteBatch, w, h);
}

// Esc pause: the frozen scene stays up behind a dark wash, with a menu list
// like the landing page. The settings page is the same one the landing menu
// uses (m_menuPage routes both).
void GameUI::RenderPauseOverlay() {
	const float w = DeviceW();
	const float h = DeviceH();
	const ui::Theme& theme = m_pauseUi.GetTheme();

	m_spriteBatch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.55f});

	DrawCenteredTitle(loc::Tr("pause.title"), h * 0.16f);

	if (m_menuPage == MenuPage::Settings) {
		const std::string subtitle = loc::Tr("menu.subtitle_settings");
		ui::Font& font = m_pauseUi.GetFont();
		const float subW = font.MeasureWidth(subtitle);
		font.Draw(m_spriteBatch, subtitle, (w - subW) * 0.5f,
				  h * (0.16f + 74.0f / kFontDesignWindowH), theme.textDim);
	}

	PauseContext().Render(m_spriteBatch, w, h);
}

// Portrait click: the frozen scene under a dark wash, with the sheet page
// (and its prev/next/Back buttons) on top.
void GameUI::RenderCharacterSheetOverlay() {
	const float w = DeviceW();
	const float h = DeviceH();

	m_spriteBatch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.55f});
	m_sheetUi.Render(m_spriteBatch, w, h);
}

void GameUI::RenderHud() {
	m_hudUi.Render(m_spriteBatch, DeviceW(), DeviceH());
}

} // namespace dungeon::game
