// ============================================================================
// Game/InstanceInspector.cpp — see InstanceInspector.h.
// ============================================================================
#include "Game/InstanceInspector.h"

#include "Core/Loc.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "UI/Controls.h"

#include <algorithm>

namespace dungeon::game {

namespace {
// The panel's inner margin, the one rect this file still authors as a fraction
// (of the derived Panel()). EVERYTHING inside it is stacked: the bands used to
// be fractions too — kTitleH 0.12, kFacingH 0.14 — which is a guess at how tall
// a title is, and the guess was wrong the moment the title face grew. A 52px
// title in a 45px band is exactly the overlap the user saw. See UI/Layout.h.
constexpr float kPad = 0.04f;
// Row extents, in rem (Units.h). See InstanceInspector::FormRow for why the
// dialog scale is folded in.
constexpr float kTitleRow = ui::kDialogTitleScale * 1.35f;
constexpr float kFooterRow = ui::kDialogTextScale * 1.9f;
constexpr float kSeparatorRow = 0.6f;
// The close box lives at the panel's top-right in absolute panel fractions
// (ui::AddCloseButton), so the title row RESERVES its width rather than
// discovering it — the two are not siblings, and nothing else would stop a long
// title running underneath the "x".
constexpr float kCloseRow = ui::kDialogTitleScale * 0.85f;
// With a preview pane the controls take this share of the body and the pane
// takes the rest, with a gutter between.
constexpr float kColumnFill = 0.60f;
constexpr float kPaneFill = 0.40f;
constexpr float kGutterRow = 1.0f;

// The preview pane: the dark backing the owner blits the live 3D image over.
// A WIDGET, not a rect drawn on the side — as furniture outside the tree it had
// no area anything could respect, which is how a checkbox label ended up
// underneath it.
class PreviewPane : public ui::Widget {
public:
	explicit PreviewPane(const gfx::Rect& rect) { bounds = rect; }

protected:
	void DrawSelf(ui::UIContext&, gfx::SpriteBatch& batch) override {
		batch.DrawRect(Pixel(), {0.02f, 0.02f, 0.03f, 1.0f});
	}
};
} // namespace

ui::Len InstanceInspector::FormRow(float lines) {
	return ui::Len::Fixed(ui::kDialogTextScale * 1.6f * lines);
}

InstanceInspector::InstanceInspector(gfx::GraphicsDevice& device,
									 ui::FontLibrary& fonts)
	: m_device(device), m_ui(fonts, ui::FontRole::Body, 18.0f) {
	// Covers all six per-instance inspectors: they build into THIS context.
	m_ui.Root().fontScale = ui::kDialogTextScale; // inherits — see LevelSettings
	m_closeIcon = CloseIcon(device);
}

InstanceInspector::~InstanceInspector() = default;

std::vector<Direction> InstanceInspector::FacingChoices() const {
	return {Direction::North, Direction::East, Direction::South, Direction::West};
}

void InstanceInspector::OpenModal() {
	m_open = true;
	m_rebuild = false;
	BuildUI();
}

void InstanceInspector::BuildUI() {
	m_ui.Clear();
	m_pane = nullptr;
	const gfx::Rect p = Panel();

	// ONE authored rect — the panel's padded interior — and every band inside it
	// computed. Nothing below writes a coordinate.
	ui::Stack* page = m_ui.Add<ui::Stack>(
		gfx::Rect{p.x + kPad * p.w, p.y + kPad * p.h, (1.0f - 2 * kPad) * p.w,
				  (1.0f - 2 * kPad) * p.h});
	page->debugName = "page";
	page->gapRem = 0.4f;

	// Title row: the title beside the space the close box occupies.
	ui::Stack* titleRow = page->Row<ui::Stack>(ui::Len::Fixed(kTitleRow), true);
	titleRow->debugName = "title";
	ui::Label* title = titleRow->Row<ui::Label>(ui::Len::Fill(), Title());
	title->fontScale = ui::kDialogTitleScale;
	title->centerV = true;
	// Close (revert, like Esc): the shared box icon, in a slot the title row
	// RESERVED for it rather than floated over the panel corner. As a floating
	// widget it sat on top of the page stack — which the overlap audit duly
	// reported, and which a long enough title would have made visible.
	ui::Box* closeSlot = titleRow->Space(ui::Len::Fixed(kCloseRow));
	closeSlot->debugName = "close";
	ui::AddCloseButton(*closeSlot, m_closeIcon, [this] {
		Revert();
		Close();
	});

	// Body: the control column, and beside it the preview pane when there is one.
	ui::Stack* body = page->Row<ui::Stack>(ui::Len::Fill(), true);
	body->debugName = "body";
	ui::Stack* column =
		body->Row<ui::Stack>(ui::Len::Fill(HasPreview() ? kColumnFill : 1.0f));
	column->debugName = "column";
	column->gapRem = 0.4f;
	if (HasPreview()) {
		body->Space(ui::Len::Fixed(kGutterRow));
		// The pane and its header are themselves a little stack, so the header
		// cannot sit on the image and the image cannot sit on the column.
		ui::Stack* pane = body->Row<ui::Stack>(ui::Len::Fill(kPaneFill));
		pane->debugName = "preview";
		ui::Label* header =
			pane->Row<ui::Label>(FormRow(0.8f), loc::Tr("map.cfg.preview"));
		header->dim = true;
		m_pane = pane->Row<PreviewPane>(ui::Len::Fill());
	}

	// Common property strip: Facing (label + dropdown) — omitted entirely for a
	// fixture with no facing choices (e.g. a floor brazier).
	const std::vector<Direction> choices = FacingChoices();
	if (!choices.empty()) {
		ui::Stack* row = column->Row<ui::Stack>(FormRow(), true);
		row->debugName = "facing";
		row->gapRem = 0.5f;
		ui::Label* label =
			row->Row<ui::Label>(ui::Len::Fill(0.9f), loc::Tr("map.insp.facing"));
		label->centerV = true;
		std::vector<std::string> items;
		int sel = 0;
		for (size_t i = 0; i < choices.size(); ++i) {
			items.push_back(loc::Tr(FacingLocKey(choices[i])));
			if (choices[i] == m_facing) sel = static_cast<int>(i);
		}
		row->Row<ui::DropDown>(ui::Len::Fill(1.4f), items, sel,
							   [this, choices](int i) {
								   if (i >= 0 && i < static_cast<int>(choices.size())) {
									   m_facing = choices[static_cast<size_t>(i)];
									   ApplyLive();
								   }
							   });
	}
	// The extra toggle (see facingExtra) takes its OWN row. It used to share the
	// facing row, which meant a checkbox squeezed into a third of a column that
	// is itself 60% of the panel when a preview is up — the label ran straight
	// out of it and under the preview pane. A row of its own cannot.
	if (facingExtra)
		column->Row<ui::Checkbox>(FormRow(), facingExtra->label, facingExtra->value,
								  [this](bool on) {
									  facingExtra->value = on;
									  if (facingExtra->onChange)
										  facingExtra->onChange(on);
								  });

	// Divider, then the derived content, then the footer. The content is a Fill
	// row, so it takes whatever the fixed chrome leaves.
	column->Row<ui::Separator>(ui::Len::Fixed(kSeparatorRow));
	ui::Stack* content = column->Row<ui::Stack>(ui::Len::Fill());
	content->debugName = "content";
	content->gapRem = 0.5f;
	BuildContent(*content);

	// Footer: Save (persist), plus Delete (remove the object from the map)
	// when the owner armed it — closing lives in the "x"/Esc, not down here.
	ui::Stack* footer = column->Row<ui::Stack>(ui::Len::Fixed(kFooterRow), true);
	footer->debugName = "footer";
	footer->gapRem = 0.8f;
	footer->Row<ui::Button>(ui::Len::Fill(), loc::Tr("map.cfg.save"), [this] {
		Persist();
		Close();
	});
	if (onDelete)
		footer->Row<ui::Button>(ui::Len::Fill(), loc::Tr("map.cfg.delete"), [this] {
			onDelete(); // the object is gone — no Revert
			Close();
		});
	else
		footer->Space(ui::Len::Fill()); // keep Save at half width, as with Delete
}

gfx::Rect InstanceInspector::PreviewRect(float, float) const {
	// The pane's own rect, from the layout that just ran — one truth for the
	// backing the widget draws and the 3D image the owner blits over it. (The
	// dialog's Render lays the tree out before Game asks, so this is current.)
	return m_pane ? m_pane->Pixel() : gfx::Rect{0.0f, 0.0f, 0.0f, 0.0f};
}

void InstanceInspector::Update(const Input& input, float w, float h) {
	if (!m_open) return;
	m_ui.UseFont(ui::FontRole::Body, std::clamp(h * 0.020f, 12.0f, 24.0f));

	if (input.WasKeyPressed(VK_ESCAPE)) {
		Revert();
		Close();
		return;
	}
	m_ui.Update(input, w, h);
	if (!m_open) return; // a footer button closed us this frame
	if (m_rebuild) {     // a dependent-field change queued a rebuild
		m_rebuild = false;
		BuildUI();
	}
}

void InstanceInspector::Render(gfx::SpriteBatch& batch, const ui::Theme& th, float w,
							   float h) {
	if (!m_open) return;
	auto px = [&](const gfx::Rect& r) {
		return gfx::Rect{r.x * w, r.y * h, r.w * w, r.h * h};
	};
	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.6f});
	const gfx::Rect p = Panel();
	const gfx::Rect panel = px(p);
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);
	// Everything else — title, facing strip, content, footer, and the preview
	// pane's backing — is a widget in the tree, so it comes out of one layout
	// that gave each of them an area of its own.
	m_ui.Render(batch, w, h);
}

} // namespace dungeon::game
