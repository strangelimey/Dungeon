// ============================================================================
// Game/ItemDetailsDialog.h - the play-mode item details dialog.
//
// A right-click on an item - in the backpack, on the paper doll, in a hand box,
// in the party inventory, or lying on the floor - opens this: the item turning
// slowly in 3D on the left, and on the right only the lines that item has
// (category, weight, weapon numbers, protection, what eating it restores, a
// written description). docs/ui-updates-plan.md P3.
//
// Built ONCE, not per open. Opening happens on a click in a settled frame, and
// the sheet's frames are guarded (Game::SteadyStateFrame), so building a widget
// tree there would be an allocation per right-click. Every row exists from
// construction; opening assigns into their reserved strings and hides the rows
// the item does not have (a Stack skips an invisible row, so the rest close up).
// The world keeps running underneath, as it does under the sheet.
//
// The 3D image is Game's: it renders the preview into its ModelPreview target
// each frame the dialog is open and blits it over PreviewRect(). The dialog only
// holds WHAT to render (the submeshes and the box to frame them by).
// ============================================================================
#pragma once

#include "Game/ItemDetails.h"
#include "Graphics/ModelPreview.h" // gfx::PreviewSubmesh
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/UIContext.h"
#include "UI/Widget.h"

#include <array>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace dungeon::ui {
class Button;
class Label;
class FontLibrary;
} // namespace dungeon::ui

namespace dungeon::game {

class DescriptionText; // the wrapped description widget (ItemDetailsDialog.cpp)

class ItemDetailsDialog {
public:
	ItemDetailsDialog(gfx::GraphicsDevice& device, ui::FontLibrary& fonts);
	~ItemDetailsDialog();

	// (Re)builds the rows in the current language. The constructor calls it; a
	// language switch calls it again (outside any widget callback - it clears the
	// tree).
	void Build();

	// Fills the rows from `details` and opens. `weightKg` is what the thing
	// weighs where it sits (a bag with its contents), so it is the caller's.
	void Open(const ItemDetails& details, float weightKg);
	void Close() { m_open = false; }

	// The footer's MEMORIZE button (Michael, spell-updates): hidden by every Open,
	// shown by the caller when the item is a rune its holder does not know yet.
	// Built with the dialog, so showing it allocates nothing. Its press runs
	// `onMemorize`; the owner re-checks the slot before it spends the tablet.
	void ShowMemorize(bool shown);
	bool MemorizeShown() const;
	std::function<void()> onMemorize;
	bool IsOpen() const { return m_open; }
	// How many times it has opened this run - `itemdetails status` prints it, so
	// tools\AllocTest.ps1 -Sheet can refuse a PASS when no open was measured.
	unsigned OpenCount() const { return m_opens; }

	// Esc closes; otherwise the dialog takes the pointer (it is modal for the
	// mouse - the keyboard is left to whoever owns it underneath).
	void Update(const Input& input, float width, float height, float dt);
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width, float height);

	// --- the 3D image (Game renders it) -----------------------------------------
	// The caller fills this at Open time; up to kMaxSubs submeshes.
	static constexpr size_t kMaxSubs = 16;
	std::span<gfx::PreviewSubmesh> PreviewBuffer() { return m_subs; }
	void SetPreview(size_t count, const Vec3& fitMin, const Vec3& fitMax, const Mat4& pose);
	std::span<const gfx::PreviewSubmesh> PreviewSubs() const {
		return {m_subs.data(), m_subCount};
	}
	const Vec3& FitMin() const { return m_fitMin; }
	const Vec3& FitMax() const { return m_fitMax; }
	// How the model is stood up before it turns about the vertical.
	const Mat4& Pose() const { return m_pose; }
	// The turntable angle: a slow full turn every kSpinSeconds.
	float Spin() const { return m_spin; }
	// A burning item's flame head (model space), else null; Game projects it
	// through the preview's framing and draws the flame over the image.
	const Vec3* FlameHead() const { return m_burning ? &m_flameHead : nullptr; }
	// The model's size in the pane: a burning one is drawn smaller, leaving
	// room above its head for the flame.
	float PreviewScale() const { return m_burning ? 0.78f : 1.0f; }
	// The pane's pixel rect from the layout that last ran (Render lays out).
	gfx::Rect PreviewRect() const;

	ui::UIContext& UI() { return m_ui; }

private:
	// One "label  value" line. The label is localized once at Build; the value
	// is assigned on Open.
	struct Row {
		ui::Widget* row = nullptr; // the horizontal stack (visibility)
		ui::Label* value = nullptr;
	};
	enum RowId {
		kCategory, kWeight, kDamage, kSpeed, kSkill, kReach, kElement,
		kArmor, kArmorClass, kWorn, kResists, kNutrition, kHydration, kRowCount
	};
	void SetRow(RowId id, std::string_view text);

	gfx::GraphicsDevice& m_device;
	ui::UIContext m_ui;
	const gfx::Texture* m_closeIcon = nullptr; // shared, owned by AssetUtil
	ui::Label* m_title = nullptr;
	ui::Widget* m_pane = nullptr;
	std::array<Row, kRowCount> m_rows{};
	DescriptionText* m_desc = nullptr;
	ui::Button* m_memorize = nullptr;

	bool m_open = false;
	unsigned m_opens = 0;
	float m_spin = 0.0f;
	float m_breath = 0.0f; // a rune tablet's groove glow, radians
	std::array<gfx::PreviewSubmesh, kMaxSubs> m_subs{};
	size_t m_subCount = 0;
	Vec3 m_fitMin{}, m_fitMax{};
	bool m_burning = false;
	Vec3 m_flameHead{};
	Mat4 m_pose{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

} // namespace dungeon::game
