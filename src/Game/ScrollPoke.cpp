// ============================================================================
// Game/ScrollPoke.cpp - see ScrollPoke.h.
// ============================================================================
#include "Game/ScrollPoke.h"

#include "Platform/Input.h"
#include "UI/Controls.h"
#include "UI/FontLibrary.h"

#include <cmath>
#include <format>
#include <vector>

namespace dungeon::game {

namespace {

constexpr int kRows = 120;      // longer than any window: the open list must scroll
constexpr float kNear = 0.5f;   // pixels: "exactly", give or take the float maths
constexpr float kHalfway = 1.0f; // pixels: a drag's pointer lands on whole pixels

struct Point {
	float x = 0.0f, y = 0.0f;
};
Point Centre(const gfx::Rect& r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }

} // namespace

std::string RunScrollCheck(ui::FontLibrary& fonts, float designHeight, float width,
						   float height) {
	ui::UIContext ui(fonts, ui::FontRole::Body, designHeight);
	std::vector<std::string> rows;
	rows.reserve(kRows);
	for (int i = 0; i < kRows; ++i) rows.push_back(std::format("row {}", i));
	int picked = -1;
	auto* list = ui.Add<ui::DropDown>(gfx::Rect{0.40f, 0.06f, 0.20f, 0.04f}, std::move(rows), 0,
									  [&picked](int i) { picked = i; });
	list->debugName = "scrollpoke.list";
	auto* page = ui.Add<ui::ScrollArea>(gfx::Rect{0.05f, 0.10f, 0.20f, 0.40f});
	page->debugName = "scrollpoke.page";
	page->Add<ui::Widget>()->bounds = {0.0f, 0.0f, 1.0f, 5.0f};

	// One persistent input, a frame at a time: the button stays down between
	// frames (a drag) and EndFrame clears only the edges and the wheel.
	Input in;
	const auto frame = [&] {
		ui.Update(in, width, height);
		in.EndFrame();
	};
	const auto moveTo = [&](Point p) { in.OnMouseMove(p.x, p.y); };
	const auto button = [&](bool down) { in.OnMouseButton(MouseButton::Left, down); };

	std::string failed;
	const auto fail = [&](std::string_view what) {
		if (!failed.empty()) failed += "; ";
		failed += what;
	};
	const auto verdict = [&]() {
		return failed.empty() ? std::string() : std::format("scrollpoke: FAIL - {}", failed);
	};

	// --- the list ------------------------------------------------------------
	frame(); // lay out
	moveTo(Centre(list->Pixel()));
	button(true);
	frame();
	button(false);
	frame();
	if (!list->IsOpen()) {
		fail("a press on the drop-down's face did not open its list");
		return verdict();
	}
	const ui::ScrollProbe top = list->ProbeScroll(ui);
	if (!top.span.Scrolls()) {
		fail(std::format("a {}-row list did not scroll (list {:.0f}px tall)", kRows, top.box.h));
		return verdict();
	}
	const float maxScroll = top.span.maxScroll;
	if (std::fabs(top.offset) > kNear) fail(std::format("the list opened at {:.1f}px, not its top", top.offset));
	if (std::fabs(top.step - list->Pixel().h) > kNear)
		fail(std::format("a wheel notch is {:.1f}px, not a row ({:.1f}px)", top.step, list->Pixel().h));
	if (top.thumb.h < top.span.minThumb - kNear || top.thumb.y < top.span.track.y - kNear ||
		top.thumb.y + top.thumb.h > top.span.track.y + top.span.track.h + kNear)
		fail("the thumb lies outside its track or under its minimum");

	// The wheel: one notch down over the list is one row; off it, nothing.
	moveTo(Centre(top.box));
	in.OnWheel(-1.0f);
	frame();
	const float oneNotch = list->ProbeScroll(ui).offset;
	if (std::fabs(oneNotch - top.step) > kNear)
		fail(std::format("one wheel notch moved the list {:.1f}px, not one row ({:.1f}px)", oneNotch,
						 top.step));
	// The control for the drag's hover check below: with the bar not holding
	// the pointer, a row under it lights.
	if (list->HoverItem() < 0) fail("the pointer over the list's rows lit no row");
	moveTo({2.0f, height - 2.0f});
	in.OnWheel(-1.0f);
	frame();
	if (std::fabs(list->ProbeScroll(ui).offset - oneNotch) > kNear)
		fail("a wheel notch off the open list scrolled it");
	if (!list->IsOpen()) fail("a wheel notch off the open list closed it");

	// The thumb: a press starts a drag and picks nothing; past the foot is the
	// end, half the travel is half way, and the release ends the drag.
	const auto dragToFoot = [&](std::string_view when) {
		const ui::ScrollProbe p = list->ProbeScroll(ui);
		moveTo(Centre(p.thumb));
		button(true);
		frame();
		if (!list->ProbeScroll(ui).dragging) fail(std::format("a press on the thumb {} started no drag", when));
		if (!list->IsOpen() || picked >= 0)
			fail(std::format("a press on the thumb {} picked a row or closed the list", when));
		moveTo({Centre(p.thumb).x, p.span.track.y + p.span.track.h + 40.0f});
		frame();
		const float end = list->ProbeScroll(ui).offset;
		if (std::fabs(end - maxScroll) > kNear)
			fail(std::format("the thumb dragged past the foot {} left the list at {:.1f}px of {:.1f}", when,
							 end, maxScroll));
		return Centre(p.thumb).y - p.thumb.y; // where the press held the thumb
	};
	const float grab = dragToFoot("at the top");
	{
		// Half way - with the pointer carried off the bar onto the ROWS' x. The
		// drag follows it, and the bar still holds it: no row lights under it,
		// and the release over a row picks nothing. (The thumb itself never
		// reaches a row - the rows stop short of the gutter - so this is the
		// step that sees the bar's hold.)
		const ui::ScrollProbe p = list->ProbeScroll(ui);
		const float range = p.span.track.h - p.thumb.h;
		moveTo({p.box.x + p.box.w * 0.3f, p.span.track.y + grab + range * 0.5f});
		frame();
		const float half = list->ProbeScroll(ui).offset;
		if (std::fabs(half - maxScroll * 0.5f) > kHalfway)
			fail(std::format("the thumb dragged half its travel left the list at {:.1f}px, not {:.1f}", half,
							 maxScroll * 0.5f));
		if (list->HoverItem() >= 0)
			fail(std::format("the thumb's drag carried over the rows lit row {}", list->HoverItem()));
	}
	button(false);
	frame();
	if (list->ProbeScroll(ui).dragging) fail("the release did not end the thumb's drag");
	if (!list->IsOpen() || picked >= 0) fail("the thumb's drag picked a row or closed the list");

	// The wheel far up stops at the top.
	moveTo(Centre(top.box));
	in.OnWheel(1000.0f);
	frame();
	if (std::fabs(list->ProbeScroll(ui).offset) > kNear) fail("the wheel far up did not stop at the top");

	// At the end, the last row is the last item.
	dragToFoot("again");
	button(false);
	frame();
	{
		const ui::ScrollProbe p = list->ProbeScroll(ui);
		moveTo({p.box.x + p.box.w * 0.3f, p.box.y + p.box.h - p.step * 0.5f});
		button(true);
		frame();
		button(false);
		frame();
		if (picked != kRows - 1)
			fail(std::format("a press on the last row at the end picked {}, not {}", picked, kRows - 1));
		if (list->IsOpen()) fail("picking a row did not close the list");
	}

	// --- the page ------------------------------------------------------------
	const ui::ScrollProbe pg = page->ProbeScroll();
	if (!pg.span.Scrolls()) {
		fail("a page five views tall did not scroll");
		return verdict();
	}
	moveTo(Centre(pg.box));
	in.OnWheel(-1.0f);
	frame();
	const float pageNotch = page->ProbeScroll().offset;
	if (pg.step <= 0.0f || std::fabs(pageNotch - pg.step) > kNear)
		fail(std::format("one wheel notch moved the page {:.1f}px, not its step ({:.1f}px)", pageNotch, pg.step));
	{
		const ui::ScrollProbe p = page->ProbeScroll();
		moveTo(Centre(p.thumb));
		button(true);
		frame();
		if (!page->ProbeScroll().dragging) fail("a press on the page's thumb started no drag");
		moveTo({Centre(p.thumb).x, p.span.track.y + p.span.track.h + 40.0f});
		frame();
		const float end = page->ProbeScroll().offset;
		if (std::fabs(end - p.span.maxScroll) > kNear)
			fail(std::format("the page's thumb dragged past the foot left it at {:.1f}px of {:.1f}", end,
							 p.span.maxScroll));
		button(false);
		frame();
		if (page->ProbeScroll().dragging) fail("the release did not end the page thumb's drag");
	}

	if (!failed.empty()) return verdict();
	return std::format("scrollpoke: PASS - a {}-row list: a wheel notch {:.1f}px (a row), off the list "
					   "none; the thumb dragged to the end ({:.1f}px) and half way over the rows, none lit "
					   "or picked under it; the last row picked after; a page: a notch {:.1f}px and the "
					   "thumb to the end",
					   kRows, top.step, maxScroll, pg.step);
}

} // namespace dungeon::game
