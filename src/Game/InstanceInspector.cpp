// ============================================================================
// Game/InstanceInspector.cpp — see InstanceInspector.h.
// ============================================================================
#include "Game/InstanceInspector.h"

#include "Core/Loc.h"
#include "UI/Controls.h"

#include <algorithm>

namespace dungeon::game {

namespace {
// Sub-region layout WITHIN the derived Panel(), as fractions of that panel. The
// common strip (title + facing) is fixed height at the top; the footer (Save/
// Close) is fixed at the bottom; the derived content fills the middle.
constexpr float kPad = 0.04f;      // panel inner margin (fraction of panel w/h)
constexpr float kTitleH = 0.12f;   // title band
constexpr float kFacingH = 0.14f;  // facing row
constexpr float kFooterH = 0.14f;  // Save/Close band
// When a preview pane is shown, the controls occupy the left column up to this
// panel-width fraction; the pane fills the rest.
constexpr float kCtrlCol = 0.60f;
constexpr float kPaneX0 = 0.62f; // preview pane left edge (panel fraction)
} // namespace

InstanceInspector::InstanceInspector(gfx::GraphicsDevice& device)
	: m_device(device), m_font(device, "", 18.0f), m_ui(device, "", 18.0f) {}

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

	// Common property strip: Facing (label + dropdown on one row).
	m_ui.Add<ui::Label>(rel(kPad, kTitleH, innerW * 0.34f, kFacingH * 0.6f),
						loc::Tr("map.insp.facing"));
	const std::vector<Direction> choices = FacingChoices();
	std::vector<std::string> items;
	int sel = 0;
	for (size_t i = 0; i < choices.size(); ++i) {
		items.push_back(loc::Tr(FacingLocKey(choices[i])));
		if (choices[i] == m_facing) sel = static_cast<int>(i);
	}
	if (!choices.empty())
		m_ui.Add<ui::DropDown>(rel(kPad + innerW * 0.36f, kTitleH, innerW * 0.64f, kFacingH * 0.7f),
							   items, sel, [this, choices](int i) {
								   if (i >= 0 && i < static_cast<int>(choices.size())) {
									   m_facing = choices[static_cast<size_t>(i)];
									   ApplyLive();
								   }
							   });

	// Divider, then the derived content between the strip and the footer.
	const float contentTop = kTitleH + kFacingH;
	m_ui.Add<ui::Separator>(rel(kPad, contentTop, innerW, 0.005f));
	BuildContent(rel(kPad, contentTop + 0.02f, innerW,
					 1.0f - contentTop - 0.02f - kFooterH));

	// Footer: Save (persist) + Close (revert), filling the control column.
	const float gap = innerW * 0.06f, bw = (innerW - gap) * 0.5f, fy = 1.0f - kFooterH + 0.02f;
	m_ui.Add<ui::Button>(rel(kPad, fy, bw, kFooterH * 0.55f), loc::Tr("map.cfg.save"),
						 [this] {
							 Persist();
							 Close();
						 });
	m_ui.Add<ui::Button>(rel(kPad + bw + gap, fy, bw, kFooterH * 0.55f),
						 loc::Tr("map.cfg.close"), [this] {
							 Revert();
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
	m_font.Commit();
	const float fh = std::clamp(h * 0.020f, 12.0f, 24.0f);
	m_font.SetHeight(fh);
	m_ui.GetFont().SetHeight(fh);

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

	m_font.Draw(batch, Title(), panel.x + kPad * panel.w, panel.y + kPad * panel.h, th.text);

	// Preview pane backing + header (the owner blits the 3D image on top).
	if (HasPreview()) {
		const gfx::Rect pv = PreviewRect(w, h);
		m_font.Draw(batch, loc::Tr("map.cfg.preview"), pv.x, pv.y - m_font.Height() - 2.0f,
					th.textDim);
		batch.DrawRect(pv, {0.02f, 0.02f, 0.03f, 1.0f});
	}

	m_ui.Render(batch, w, h);
}

} // namespace dungeon::game
