// ============================================================================
// Game/MonsterConfigDialog.cpp — see MonsterConfigDialog.h.
// ============================================================================
#include "Game/MonsterConfigDialog.h"

#include "Core/Loc.h"
#include "UI/Controls.h" // ui::DrawBorder

#include <algorithm>

namespace dungeon::game {

namespace {
// Display name for a state (loc key anim.state.<token>; the key itself shows if
// unmapped, never fatal — see Core/Loc).
std::string StateLabel(anim::CreatureState s) {
	return loc::Tr("anim.state." + std::string(anim::StateName(s)));
}

// The state a clip belongs to (library clips are named <state>__<clip>): the
// token before "__", or the whole name if it is itself a state token, else empty
// (so a plain hand-authored "idle" clip still maps to Idle; an un-prefixed clip
// that isn't a token maps to no state and shows under none).
std::string ClipState(const std::string& name) {
	const size_t sep = name.find("__");
	const std::string head = sep == std::string::npos ? name : name.substr(0, sep);
	return anim::ParseState(head) ? head : std::string();
}

// A clip name without its "<state>__" prefix, for display.
std::string ClipLabel(const std::string& name) {
	const size_t sep = name.find("__");
	return sep == std::string::npos ? name : name.substr(sep + 2);
}
} // namespace

MonsterConfigDialog::MonsterConfigDialog(gfx::GraphicsDevice& device)
	: m_device(device), m_font(device, "", 18.0f) {}

void MonsterConfigDialog::Open(const std::string& type, const std::string& display,
							   const Support& supported, const Clips& clips,
							   const std::vector<std::string>& modelClips) {
	m_open = true;
	m_display = display;
	m_cfg = {type, supported, clips};
	m_original = m_cfg; // snapshot for revert
	m_modelClips = modelClips;
	m_selState = static_cast<int>(anim::CreatureState::Idle);
	m_selClip = FirstClipOf(m_selState); // auto-preview the first clip of the state
	m_clipScroll = 0.0f;
}

// Resolves the centered panel, the two columns, the rows and the footer buttons.
// Clip rows carry their model-clip index and drop out of the list when scrolled
// past the column viewport, so Update (hit-test) and Render (draw) agree.
MonsterConfigDialog::Layout MonsterConfigDialog::BuildLayout(float w, float h) const {
	Layout L;
	L.panel = {0.16f * w, 0.12f * h, 0.68f * w, 0.76f * h};
	const float pad = std::clamp(L.panel.w * 0.02f, 8.0f, 24.0f);
	const float fh = m_font.Height();
	L.rowH = std::clamp(fh * 1.7f, 20.0f, 40.0f);
	const float titleB = L.panel.y + pad + fh * 1.4f; // below the title
	const float colTop = titleB + L.rowH;             // below the column headers
	const float footerH = std::clamp(fh * 1.9f, 24.0f, 44.0f);
	const float footerY = L.panel.y + L.panel.h - pad - footerH;

	// Three columns: States | clip list | preview pane.
	const float innerW = L.panel.w - 2.0f * pad;
	const float colH = footerY - colTop - pad;
	L.statesCol = {L.panel.x + pad, colTop, innerW * 0.26f, colH};
	L.clipsCol = {L.statesCol.x + L.statesCol.w + pad, colTop, innerW * 0.32f, colH};
	L.previewCol = {L.clipsCol.x + L.clipsCol.w + pad, colTop,
					innerW - L.statesCol.w - L.clipsCol.w - 2.0f * pad, colH};

	const float box = L.rowH * 0.55f;
	for (int i = 0; i < N; ++i) {
		const float y = L.statesCol.y + i * L.rowH;
		L.stateRow[i] = {L.statesCol.x, y, L.statesCol.w, L.rowH};
		L.stateCheck[i] = {L.statesCol.x + pad, y + (L.rowH - box) * 0.5f, box, box};
	}

	// Only the SELECTED state's animations (name-encoded, or already assigned);
	// clipOf keeps the index into m_modelClips so a toggle still uses the full name.
	const float clipBottom = L.clipsCol.y + L.clipsCol.h;
	int ord = 0; // candidate ordinal (drives row Y + content height)
	for (int i = 0; i < static_cast<int>(m_modelClips.size()); ++i) {
		if (!ClipBelongs(m_modelClips[i], m_selState)) continue;
		const float y = L.clipsCol.y - m_clipScroll + ord * L.rowH;
		++ord;
		if (y + L.rowH <= L.clipsCol.y || y >= clipBottom) continue; // scrolled out
		L.clipRow.push_back({L.clipsCol.x, y, L.clipsCol.w, L.rowH});
		L.clipCheck.push_back({L.clipsCol.x, y + (L.rowH - box) * 0.5f, box, box});
		L.clipOf.push_back(i);
	}
	L.clipContentH = ord * L.rowH;

	const float btnW = std::clamp(L.panel.w * 0.16f, 80.0f, 180.0f);
	L.close = {L.panel.x + L.panel.w - pad - btnW, footerY, btnW, footerH};
	L.save = {L.close.x - pad - btnW, footerY, btnW, footerH};
	m_previewColCache = L.previewCol; // so PreviewRect need not rebuild the layout
	return L;
}

bool MonsterConfigDialog::ClipBelongs(const std::string& name, int state) const {
	const std::string token(anim::StateName(static_cast<anim::CreatureState>(state)));
	if (ClipState(name) == token) return true;
	const auto& vec = m_cfg.clips[state]; // already-assigned clips stay visible/editable
	return std::find(vec.begin(), vec.end(), name) != vec.end();
}

std::string MonsterConfigDialog::FirstClipOf(int state) const {
	for (const std::string& c : m_modelClips)
		if (ClipBelongs(c, state)) return c;
	return {};
}

gfx::Rect MonsterConfigDialog::PreviewRect(float /*w*/, float /*h*/) const {
	// The preview pane fills the whole preview column (rendered at that aspect, so
	// the tall box doesn't distort). Return the rect the last BuildLayout cached
	// rather than rebuilding the layout just to read one rect.
	return m_previewColCache;
}

void MonsterConfigDialog::Update(const Input& input, float w, float h) {
	if (!m_open) return;
	m_font.Commit(); // flush glyphs cached last frame before this frame draws
	m_font.SetHeight(std::clamp(h * 0.022f, 12.0f, 26.0f));

	if (input.WasKeyPressed(VK_ESCAPE)) { // cancel: revert live to the snapshot
		if (onApply) onApply(m_original);
		Close();
		return;
	}

	const float mx = input.MouseX(), my = input.MouseY();
	const Layout L = BuildLayout(w, h);

	// Wheel over the clip column scrolls it.
	if (input.WheelDelta() != 0.0f && L.clipsCol.Contains(mx, my)) {
		const float maxScroll = std::max(0.0f, L.clipContentH - L.clipsCol.h);
		m_clipScroll = std::clamp(m_clipScroll - input.WheelDelta() * L.rowH, 0.0f, maxScroll);
	}

	if (!input.WasMousePressed(MouseButton::Left)) return;

	if (L.save.Contains(mx, my)) {
		if (onSave) onSave(m_cfg);
		Close();
		return;
	}
	if (L.close.Contains(mx, my)) { // cancel: revert
		if (onApply) onApply(m_original);
		Close();
		return;
	}

	// State column: the checkbox toggles supported; the rest of the row selects it.
	for (int i = 0; i < N; ++i) {
		if (L.stateCheck[i].Contains(mx, my)) {
			if (i != static_cast<int>(anim::CreatureState::Idle)) { // Idle is the floor
				m_cfg.supported[i] = !m_cfg.supported[i];
				Apply();
			}
			return;
		}
		if (L.stateRow[i].Contains(mx, my)) {
			if (m_selState != i) {
				m_selState = i;
				m_clipScroll = 0.0f;
				m_selClip = FirstClipOf(i); // preview the state's first clip
			}
			return;
		}
	}

	// Clip column: the checkbox toggles valid; the rest of the row selects the clip
	// for preview.
	for (size_t r = 0; r < L.clipRow.size(); ++r) {
		const std::string& name = m_modelClips[L.clipOf[r]];
		if (L.clipCheck[r].Contains(mx, my)) {
			auto& vec = m_cfg.clips[m_selState];
			if (const auto it = std::find(vec.begin(), vec.end(), name); it != vec.end())
				vec.erase(it);
			else
				vec.push_back(name);
			Apply();
			return;
		}
		if (L.clipRow[r].Contains(mx, my)) {
			m_selClip = name; // select for preview
			return;
		}
	}
}

void MonsterConfigDialog::Render(gfx::SpriteBatch& batch, const ui::Theme& th,
								 float w, float h) {
	if (!m_open) return;
	const Layout L = BuildLayout(w, h);
	const float pad = std::clamp(L.panel.w * 0.02f, 8.0f, 24.0f);
	const float fh = m_font.Height();

	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.6f}); // dim the editor behind
	batch.DrawRect(L.panel, th.panel);
	ui::DrawBorder(batch, L.panel, th.panelBorder);

	// Draws a checkbox: bordered box, filled accent when checked.
	auto checkbox = [&](const gfx::Rect& box, bool on) {
		batch.DrawRect(box, th.control);
		ui::DrawBorder(batch, box, th.panelBorder);
		if (on) {
			const float in = box.w * 0.22f;
			batch.DrawRect({box.x + in, box.y + in, box.w - 2 * in, box.h - 2 * in},
						   th.accent);
		}
	};
	auto textY = [&](const gfx::Rect& r) { return r.y + (r.h - fh) * 0.5f; };

	// Title + column headers.
	m_font.Draw(batch, loc::Format("map.cfg.title", m_display), L.panel.x + pad,
				L.panel.y + pad, th.text);
	m_font.Draw(batch, loc::Tr("map.cfg.states"), L.statesCol.x,
				L.statesCol.y - L.rowH + (L.rowH - fh) * 0.5f, th.textDim);
	m_font.Draw(batch,
				loc::Format("map.cfg.anims",
							StateLabel(static_cast<anim::CreatureState>(m_selState))),
				L.clipsCol.x, L.clipsCol.y - L.rowH + (L.rowH - fh) * 0.5f, th.textDim);

	// State rows.
	for (int i = 0; i < N; ++i) {
		const auto s = static_cast<anim::CreatureState>(i);
		if (i == m_selState) {
			batch.DrawRect(L.stateRow[i], th.controlActive);
			ui::DrawBorder(batch, L.stateRow[i], th.panelBorder);
		}
		checkbox(L.stateCheck[i], m_cfg.supported[i]);
		const float lx = L.stateCheck[i].x + L.stateCheck[i].w + pad * 0.6f;
		m_font.Draw(batch, StateLabel(s), lx, textY(L.stateRow[i]),
					i == m_selState ? th.text : th.textDim);
	}

	// Clip column: the selected state's animations (scissored so scrolled rows clip
	// to the column). The checkbox = valid; the highlighted row = previewing. Empty
	// when the model has no clips for this state.
	batch.SetScissor(&L.clipsCol);
	if (L.clipContentH <= 0.0f) {
		m_font.Draw(batch, loc::Tr("map.cfg.noclips"), L.clipsCol.x,
					L.clipsCol.y + (L.rowH - fh) * 0.5f, th.textDim);
	} else {
		const auto& vec = m_cfg.clips[m_selState];
		for (size_t r = 0; r < L.clipRow.size(); ++r) {
			const std::string& name = m_modelClips[L.clipOf[r]]; // full <state>__<clip>
			const bool on = std::find(vec.begin(), vec.end(), name) != vec.end();
			const bool sel = name == m_selClip; // being previewed
			const gfx::Rect& row = L.clipRow[r];
			if (sel) {
				batch.DrawRect(row, th.controlActive);
				ui::DrawBorder(batch, row, th.panelBorder);
			}
			checkbox(L.clipCheck[r], on);
			const float lx = L.clipCheck[r].x + L.clipCheck[r].w + pad * 0.6f;
			m_font.Draw(batch, ClipLabel(name), lx, textY(row),
						(on || sel) ? th.text : th.textDim);
		}
	}
	batch.SetScissor(nullptr);

	// Preview pane: header + backing box (Game blits the live looping animation
	// into PreviewRect on top). A hint shows when nothing is selected.
	m_font.Draw(batch, loc::Tr("map.cfg.preview"), L.previewCol.x,
				L.previewCol.y - L.rowH + (L.rowH - fh) * 0.5f, th.textDim);
	const gfx::Rect pv = L.previewCol; // already built this frame; no extra BuildLayout
	batch.DrawRect(pv, {0.02f, 0.02f, 0.03f, 1.0f});
	ui::DrawBorder(batch, pv, th.panelBorder);
	if (m_selClip.empty()) {
		const std::string hint = loc::Tr("map.cfg.nopreview");
		m_font.Draw(batch, hint, pv.x + (pv.w - m_font.MeasureWidth(hint)) * 0.5f,
					pv.y + pv.h * 0.5f - fh * 0.5f, th.textDim);
	}

	// Footer buttons.
	auto button = [&](const gfx::Rect& r, const std::string& label, const Vec4& bg) {
		batch.DrawRect(r, bg);
		ui::DrawBorder(batch, r, th.panelBorder);
		m_font.Draw(batch, label, r.x + (r.w - m_font.MeasureWidth(label)) * 0.5f,
					textY(r), th.text);
	};
	button(L.save, loc::Tr("map.cfg.save"), th.controlActive);
	button(L.close, loc::Tr("map.cfg.close"), th.control);
}

} // namespace dungeon::game
