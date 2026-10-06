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

#include "Assets/Model.h" // SkeletonData, AnimationClipData (animated preview)
#include "Game/DialogLayout.h" // FormRow - what BuildContent sizes its rows by
#include "Game/Entity.h"  // Direction
#include "Graphics/GraphicsDevice.h"
#include "Graphics/ModelPreview.h" // gfx::PreviewSubmesh
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/Layout.h" // ui::Stack — the derived contract takes one
#include "UI/UIContext.h"
#include "UI/Widget.h" // complete ui::Widget: m_ui (UIContext) holds it by value

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dungeon::ui {
class TabControl;
}

namespace dungeon::game {

// What a dialog wants shown in its 3D preview pane. Populated by the owner (Game
// resolves the meshes) when the dialog opens; the dialog holds it and may mutate
// live bits (e.g. a torch flips showFire with its Lit toggle). Uses only gfx /
// assets types so the base stays independent of the world/catalog layer.
struct PreviewSpec {
	std::vector<gfx::PreviewSubmesh> subs; // the mesh(es); empty = no preview pane
	float scale = 1.0f;
	float yaw = 0.0f; // model facing fixup, so a front-on view matches in-world
	// The model-space point the grounded view stands and turns on (a monster's
	// rig root - gfx::ModelPreview::Render's `pivot`); zero = the origin.
	Vec3 pivot{};
	// Skinned animation: the owner builds an Animator over these and plays idleClip.
	const assets::SkeletonData* skeleton = nullptr;
	const std::vector<assets::AnimationClipData>* clips = nullptr;
	std::string idleClip;
	// Fire/smoke overlay: a flame at flameHeight above the base, shown when showFire.
	bool fire = false;
	float flameHeight = 0.0f;
	float flameScale = 0.55f; // sconce flame; a brazier's is bigger
	bool showFire = false;
	// Auto-fit framing (for small/loose props like weapons): scale the model to fill
	// the pane and centre it on the model-space AABB [fitMin,fitMax] instead of the
	// grounded head-on default; spin makes it revolve on a turntable.
	bool autoFit = false;
	bool spin = false;
	Vec3 fitMin{}, fitMax{};
};

// The FLAGS an inspector may pick from (flags.cat): (id, label), the label
// naming the flag and its scope so a dungeon's flag reads as that dungeon's.
// The door, lever and stair inspectors all take one.
using FlagChoices = std::vector<std::pair<std::string, std::string>>;

// A "(none)" + flags dropdown bound to `value` (its id, "" = none). A value the
// list lacks (a flag since deleted) is kept selectable, so opening and saving
// cannot quietly drop it. `onChange` runs after `value` is updated; `value`
// must outlive the widget (an inspector's Config member does).
void FlagDropDown(ui::Stack& page, ui::Len len, const FlagChoices& flags,
				  std::string& value, std::function<void()> onChange);

class InstanceInspector {
public:
	InstanceInspector(gfx::GraphicsDevice& device, ui::FontLibrary& fonts);
	// Out-of-line (defined in the .cpp): m_ui holds unique_ptr<ui::Widget>, which is
	// only forward-declared via UIContext.h here — the .cpp includes the full type.
	virtual ~InstanceInspector();

	bool IsOpen() const { return m_open; }
	void Close() { m_open = false; }
	// What Esc and the close box do: put the live object back as it was, then
	// close. Public so the harness can press it (`editor inspector esc`).
	void Cancel() {
		Revert();
		Close();
	}
	// The footer's Save: persist, then close.
	void ClickSave() {
		Persist();
		Close();
	}
	// The footer's Delete (see onDelete below), which a harness presses through
	// `editor inspector delete`. False when this open offers none.
	bool ClickDelete() {
		if (!onDelete) return false;
		onDelete(); // gone - no Revert
		Close();
		return true;
	}
	// A rebuild a click queued is done by the next Update; a harness, which
	// runs no Update while its console is up, does it now (the type editor's
	// ApplyPending rule).
	void ApplyPending();
	// The content's tab strip, when it has one (-1 = none), and a pick of a tab
	// as its strip's click makes it.
	int ActiveTab() const;
	void SelectTab(int tab);
	// A press on the n-th drop-down the dialog shows (`editor inspector popup
	// <n>`; opened at the next Update), and whether one is open - what Esc
	// asks before it cancels the dialog (code-review C81).
	void OpenPopup(int n) { m_ui.OpenPopupNext(n); }
	bool PopupOpen() const { return m_ui.PopupOpen(); }

