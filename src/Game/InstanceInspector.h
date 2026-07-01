// ============================================================================
// Game/InstanceInspector.h — base class for the editor's per-INSTANCE edit
// dialogs (a placed monster, torch, item, decoration, ...).
//
// Every placed object shares a set of COMMON properties — starting with facing
// — so the modal chrome (dim + panel + title), the common-property strip (the
// Facing dropdown), the Save/Close footer, Esc-to-revert and the rebuild loop
// all live here ONCE. A concrete dialog derives and supplies only: its title,
// panel size, the facing choices (default N/E/S/W; a wall torch narrows this to
// solid walls), any type-specific controls (BuildContent), and how an edit is
// applied live / persisted / reverted. Facing is stored on the base and pushed
// through ApplyLive() like any other edit.
// ============================================================================
#pragma once

#include "Game/Entity.h" // Direction
#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h"
#include "UI/Widget.h" // complete ui::Widget: m_ui (UIContext) holds it by value

#include <string>
#include <vector>

namespace dungeon::game {

class InstanceInspector {
public:
	explicit InstanceInspector(gfx::GraphicsDevice& device);
	// Out-of-line (defined in the .cpp): m_ui holds unique_ptr<ui::Widget>, which is
	// only forward-declared via UIContext.h here — the .cpp includes the full type.
	virtual ~InstanceInspector();

	bool IsOpen() const { return m_open; }
	void Close() { m_open = false; }

	void Update(const Input& input, float width, float height);
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

protected:
	// --- derived contract ---------------------------------------------------
	virtual std::string Title() const = 0;
	// Panel rect in window fractions (0..1); each dialog sizes itself.
	virtual gfx::Rect Panel() const = 0;
	// The selectable facings for THIS object. Default = all four cardinals; a
	// wall-mounted fixture overrides to the solid walls it may hang on.
	virtual std::vector<Direction> FacingChoices() const;
	// Add type-specific widgets into m_ui within `content` (window fractions).
	// Base already placed the Facing strip above it and the footer below.
	virtual void BuildContent(const gfx::Rect& /*content*/) {}
	// Push the working values (incl. facing) to the live object — called on
	// every edit. Persist writes to disk (Save button); Revert restores the
	// pre-edit state (Close/Esc).
	virtual void ApplyLive() {}
	virtual void Persist() {}
	virtual void Revert() {}

	// --- helpers for derived ------------------------------------------------
	void OpenModal(); // set open + build the UI (call after seeding config)
	void RequestRebuild() { m_rebuild = true; } // rebuild rows next frame
	Direction FacingValue() const { return m_facing; }
	void SetFacingValue(Direction d) { m_facing = d; }
	ui::UIContext& UI() { return m_ui; }

	gfx::GraphicsDevice& m_device;

private:
	void BuildUI();

	ui::Font m_font;    // title text
	ui::UIContext m_ui; // common strip + derived content + footer

	bool m_open = false;
	bool m_rebuild = false;
	Direction m_facing = Direction::South;
};

} // namespace dungeon::game
