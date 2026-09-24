// ============================================================================
// Game/WorldsDialog.h — the worlds beside this one, and the way between them
// (W8, docs/world-editor-plan.md). The `worlds` console command wearing a face.
//
// A WORLD IS A PROJECT FOLDER (W7): its own overworld, dungeons, levels and
// content. This lists them, opens one, and makes a new one — nothing else. It
// is opened from the world screen's toolbar, the leftmost disc, because it is
// the one tool there that is about something other than THIS world.
//
// OPENING ONE RELAUNCHES THE GAME (Game::SwitchWorld — everything downstream
// is built from the choice at startup), so the click that does it is ARMED:
// the first click on a row's Open says what is about to happen and the second
// does it. The TypeEditorDialog's Delete makes the same bargain for the same
// reason — the only two buttons in the editor whose effect undo cannot reach.
//
// THE DIALOG PROPOSES, THE OWNER DISPOSES (the WorldSettingsDialog split):
// creating and switching leave through callbacks, and the list is re-read from
// the owner after a create, so the rows are always what is on disk rather than
// what this dialog believes it made.
// ============================================================================
#pragma once

#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h"

#include <functional>
#include <string>
#include <vector>

namespace dungeon::ui {
class Label;
}

namespace dungeon::game {

class WorldsDialog {
public:
	WorldsDialog(gfx::GraphicsDevice& device, ui::FontLibrary& fonts);

	bool IsOpen() const { return m_open; }
	// `openName` is the world RUNNING now — not the one settings.ini names,
	// which a `-project` run leaves alone by design.
	void Open(std::string openName);
	void Close() { m_open = false; }

	void Update(const Input& input, float width, float height);
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

	// The worlds on disk, by folder name. Asked on Open and after a create.
	std::function<std::vector<std::string>()> onList;
	// Makes a new world; returns its name as written (the name is FILTERED to
	// a folder-safe id), or "" when refused — the log says why.
	std::function<std::string(const std::string& name)> onCreate;
	// Relaunches into `name`. False when it could not (no such world).
	std::function<bool(const std::string& name)> onSwitch;

	// For the harness: what the dialog would say, and which row is armed.
	const std::vector<std::string>& Worlds() const { return m_worlds; }
	const std::string& Armed() const { return m_armed; }
	const std::string& Note() const { return m_note; }
	// The same two clicks the rows take, so a script can drive them.
	void ClickOpen(const std::string& name);
	void Create(const std::string& name);

private:
	void BuildUI();
	void SetNote(std::string text);

	gfx::GraphicsDevice& m_device;
	ui::UIContext m_ui;
	const gfx::Texture* m_closeIcon = nullptr; // shared, owned by AssetUtil

	bool m_open = false;
	// Deferred rebuild — a row's button is inside the tree walk when it fires
	// (the m_pendingLanguage convention).
	bool m_uiRebuild = false;

	std::string m_openName;           // the world running now
	std::vector<std::string> m_worlds;
	std::string m_armed;              // the row whose Open was clicked once
	std::string m_newName;            // the create field's text, across rebuilds
	std::string m_note;
	ui::Label* m_noteLabel = nullptr; // borrowed from the live tree
};

} // namespace dungeon::game
