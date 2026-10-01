// ============================================================================
// Game/NewWorldDialog.h - making a new world (docs/editor-updates-plan.md, P4).
//
// Michael: "add a button. when clicked it brings up a dialog where the user
// can start blank, copy the current world/level/*.cat files, or set them up
// with a 'new world' wizard." Opened from a disc on BOTH editor toolbars (the
// level editor's and the world screen's) and from the Worlds dialog's "New
// world..." button - one dialog, so there is one way to make a world however
// you got here.
//
// A name, a choice of how it starts (Game/NewWorld.h: blank from the
// template, this world whole, or one of its levels), and Create. A made world
// is not opened by making it: the dialog then offers SWITCH NOW, since
// switching ends the game in hand, and that is a second decision.
//
// THE DIALOG PROPOSES, THE OWNER DISPOSES (the WorldsDialog split): making and
// switching leave through callbacks, and the refusals come back as sentences.
// ============================================================================
#pragma once

#include "Game/NewWorld.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dungeon::ui {
class Label;
}

namespace dungeon::game {

class NewWorldDialog {
public:
	NewWorldDialog(gfx::GraphicsDevice& device, ui::FontLibrary& fonts);

	bool IsOpen() const { return m_open; }
	void Open();
	void Close() { m_open = false; }

	void Update(const Input& input, float width, float height);
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

	// Makes the world; returns {name as written, ""} or {"", the reason}.
	std::function<std::pair<std::string, std::string>(const std::string& name,
													  const NewWorldSpec& spec)>
		onCreate;
	// Opens `name` (a new game there). False when it could not.
	std::function<bool(const std::string& name)> onSwitch;
	// The running world's levels, for "Copy one level". Asked on Open.
	std::function<std::vector<std::string>()> onLevels;
	// The wizard's tag choices (the template's content tags). Asked on Open.
	std::function<std::vector<std::string>()> onTags;
	// The shared library's styles as (id, display), for the Style row a blank
	// or wizard world starts in (Phase 7). Asked on Open.
	std::function<std::vector<std::pair<std::string, std::string>>()> onStyles;
	// The Style row's pick, as the harness makes it ("" = none).
	void SetStyle(const std::string& id);

	// The wizard's knobs (P5), for the harness as for its rows. Size is the
	// map's side in squares; the dialog offers three.
	void SetWizard(const std::string& tag, int size, float difficulty, u32 seed);
	const NewWorldSpec& Spec() const { return m_spec; }

	// For the harness: the same clicks, and what the dialog would say.
	void SetSource(NewWorldSpec::Source source, const std::string& level = {});
	void Create(const std::string& name);
	void SwitchNow();
	const std::string& Note() const { return m_note; }
	const std::string& Made() const { return m_made; }
	NewWorldSpec::Source Source() const { return m_spec.source; }
	// Applies a rebuild the calls above deferred (the WorldsDialog reason: a
	// console command is not inside the tree walk, and `uioverlap` after one
	// must audit the view AFTER it).
	void ApplyPending();

private:
	void BuildUI();

	gfx::GraphicsDevice& m_device;
	ui::UIContext m_ui;
	const gfx::Texture* m_closeIcon = nullptr; // shared, owned by AssetUtil

	bool m_open = false;
	bool m_uiRebuild = false; // deferred: buttons fire from inside the tree walk
	std::string m_name;       // the name field's text, across rebuilds
	NewWorldSpec m_spec;
	std::vector<std::string> m_levels; // this world's, for Copy one level
	std::vector<std::string> m_tagChoices; // the template's tags, for the wizard
	std::vector<std::pair<std::string, std::string>> m_styleChoices; // the library's
	gfx::Rect m_panel{};               // taller while the wizard's rows show
	std::string m_made;                // the world just made ("" = none yet)
	std::string m_note;
	ui::Label* m_noteLabel = nullptr; // borrowed from the live tree
};

} // namespace dungeon::game
