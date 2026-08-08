// ============================================================================
// Game/InstanceInspector.cpp — see InstanceInspector.h.
// ============================================================================
#include "Game/InstanceInspector.h"

#include "Core/Loc.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/DialogLayout.h"
#include "UI/Controls.h"

#include <algorithm>

namespace dungeon::game {

namespace {
// The bands used to be fractions of the panel — kTitleH 0.12, kFacingH 0.14 —
// which is a guess at how tall a title is, and the guess was wrong the moment
// the title face grew. A 52px title in a 45px band is exactly the overlap the
// user saw. The card is Game/DialogLayout's now; only what goes INSIDE it is
// here. See UI/Layout.h.
constexpr float kSeparatorRow = 0.6f;
// With a preview pane the controls take this share of the body and the pane
// takes the rest, with a gutter between.
constexpr float kColumnFill = 0.60f;
constexpr float kPaneFill = 0.40f;
constexpr float kGutterRow = 1.0f;
} // namespace

ui::Len InstanceInspector::FormRow(float lines) { return game::FormRow(lines); }

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

	// The shared card (Game/DialogLayout.h): panel padding, title row with the
	// close box in a slot of its own, body, footer. Nothing here writes a
	// coordinate; the rest of this function only says how tall each row is.
	DialogChrome chrome = BuildDialogChrome(m_ui, Panel(), Title(), m_closeIcon,
											[this] {
												Revert();
												Close();
											});

	// Body: the control column, and beside it the preview pane when there is one.
	ui::Stack* body = chrome.body;
	body->horizontal = true;
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

	// Divider, then the derived content. The content is a Fill row, so it takes
	// whatever the fixed chrome leaves.
	column->Row<ui::Separator>(ui::Len::Fixed(kSeparatorRow));
	ui::Stack* content = column->Row<ui::Stack>(ui::Len::Fill());
	content->debugName = "content";
	content->gapRem = 0.5f;
	BuildContent(*content);

	// Footer: Save (persist), plus Delete (remove the object from the map)
	// when the owner armed it — closing lives in the "x"/Esc, not down here.
	// Left-aligned under the control column, so the preview pane keeps its side.
	chrome.footer->Row<ui::Button>(FooterButton(), loc::Tr("map.cfg.save"), [this] {
		Persist();
		Close();
	});
	if (onDelete)
		chrome.footer->Row<ui::Button>(FooterButton(), loc::Tr("map.cfg.delete"),
									   [this] {
										   onDelete(); // gone — no Revert
										   Close();
									   });
	chrome.footer->Space(ui::Len::Fill());
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
