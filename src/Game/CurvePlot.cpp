// ============================================================================
// Game/CurvePlot.cpp — see CurvePlot.h.
// ============================================================================
#include "Game/CurvePlot.h"

#include "Core/Loc.h"
#include "Game/Curve.h"
#include "UI/UIContext.h"

#include <algorithm>
#include <cmath>

namespace dungeon::game {

namespace {

// The dice deviation both curves are judged against: two opposed d100s, each
// open-ended. MEASURED by tools/RollTest rather than derived here — a d100's
// own deviation is ~28.9, and the escalation fattens it, so the honest figure
// comes from the harness that rolls the real engine.
constexpr float kDiceDeviation = 41.0f;

// A polyline as a run of thin rects, one per pixel column. Cheap, and it needs
// no line primitive — SpriteBatch draws rects, triangles and sprites, and a
// curve of a few hundred columns is nothing beside a frame of dungeon.
void PlotCurve(gfx::SpriteBatch& batch, const gfx::Rect& area, float maxX,
			   float maxY, const CurveRules& rules, const Vec4& color,
			   float thickness) {
	if (area.w < 2.0f || maxX <= 0.0f || maxY <= 0.0f) return;
	const int columns = static_cast<int>(area.w);
	float prevY = 0.0f;
	for (int i = 0; i <= columns; ++i) {
		const float t = static_cast<float>(i) / static_cast<float>(columns);
		const float v = CurveValue(t * maxX, rules);
		const float y = area.y + area.h - std::clamp(v / maxY, 0.0f, 1.0f) * area.h;
		if (i > 0) {
			// Span the gap between this sample and the last, so a steep
			// section stays a connected line instead of a dotted one.
			const float top = std::min(prevY, y);
			const float h = std::max(std::fabs(y - prevY), thickness);
			batch.DrawRect({area.x + static_cast<float>(i) - 1.0f, top, thickness, h},
						   color);
		}
		prevY = y;
	}
}

} // namespace

void CurvePlot::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	if (!m_balance) return;
	const ui::Theme& theme = ctx.GetTheme();
	const gfx::Rect& px = Pixel();
	const ui::Font& font = TextFont();

	// Room for the axis labels along the bottom and the legend along the top.
	const float pad = Rem(0.4f);
	const float labelH = font.Height();
	const gfx::Rect area{px.x + pad, px.y + labelH + pad,
						 px.w - pad * 2.0f,
						 px.h - labelH * 2.0f - pad * 3.0f};
	if (area.w <= 4.0f || area.h <= 4.0f) return;

	batch.DrawRect(px, {0, 0, 0, 0.25f}); // the plot's own well
	const Vec4 axis{theme.textDim.x, theme.textDim.y, theme.textDim.z, 0.5f};
	batch.DrawRect({area.x, area.y + area.h, area.w, 1.0f}, axis);
	batch.DrawRect({area.x, area.y, 1.0f, area.h}, axis);

	// The vertical span. Both curves share it so they can be COMPARED — a stat
	// curve drawn to its own scale would look as mighty as the skill curve and
	// tell the exact opposite of the truth.
	const CurveRules skill = m_balance->SkillCurve();
	const CurveRules stat = m_balance->StatCurve();
	const float maxY =
		std::max({CurveValue(maxSkill, skill), CurveValue(maxStat, stat),
				  kDiceDeviation * 1.2f, 1.0f});

	// The dice line — see the header: this is what says whether a curve is
	// doing anything a fight can feel.
	const float diceY = area.y + area.h - (kDiceDeviation / maxY) * area.h;
	const Vec4 diceColor{0.85f, 0.35f, 0.35f, 0.55f};
	for (float x = area.x; x < area.x + area.w; x += 8.0f) // dashed
		batch.DrawRect({x, diceY, 4.0f, 1.0f}, diceColor);

	PlotCurve(batch, area, maxSkill, maxY, skill, theme.accent, 2.0f);
	PlotCurve(batch, area, maxStat, maxY, stat, theme.text, 2.0f);

	// Legend across the top, each entry in the colour of its line.
	const float ly = px.y;
	float lx = px.x + pad;
	font.Draw(batch, loc::Format("map.balance.curve.skill",
								 static_cast<int>(maxSkill)),
			  lx, ly, theme.accent);
	lx += font.MeasureWidth(loc::Format("map.balance.curve.skill",
										static_cast<int>(maxSkill))) +
		  Rem(1.0f);
	font.Draw(batch, loc::Format("map.balance.curve.stat",
								 static_cast<int>(maxStat)),
			  lx, ly, theme.text);
	lx += font.MeasureWidth(loc::Format("map.balance.curve.stat",
										static_cast<int>(maxStat))) +
		  Rem(1.0f);
	font.Draw(batch, loc::Tr("map.balance.curve.dice"), lx, ly, diceColor);

	// The value axis, stated rather than gridded: the peak each curve reaches
	// at the right edge is the number being tuned.
	const float by = area.y + area.h + pad;
	font.Draw(batch,
			  loc::Format("map.balance.curve.peak",
						  static_cast<int>(CurveValue(maxSkill, skill) + 0.5f),
						  static_cast<int>(CurveValue(maxStat, stat) + 0.5f)),
			  area.x, by, theme.textDim);
}

} // namespace dungeon::game
