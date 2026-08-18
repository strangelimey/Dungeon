// ============================================================================
// Game/GenerateDialog.h — the level generator's knobs.
//
// Opened by the editor toolbar's Generate button. Rough out the level you are
// LOOKING AT, tweak, roll again — the loop the whole feature exists for, which
// is why Regenerate stays put rather than closing the dialog: the knobs are only
// useful if you can turn one and immediately see what it did.
//
// Regenerating is DESTRUCTIVE and rides the editor's ordinary undo (the decision
// on record): one reroll is one Ctrl+Z. Nothing here pretends to merge with hand
// edits, so the honest workflow is to settle the rough shape first and detail it
// after.
//
// The SEED is a knob like any other, and shown rather than hidden, because that
// is what makes a result you liked reachable again instead of a dice roll you
// cannot get back. "Roll" just picks a new one.
// ============================================================================
#pragma once

#include "Game/Generate.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h"

#include <functional>
#include <string>

namespace dungeon::game {

class GenerateDialog {
public:
	GenerateDialog(gfx::GraphicsDevice& device, ui::FontLibrary& fonts);

	bool IsOpen() const { return m_open; }
	// `levelLabel` is the stem being rerolled, shown in the title so there is no
	// doubt WHICH level the button is about to replace.
	void Open(const std::string& levelLabel);
	void Close() { m_open = false; }

	void Update(const Input& input, float width, float height);
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

	// Roll the level with these knobs. The owner does the work (and runs the
	// check afterwards); the dialog stays open for the next tweak.
	std::function<void(const generate::Params&)> onGenerate;

private:
	void BuildUI();

	gfx::GraphicsDevice& m_device;
	ui::UIContext m_ui;
	const gfx::Texture* m_closeIcon = nullptr; // shared, owned by AssetUtil

	bool m_open = false;
	std::string m_level;
	generate::Params m_params;
	bool m_uiRebuild = false; // deferred BuildUI (a callback cannot Clear itself)
};

} // namespace dungeon::game
