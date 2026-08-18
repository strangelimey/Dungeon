// ============================================================================
// Game/ValidateDialog.h — the playability check's report.
//
// Opened by the editor toolbar's Check button (and after a generate, once there
// is one). Lists what Game/Validate.h found, worst first, and CLICKING A ROW
// JUMPS THE MAP TO IT — which is most of the value: a fault you cannot navigate
// to is a fault you will not fix, and "showcase @13,19" is a coordinate a person
// then has to hunt for by hand.
//
// It knows nothing about how the findings were reached: it takes a list of
// validate::Issue and renders it. The severity split it does understand, because
// an ERROR means the dungeon cannot be completed and a WARNING means something
// looks wrong but may be deliberate — a one-way pit being the standing example.
// ============================================================================
#pragma once

#include "Game/Validate.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h"

#include <functional>
#include <string>
#include <vector>

namespace dungeon::game {

class ValidateDialog {
public:
	ValidateDialog(gfx::GraphicsDevice& device, ui::FontLibrary& fonts);

	bool IsOpen() const { return m_open; }
	// Show a report. An EMPTY list is still shown, deliberately: "no faults
	// found" is the answer you most want to see, and a check that silently does
	// nothing when it passes teaches you not to trust that it ran.
	void Open(std::vector<validate::Issue> issues);
	void Close() { m_open = false; }

	void Update(const Input& input, float width, float height);
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

	// A row was clicked: show this cell. `x` < 0 means the finding is about the
	// level as a whole, so the owner should browse there without selecting a
	// square. Does NOT close — you are working through a list, and having the
	// report vanish on the first jump would make the second one a re-run.
	std::function<void(const std::string& level, int x, int z)> onJump;

private:
	void BuildUI();

	gfx::GraphicsDevice& m_device;
	ui::UIContext m_ui;
	const gfx::Texture* m_closeIcon = nullptr; // shared, owned by AssetUtil

	bool m_open = false;
	std::vector<validate::Issue> m_issues;
	int m_errors = 0; // counted once at Open, for the title
};

} // namespace dungeon::game
