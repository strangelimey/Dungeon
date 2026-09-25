// ============================================================================
// Game/GenerateDialog.h — the level generator's knobs.
//
// TWO MODES, one form (docs/level-building.md, P1):
//
//   * CREATE — opened by the editor toolbar's [+]. Makes a NEW level in the
//     viewed dungeon: generated from the knobs, or the old empty box. A
//     generated create then flips the dialog into REGENERATE on the level it
//     just made, because the first roll is rarely the keeper and the loop is the
//     point.
//   * REGENERATE — opened by the Generate button. Rough out the level you are
//     LOOKING AT, tweak, roll again. Regenerate stays put rather than closing:
//     the knobs are only useful if you can turn one and immediately see what it
//     did.
//
// Regenerating is DESTRUCTIVE and rides the editor's ordinary undo (the decision
// on record): one reroll is one Ctrl+Z. Nothing here pretends to merge with hand
// edits, so the honest workflow is to settle the rough shape first and detail it
// after.
//
// The form is built from Game/GenerateKnobs.h — tabs and rows alike — so a new
// knob is a table row, not a block of layout. The SEED is a knob like any other,
// and shown rather than hidden, because that is what makes a result you liked
// reachable again instead of a dice roll you cannot get back. "Roll" just picks
// a new one.
// ============================================================================
#pragma once

#include "Game/Generate.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Controls.h"
#include "UI/Font.h"
#include "UI/UIContext.h"

#include <functional>
#include <string>

namespace dungeon::game {

class GenerateDialog {
public:
	enum class Mode { Create, Regenerate };

	GenerateDialog(gfx::GraphicsDevice& device, ui::FontLibrary& fonts);

	bool IsOpen() const { return m_open; }
	Mode GetMode() const { return m_mode; }
	// A new level in `dungeonId` (empty = no dungeon). `where` is the line the
	// form opens with — which dungeon, below which floor — so there is no doubt
	// WHERE the level will land or what its stair will join.
	void OpenCreate(const std::string& dungeonId, const std::string& where);
	// `levelStem` is the level being rerolled, shown in the title so there is no
	// doubt WHICH level the button is about to replace.
	void OpenRegenerate(const std::string& levelStem);
	void Close() { m_open = false; }

	// The knobs, as last set. Seeded by the owner at startup from settings.ini.
	const generate::Params& Knobs() const { return m_params; }
	void SetKnobs(const generate::Params& params) { m_params = params; }

	void Update(const Input& input, float width, float height);
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

	// Reroll the viewed level with these knobs. The owner does the work (and
	// runs the check afterwards); the dialog stays open for the next tweak.
	std::function<void(const generate::Params&)> onGenerate;
	// Make a new level in `dungeonId`: generated from `params`, or the empty
	// box when `params` is null. Returns the new stem, empty on failure.
	std::function<std::string(const std::string& dungeonId,
							  const generate::Params* params)>
		onCreate;
	// The knobs were just USED (a create or a regenerate) — the moment worth
	// persisting them, rather than on every slider tick.
	std::function<void(const generate::Params&)> onKnobsUsed;

private:
	void BuildUI();

	gfx::GraphicsDevice& m_device;
	ui::UIContext m_ui;
	const gfx::Texture* m_closeIcon = nullptr; // shared, owned by AssetUtil
	bool m_open = false;
	Mode m_mode = Mode::Regenerate;
	std::string m_level;        // REGENERATE: the stem being rerolled
	std::string m_dungeon;      // CREATE: where the new level lands
	std::string m_where;        // CREATE: where it lands, said above the form
	generate::Params m_params;
	ui::TabControl* m_tabs = nullptr;   // this tree's; dies on Clear
	ui::TextField* m_seedField = nullptr; // ditto — Roll writes into it
	int m_activeTab = 0;      // survives a rebuild (the create->regenerate flip)
	bool m_uiRebuild = false; // deferred BuildUI (a callback cannot Clear itself)
};

} // namespace dungeon::game
