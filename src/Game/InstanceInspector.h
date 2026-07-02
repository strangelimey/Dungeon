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
#include "Game/Entity.h"  // Direction
#include "Graphics/GraphicsDevice.h"
#include "Graphics/ModelPreview.h" // gfx::PreviewSubmesh
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h"
#include "UI/Widget.h" // complete ui::Widget: m_ui (UIContext) holds it by value

#include <string>
#include <utility>
#include <vector>

namespace dungeon::game {

// What a dialog wants shown in its 3D preview pane. Populated by the owner (Game
// resolves the meshes) when the dialog opens; the dialog holds it and may mutate
// live bits (e.g. a torch flips showFire with its Lit toggle). Uses only gfx /
// assets types so the base stays independent of the world/catalog layer.
struct PreviewSpec {
	std::vector<gfx::PreviewSubmesh> subs; // the mesh(es); empty = no preview pane
	float scale = 1.0f;
	float yaw = 0.0f; // model facing fixup, so a front-on view matches in-world
	// Skinned animation: the owner builds an Animator over these and plays idleClip.
	const assets::SkeletonData* skeleton = nullptr;
	const std::vector<assets::AnimationClipData>* clips = nullptr;
	std::string idleClip;
	// Fire/smoke overlay: a flame at flameHeight above the base, shown when showFire.
	bool fire = false;
	float flameHeight = 0.0f;
	bool showFire = false;
	// Auto-fit framing (for small/loose props like weapons): scale the model to fill
	// the pane and centre it on the model-space AABB [fitMin,fitMax] instead of the
	// grounded head-on default; spin makes it revolve on a turntable.
	bool autoFit = false;
	bool spin = false;
	Vec3 fitMin{}, fitMax{};
};

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
	// Preview access for derived types: set the spec at Open, flip live bits later.
	void SetPreview(PreviewSpec spec) { m_preview = std::move(spec); }
	void SetShowFire(bool on) { m_preview.showFire = on; }

	gfx::GraphicsDevice& m_device;

private:
	void BuildUI();

	ui::Font m_font;    // title text
	ui::UIContext m_ui; // common strip + derived content + footer

	bool m_open = false;
	bool m_rebuild = false;
	Direction m_facing = Direction::South;
	PreviewSpec m_preview; // what the preview pane shows (empty = no pane)
};

} // namespace dungeon::game
