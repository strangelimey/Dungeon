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
// Sub-region layout WITHIN the derived Panel(), as fractions of that panel. The
// common strip (title + facing) is fixed height at the top; the footer (Save/
// Close) is fixed at the bottom; the derived content fills the middle.
constexpr float kPad = 0.04f;      // panel inner margin (fraction of panel w/h)
// Title band. It has to clear the TITLE FACE's line advance, which is
// kDialogTitleScale (2.9x) the document size — at 0.12, the value from when
// titles drew at 1x, the title spilled down into the facing row.
//
// Derived, not guessed. The context font is clamp(h * 0.020, 12, 24), the title
// advance is that x 2.9 x 1.25, and the shallowest panel is 0.42 h
// (PropInspector). While the clamp is off both scale with h, so the ratio is
// constant: 0.020 * 2.9 * 1.25 / 0.42 = 0.173 of the panel, plus kPad above it.
// The clamp only ever makes it smaller (at 1440 it falls to 0.144), so 0.22 is
// the worst case with a little headroom — and FitDialogTitle shrinks rather
// than overlaps should a future panel be shallower still.
constexpr float kTitleH = 0.22f;
constexpr float kFacingH = 0.14f;  // facing row
constexpr float kFooterH = 0.14f;  // Save/Close band
// When a preview pane is shown, the controls occupy the left column up to this
// panel-width fraction; the pane fills the rest.
constexpr float kCtrlCol = 0.60f;
constexpr float kPaneX0 = 0.62f; // preview pane left edge (panel fraction)
} // namespace

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
	// Resolve panel-relative fractions into absolute window fractions.
	const gfx::Rect p = Panel();
	auto rel = [&](float fx, float fy, float fw, float fh) {
		return gfx::Rect{p.x + fx * p.w, p.y + fy * p.h, fw * p.w, fh * p.h};
	};

	// Controls live in a left column when a preview pane takes the right side.
	const float cw = HasPreview() ? kCtrlCol : 1.0f;
	const float innerW = cw - 2 * kPad; // usable control width

	// Common property strip, stacked one row per control. Each row is added only
	// if it has something to show, and `rowY` walks down as they are placed, so
	// the content below starts wherever the strip actually ended.
	float rowY = kTitleH;
	bool anyRow = false;

	// Facing (label + dropdown) — omitted entirely for a fixture with no facing
	// choices (e.g. a floor brazier).
	const std::vector<Direction> choices = FacingChoices();
	if (!choices.empty()) {
		m_ui.Add<ui::Label>(rel(kPad, rowY, innerW * 0.30f, kFacingH * 0.6f),
							loc::Tr("map.insp.facing"));
		std::vector<std::string> items;
		int sel = 0;
		for (size_t i = 0; i < choices.size(); ++i) {
			items.push_back(loc::Tr(FacingLocKey(choices[i])));
			if (choices[i] == m_facing) sel = static_cast<int>(i);
		}
		m_ui.Add<ui::DropDown>(rel(kPad + innerW * 0.32f, rowY, innerW * 0.68f,
								   kFacingH * 0.7f),
							   items, sel, [this, choices](int i) {
								   if (i >= 0 && i < static_cast<int>(choices.size())) {
									   m_facing = choices[static_cast<size_t>(i)];
									   ApplyLive();
								   }
							   });
		rowY += kFacingH;
		anyRow = true;
	}

	// The optional toggle gets its OWN row and the FULL control width. It used
	// to share the facing row at 0.28 of it, which was authored when dialog text
	// drew at 1x; at kDialogTextScale a label like "Map arrow" is wider than
	// that slot and was cut off mid-word. Three controls do not fit on one row
	// at this size — the row is ~333px and the label alone wants ~200.
	if (facingExtra) {
		m_ui.Add<ui::Checkbox>(rel(kPad, rowY, innerW, kFacingH * 0.7f),
							   facingExtra->label, facingExtra->value,
							   [this](bool on) {
								   facingExtra->value = on;
								   if (facingExtra->onChange)
									   facingExtra->onChange(on);
							   });
		rowY += kFacingH;
		anyRow = true;
	}

	// Divider, then the derived content between the strip and the footer. With
	// no rows at all the content starts higher (reclaims the band).
	const float contentTop = anyRow ? rowY : kTitleH + 0.02f;
	m_ui.Add<ui::Separator>(rel(kPad, contentTop, innerW, 0.005f));
	BuildContent(rel(kPad, contentTop + 0.02f, innerW,
					 1.0f - contentTop - 0.02f - kFooterH));

	// Close: the shared box icon in the panel's top-right corner (revert, like Esc).
	ui::AddCloseButton(m_ui, p, m_closeIcon, [this] {
		Revert();
		Close();
	});

	// Footer: Save (persist), plus Delete (remove the object from the map)
	// when the owner armed it — closing lives in the "x"/Esc, not down here.
	const float gap = innerW * 0.06f, bw = (innerW - gap) * 0.5f, fy = 1.0f - kFooterH + 0.02f;
	m_ui.Add<ui::Button>(rel(kPad, fy, bw, kFooterH * 0.55f), loc::Tr("map.cfg.save"),
						 [this] {
							 Persist();
							 Close();
						 });
	if (onDelete)
		m_ui.Add<ui::Button>(rel(kPad + bw + gap, fy, bw, kFooterH * 0.55f),
							 loc::Tr("map.cfg.delete"), [this] {
								 onDelete(); // the object is gone — no Revert
								 Close();
							 });
}

gfx::Rect InstanceInspector::PreviewRect(float w, float h) const {
	if (!HasPreview()) return {0.0f, 0.0f, 0.0f, 0.0f};
	const gfx::Rect p = Panel();
	const float x1 = 1.0f - kPad, y0 = kTitleH, y1 = 1.0f - kFooterH;
	return {(p.x + kPaneX0 * p.w) * w, (p.y + y0 * p.h) * h, (x1 - kPaneX0) * p.w * w,
			(y1 - y0) * p.h * h};
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

	// The title heads the panel and gets the FULL inner width, running over the
	// preview pane's column rather than stopping short of it. Names here share
	// long prefixes — "Decoration: Wall Arch (Rustic)" vs "(Rough)" — so
	// clipping at the pane edge ellipsised away the only part that identifies
	// which type you opened. Nothing is overlapped by this: the pane starts at
	// kTitleH, BELOW the band, and the pane's own header moved under it.
	// FitDialogTitle stays as the guard for a pathologically long name.
	const gfx::Rect titleBand{panel.x + kPad * panel.w, panel.y + kPad * panel.h,
							  (1.0f - 2 * kPad) * panel.w, (kTitleH - kPad) * panel.h};
	const ui::FittedTitle title = ui::FitDialogTitle(m_ui, Title(), titleBand);
	title.font->Draw(batch, title.text, titleBand.x, titleBand.y, th.text);
	const ui::Font& font = m_ui.GetFont();

	// Preview pane backing + header (the owner blits the 3D image on top, so the
	// header cannot live INSIDE the pane). It sits UNDER the pane: above it is
	// the title band now, and the footer's buttons are all in the left column,
	// so the space beneath the pane is free.
	if (HasPreview()) {
		const gfx::Rect pv = PreviewRect(w, h);
		batch.DrawRect(pv, {0.02f, 0.02f, 0.03f, 1.0f});
		font.Draw(batch, loc::Tr("map.cfg.preview"), pv.x, pv.y + pv.h + 2.0f, th.textDim);
	}

	m_ui.Render(batch, w, h);
}

} // namespace dungeon::game
