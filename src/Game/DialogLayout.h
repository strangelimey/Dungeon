// ============================================================================
// Game/DialogLayout.h — the chrome every editor dialog shares, built once.
//
// Ten dialogs each authored the same card by hand: a panel rect, a title rect
// beside it, a content rect under that, a footer rect at the bottom, and a
// close box floated into the corner — five window fractions per dialog, each an
// independent guess at where the one above it ended. That is the arrangement
// that put a title over its first row, and no amount of care makes twenty
// hand-tuned fractions agree with a font that changes size.
//
// Here the card is a Stack (UI/Layout.h): the dialog gives its panel rect and
// its title, and gets back the body it fills with rows. Nothing downstream
// writes a coordinate, so the bands cannot drift into one another, and a change
// to the shared shape reaches every dialog at once.
//
//   DialogChrome c = BuildDialogChrome(m_ui, kPanel, title, m_closeIcon, close);
//   c.body->Row<ui::Label>(FormRow(), "...");
//   c.footer->Space(ui::Len::Fill()); // centres what follows
//   c.footer->Row<ui::Button>(FooterButton(), "Save", ...);
//   c.footer->Space(ui::Len::Fill());
//
// The panel's own backing is still drawn straight to the batch by the dialog:
// it is the surface everything sits on, not a widget among them.
// ============================================================================
#pragma once

#include "Graphics/SpriteBatch.h"
#include "UI/Controls.h"
#include "UI/Layout.h"
#include "UI/UIContext.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace dungeon::game {

// The dark backing a dialog's 3D preview is blitted over, with an optional
// centred hint for "nothing to show". A WIDGET, not a rect drawn on the side:
// as furniture outside the tree it had no area anything could respect, which is
// how a checkbox label ended up underneath one.
class PreviewPane : public ui::Widget {
public:
	explicit PreviewPane(const gfx::Rect& rect) { bounds = rect; }

	std::string hint;     // drawn centred when set (e.g. "no clip selected")
	bool showHint = true; // flipped per frame by owners whose state changes
						  // without a rebuild — the string stays cached
	bool border = false;

protected:
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;
};

// A title line that is part label, part affordance: a plain prefix, then a NAME
// that is clickable (accent + underline on hover) to open an inline rename. ONE
// widget, because the two halves have to measure and draw in the same face and
// the click target IS the measured half — the level and type dialogs each used
// to draw this in Render and re-derive the hit box in a second function, which
// is how a click target ends up somewhere the text is not.
//
// `onClick` fires from inside the tree walk, so a handler that rebuilds the UI
// must DEFER it a frame (the m_uiRebuild pattern).
class EditableTitle : public ui::Widget {
public:
	EditableTitle(const gfx::Rect& rect, std::string prefix, std::string name,
				  std::function<void()> onClick);

	std::string prefix, name;
	std::function<void()> onClick;

	gfx::Rect InkRect() const override;

protected:
	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	gfx::Rect NamePixels() const;
	bool m_hot = false;
};

// The pieces a dialog fills in. Everything is owned by the context's tree.
struct DialogChrome {
	ui::Stack* page = nullptr;     // the panel's padded interior (the whole card)
	ui::Stack* titleRow = nullptr; // the title slot + the close box's slot
	// The title's own area, left of the close box. Holds the Label built from
	// `title`; a dialog whose title is an affordance (the level dialog's
	// click-to-rename stem) passes an empty title and puts its own widget here,
	// so the band is reserved either way.
	ui::Box* titleSlot = nullptr;
	ui::Label* title = nullptr; // null when the dialog fills the slot itself
	ui::Stack* body = nullptr;     // between title and footer; takes what is left
	ui::Stack* footer = nullptr;   // horizontal; null when the dialog has none
};

// Builds the card inside `panel` (window fractions). An empty `title` still
// reserves the title band and hands back a null `title` — for the dialogs whose
// title is part label, part affordance and draws itself.
DialogChrome BuildDialogChrome(ui::UIContext& ui, const gfx::Rect& panel,
							   const std::string& title,
							   const gfx::Texture* closeIcon,
							   std::function<void()> onClose,
							   bool withFooter = true);

// The extent a control of `lines` text lines wants in a dialog stack. Rem is
// the CONTEXT's document size while these dialogs draw at ui::kDialogTextScale
// times it, so a row's height in rem is its height in lines times the scale it
// is actually drawn at — one place that knows it, rather than the factor
// written out at every row.
ui::Len FormRow(float lines = 1.0f);
// A footer button's width. Fixed rather than filling, so a footer of two
// buttons and one of five look like the same family; `widths` widens a button
// that needs it ("Animation..." beside "Save").
ui::Len FooterButton(float widths = 1.0f);

// The stack a TabControl page's rows go in. CONTENT-SIZED (UI/Layout.h
// fitContent): a tab page is a window onto its rows, not a box they have to fit
// in, so the stack measures itself and the page scrolls when the rows outrun
// it. Rows are added with Row() as anywhere else, which is the point — a
// schema-driven form no longer steps a y cursor by a guessed row pitch, and a
// row that needs two lines just says so.
ui::Stack* TabStack(ui::TabControl& tabs, size_t tab);

// The GitHub-style confirmation for a delete NOTHING brings back — a world
// (W9), a dungeon and its levels (W10). What goes, one line each (scrolled,
// since a dungeon's level list has no fixed length), that it cannot be undone,
// the name to type, Cancel, and a Delete button that wakes ONLY once the typed
// text equals `name` exactly — not trimmed, not case-folded.
//
// ONE BUILDER FOR BOTH, because the rule is the part worth sharing: two copies
// of "enabled when it matches" are two places for one of them to start
// trimming. `typed` is the OWNER's member (it outlives the tree and survives a
// rebuild); `onEdit` fires after each keystroke, for the owner's note;
// `onConfirm` fires from Enter and from the button alike, match or not — the
// owner re-checks, since Enter is not gated by `enabled`. Returns the Delete
// button, which dies with the tree on the next Clear.
struct TypedConfirmText {
	std::string undo;    // "This cannot be undone."
	std::string prompt;  // "Type crypt to confirm:"
	std::string cancel;  // an ANSWER, not a dismiss (the Yes/No exemption)
	std::string confirm; // "Delete this world"
};
ui::Button* BuildTypedConfirm(ui::Stack& body, std::span<const std::string> what,
							  const TypedConfirmText& text, const std::string& name,
							  std::string& typed, std::function<void()> onEdit,
							  std::function<void()> onCancel,
							  std::function<void()> onConfirm);

// The "?" explainer a dialog puts over itself: a second dim wash (so the dialog
// beneath visibly freezes), a panel, a title, and word-wrapped paragraphs. The
// owner holds the open/closed flag and dismisses on any click or Esc — the
// drawing is all that is shared, and it is shared because two dialogs hand-
// rolling the same word-wrap is two places for it to be subtly different.
//
// TEXT ARRIVES AS VIEWS, not strings: callers pass loc::View(key), which
// borrows from the string table, so an overlay that is up for a hundred frames
// allocates on none of them.
void DrawHelpOverlay(gfx::SpriteBatch& batch, const ui::Theme& theme,
					 const ui::Font& font, float width, float height,
					 std::string_view title,
					 std::span<const std::string_view> paragraphs);

} // namespace dungeon::game