	// Optional Delete action: when the OWNER sets this before Open, the footer
	// shows a Delete button beside Save (in the old Close slot — closing moved
	// to the panel's top-right "x" and Esc). Clicking it fires the callback
	// (which removes the object from the map) and closes WITHOUT Revert — the
	// object is gone, there is nothing to restore. Set per open; inspectors
	// that never set it just show Save.
	std::function<void()> onDelete;

	// Optional toggle drawn BESIDE the Facing dropdown (the decoration dialog's
	// per-TYPE "map facing arrow" flag). Set — or reset — before Open, like
	// onDelete. The change applies through the callback IMMEDIATELY (it edits
	// the type, not this instance, so it is deliberately outside Save/Revert).
	struct FacingExtra {
		std::string label;
		bool value = true;
		std::function<void(bool)> onChange;
	};
	std::optional<FacingExtra> facingExtra;

	void Update(const Input& input, float width, float height);
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

	// A right-side 3D preview pane (a live mesh/animation/particle thumbnail, like
	// the monster-config dialog). Present when the dialog was given a non-empty
	// PreviewSpec; the controls then reflow into a left column and this rect (window
	// pixels) is the pane the owner renders + blits into.
	bool HasPreview() const { return !m_preview.subs.empty(); }
	gfx::Rect PreviewRect(float width, float height) const;
	// The live preview description (owner reads it each frame to render the pane).
	const PreviewSpec& Preview() const { return m_preview; }

protected:
	// --- derived contract ---------------------------------------------------
	virtual std::string Title() const = 0;
	// Panel rect in window fractions (0..1); each dialog sizes itself.
	virtual gfx::Rect Panel() const = 0;
	// The selectable facings for THIS object. Default = all four cardinals; a
	// wall-mounted fixture overrides to the solid walls it may hang on.
	virtual std::vector<Direction> FacingChoices() const;
	// Add type-specific rows to `content` — a ui::Stack, so a dialog says how
	// TALL each row is and never where it sits (UI/Layout.h). Rows are added in
	// order, sized in rem, and cannot overlap each other or the chrome the base
	// stacked above and below them. Use game::FormRow (Game/DialogLayout.h, the
	// one every editor dialog sizes its rows by: 1 line = a label, a dropdown, a
	// checkbox; ~1.9 = a Slider, which stacks its label over its track) for a
	// control, ui::Len::Fill() for something that should take the rest (a tab
	// control).
	virtual void BuildContent(ui::Stack& /*content*/) {}

	// Push the working values (incl. facing) to the live object — called on
	// every edit. Persist writes to disk (Save button); Revert restores the
	// pre-edit state (Close/Esc).
	virtual void ApplyLive() {}
	virtual void Persist() {}
	virtual void Revert() {}

	// --- helpers for derived ------------------------------------------------
	void OpenModal(); // set open + build the UI (call after seeding config)
	void RequestRebuild() { m_rebuild = true; } // rebuild rows next frame
	// Registers the content's tab strip (BuildContent hands back the one it
	// made). A REBUILD keeps the tab the user is on - a dependent field or a
	// cleared route used to throw the dialog back to its first tab, so the
	// change was never seen (code-review C104); a fresh Open starts on the
	// first.
	ui::TabControl* ContentTabs(ui::TabControl* tabs) {
		m_tabs = tabs;
		return tabs;
	}
	Direction FacingValue() const { return m_facing; }
	void SetFacingValue(Direction d) { m_facing = d; }
	ui::UIContext& UI() { return m_ui; }
	// Preview access for derived types: set the spec at Open, flip live bits later.
	void SetPreview(PreviewSpec spec) { m_preview = std::move(spec); }
	void SetShowFire(bool on) { m_preview.showFire = on; }

	gfx::GraphicsDevice& m_device;

private:
	void BuildUI();

	ui::UIContext m_ui; // title + common strip + derived content + footer
	const gfx::Texture* m_closeIcon = nullptr; // shared, owned by AssetUtil
	// The preview pane widget, owned by the tree; null when there is no preview.
	// PreviewRect hands its rect out, so the backing and the 3D blit are the
	// same area by construction.
	ui::Widget* m_pane = nullptr;
	// The content's tab strip (ContentTabs), owned by the tree; null when it
	// has none.
	ui::TabControl* m_tabs = nullptr;

	bool m_open = false;
	bool m_rebuild = false;
	Direction m_facing = Direction::South;
	PreviewSpec m_preview; // what the preview pane shows (empty = no pane)
};

} // namespace dungeon::game
