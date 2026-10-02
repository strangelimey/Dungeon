// ============================================================================
// Game/StonePicker.cpp - see StonePicker.h.
// ============================================================================
#include "Game/StonePicker.h"

#include "Graphics/SpriteBatch.h"
#include "Graphics/Texture.h"
#include "Platform/Input.h"
#include "UI/Controls.h"
#include "UI/Font.h"
#include "UI/Skin.h"
#include "UI/UIContext.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace dungeon::game {

namespace {

// A tile, in rem: the thumbnail square, the name line under it, and the gap
// between tiles both ways. Sized with kNameScale so the longest English name,
// "Banded limestone", fits under its own thumbnail (TextOverrun reports any
// name that does not, so a longer translation shows up in `uioverlap`).
constexpr float kTileRem = 5.6f;
constexpr float kNameRem = 1.3f;
constexpr float kGapRem = 0.8f;
// The names are captions, a size under the page's controls.
constexpr float kNameScale = 0.85f;

char Lower(char c) {
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Case-insensitive (ASCII) substring test, allocation-free.
bool ContainsNoCase(std::string_view hay, std::string_view lowNeedle) {
	if (lowNeedle.empty()) return true;
	if (lowNeedle.size() > hay.size()) return false;
	for (size_t i = 0; i + lowNeedle.size() <= hay.size(); ++i) {
		size_t k = 0;
		while (k < lowNeedle.size() && Lower(hay[i + k]) == lowNeedle[k]) ++k;
		if (k == lowNeedle.size()) return true;
	}
	return false;
}

} // namespace

StonePicker::StonePicker(const gfx::Rect& rect, std::vector<Entry> entries,
						 std::string current, std::function<void(const std::string&)> onPick)
	: m_entries(std::move(entries)), m_current(std::move(current)),
	  m_onPick(std::move(onPick)) {
	bounds = rect;
	debugName = "StonePicker";
	fontScale = kNameScale;
}

float StonePicker::TextOverrun() const {
	float worst = 0.0f;
	const float tile = kTileRem * Rem(1.0f);
	for (const Entry& e : m_entries)
		if (Matches(e)) worst = std::max(worst, TextFont().MeasureWidth(e.label) - tile);
	return worst;
}

void StonePicker::SetFilter(std::string_view text) {
	m_filter.assign(text);
	for (char& c : m_filter) c = Lower(c);
}

void StonePicker::SetShow(Show show) { m_show = show; }

void StonePicker::SetThumb(std::string_view name, const gfx::Texture* thumb) {
	for (Entry& e : m_entries)
		if (e.name == name) e.thumb = thumb;
}

bool StonePicker::Matches(const Entry& e) const {
	if (e.always) return true;
	if (!m_family.empty() && e.family != m_family) return false;
	if (m_show == Show::Light && e.luminance < kLightFrom) return false;
	if (m_show == Show::Dark && e.luminance >= kLightFrom) return false;
	// The label is what the player reads; the stem is what an ini or a bug
	// report says - either finds it.
	return ContainsNoCase(e.label, m_filter) || ContainsNoCase(e.name, m_filter);
}

size_t StonePicker::ShownCount() const {
	size_t n = 0;
	for (const Entry& e : m_entries)
		if (Matches(e)) ++n;
	return n;
}

int StonePicker::Columns(float widthPx, float remPx) const {
	const float tile = kTileRem * remPx, gap = kGapRem * remPx;
	if (tile <= 0.0f) return 1;
	return std::max(1, static_cast<int>(std::floor((widthPx + gap) / (tile + gap))));
}

float StonePicker::FitExtent(float crossPx, float remPx) const {
	const size_t n = ShownCount();
	if (n == 0) return kNameRem * remPx; // room for the "no stone matches" line
	const size_t cols = static_cast<size_t>(Columns(crossPx, remPx));
	const size_t rows = (n + cols - 1) / cols;
	return static_cast<float>(rows) * (kTileRem + kNameRem) * remPx +
		   static_cast<float>(rows - 1) * kGapRem * remPx;
}

gfx::Rect StonePicker::TileRect(size_t slot) const {
	const gfx::Rect& px = Pixel();
	const float rem = Rem(1.0f);
	const size_t cols = static_cast<size_t>(Columns(px.w, rem));
	const float tile = kTileRem * rem, gap = kGapRem * rem;
	const float col = static_cast<float>(slot % cols), row = static_cast<float>(slot / cols);
	return {px.x + col * (tile + gap), px.y + row * (tile + kNameRem * rem + gap), tile,
			tile + kNameRem * rem};
}

void StonePicker::UpdateSelf(ui::UIContext& ctx) {
	m_hot = -1;
	const Input* input = ctx.CurrentInput();
	if (!input || ctx.IsMouseConsumed()) return;
	const float mx = input->MouseX(), my = input->MouseY();
	size_t slot = 0;
	for (size_t i = 0; i < m_entries.size(); ++i) {
		if (!Matches(m_entries[i])) continue;
		if (TileRect(slot++).Contains(mx, my)) {
			// Only a tile claims the pointer - the gaps between them are page.
			m_hot = static_cast<int>(i);
			ctx.ConsumeMouse();
			if (input->WasMousePressed(MouseButton::Left) && m_entries[i].name != m_current) {
				m_current = m_entries[i].name;
				if (m_onPick) m_onPick(m_current);
			}
			return;
		}
	}
}

void StonePicker::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const ui::Theme& theme = ctx.GetTheme();
	const ui::Font& font = TextFont();
	const ui::Skin* skin = ctx.GetSkin();
	const float rem = Rem(1.0f);
	const float tile = kTileRem * rem;

	size_t slot = 0, materials = 0;
	for (size_t i = 0; i < m_entries.size(); ++i) {
		const Entry& e = m_entries[i];
		if (!Matches(e)) continue;
		if (!e.always) ++materials;
		const gfx::Rect r = TileRect(slot++);
		const gfx::Rect thumb{r.x, r.y, tile, tile};
		if (e.thumb) batch.DrawSprite(thumb, {0, 0, 1, 1}, *e.thumb, {1, 1, 1, 1});
		else batch.DrawRect(thumb, theme.control);
		// The panel's own bevel over it: the tile is a slab of the chrome.
		if (skin) ui::DrawNineSlice(batch, thumb, skin->panel, {1, 1, 1, 1});
		if (static_cast<int>(i) == m_hot) batch.DrawRect(thumb, {1, 1, 1, 0.08f});

		const bool current = e.name == m_current;
		if (current) {
			// Two hairlines, so the mark reads on a light stone as well as a dark.
			ui::DrawBorder(batch, thumb, theme.accent);
			ui::DrawBorder(batch, {thumb.x + 1, thumb.y + 1, thumb.w - 2, thumb.h - 2},
						   theme.accent);
		}
		ui::DrawFittedText(batch, font, e.label, r.x, r.y + tile + 0.15f * rem, tile,
						   current ? theme.accent : theme.textDim);
	}
	// Nothing matched but the follow tile: say so where the next tile would be.
	if (materials == 0 && !emptyText.empty()) {
		const gfx::Rect r = TileRect(slot);
		font.Draw(batch, emptyText, r.x, r.y + (tile - font.LineAdvance()) * 0.5f,
				  theme.textDim);
	}
}

} // namespace dungeon::game
