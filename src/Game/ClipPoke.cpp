// ============================================================================
// Game/ClipPoke.cpp - see ClipPoke.h.
// ============================================================================
#include "Game/ClipPoke.h"

#include "Core/Log.h"
#include "UI/Controls.h"
#include "UI/FontLibrary.h"

#include <format>
#include <stdexcept>

namespace dungeon::game {

namespace {

// A scroll area clips only while it has something to scroll, so each one in
// these trees holds a child taller than itself.
constexpr gfx::Rect kTaller{0.0f, 0.0f, 1.0f, 3.0f};

// True when the two rects share no pixel.
bool Disjoint(const gfx::Rect& a, const gfx::Rect& b) {
	return a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y;
}

// The throwing child: its walk runs inside the scroll area's clip.
class Thrower : public ui::Widget {
public:
	explicit Thrower(const gfx::Rect& rect) { bounds = rect; }
	bool armed = false;
	// The button the harness clicks afterwards (set by ClipThrowPoke).
	const ui::Widget* outside = nullptr;

protected:
	void UpdateSelf(ui::UIContext&) override {
		if (!armed) return;
		armed = false; // one throw; the walks after it are the test
		// The throw tests the fix only if a clip is in force HERE and the button
		// lies wholly outside it: a scroll area clips only while it overflows, and
		// with no clip - or one over the button - the click lands under the old
		// walk too, and the case passes having tested nothing. So the message
		// says which it was, and HealthTest's `uiclip` expects the first form.
		const gfx::Rect* clip = ui::ActiveClip();
		if (!clip)
			throw std::runtime_error(
				"crashpoke uiclip: NO clip was in force at the throw - the case tests nothing");
		const gfx::Rect b = outside ? outside->Pixel() : gfx::Rect{};
		if (!outside || !Disjoint(b, *clip))
			throw std::runtime_error(std::format(
				"crashpoke uiclip: the button [{:.0f},{:.0f} {:.0f}x{:.0f}] is not outside the "
				"clip [{:.0f},{:.0f} {:.0f}x{:.0f}] - the case tests nothing",
				b.x, b.y, b.w, b.h, clip->x, clip->y, clip->w, clip->h));
		throw std::runtime_error(std::format(
			"crashpoke: a deliberate throw inside a scroll area's walk, under its clip "
			"[{:.0f},{:.0f} {:.0f}x{:.0f}], the button [{:.0f},{:.0f} {:.0f}x{:.0f}] outside it",
			clip->x, clip->y, clip->w, clip->h, b.x, b.y, b.w, b.h));
	}
};

// A sibling the nesting check asks about: did the walk let it take a click, and
// did it draw with its whole rect inside the clip in force (none counts)?
class Probe : public ui::Widget {
public:
	explicit Probe(const gfx::Rect& rect) { bounds = rect; }
	bool clicked = false;
	bool drew = false;      // DrawSelf ran at all
	bool unclipped = false; // ...and nothing clipped any of it away

protected:
	void UpdateSelf(ui::UIContext& ctx) override {
		const Input* in = ctx.CurrentInput();
		if (!in || ctx.IsMouseConsumed() || !Pixel().Contains(in->MouseX(), in->MouseY()))
			return;
		if (in->WasMousePressed(MouseButton::Left)) clicked = true;
		ctx.ConsumeMouse();
	}
	void DrawSelf(ui::UIContext&, gfx::SpriteBatch&) override {
		drew = true;
		const gfx::Rect& px = Pixel();
		const gfx::Rect* clip = ui::ActiveClip();
		constexpr float kSlack = 0.5f; // pixels
		unclipped = !clip || (px.x >= clip->x - kSlack && px.y >= clip->y - kSlack &&
							  px.x + px.w <= clip->x + clip->w + kSlack &&
							  px.y + px.h <= clip->y + clip->h + kSlack);
	}
};

// One frame's input: the pointer at the centre of `at`, the left button pressed.
Input ClickAt(const gfx::Rect& at) {
	Input input;
	input.OnMouseMove(at.x + at.w * 0.5f, at.y + at.h * 0.5f);
	input.OnMouseButton(MouseButton::Left, true);
	return input;
}

} // namespace

// --- the throw ---------------------------------------------------------------

ClipThrowPoke::ClipThrowPoke(ui::FontLibrary& fonts, float designHeight)
	: m_ui(fonts, ui::FontRole::Body, designHeight) {
	// Over the 3D view, clear of the HUD's default panels: the area high in the
	// middle, the button under it. A click there also reaches the world, where
	// it meets wall or ceiling.
	m_area = m_ui.Add<ui::ScrollArea>(gfx::Rect{0.42f, 0.22f, 0.16f, 0.10f});
	m_area->debugName = "clippoke.area";
	auto* thrower = m_area->Add<Thrower>(kTaller);
	m_armed = &thrower->armed;
	m_button = m_ui.Add<ui::Button>(gfx::Rect{0.45f, 0.40f, 0.10f, 0.06f}, "uiclip", [this] {
		++m_clicks;
		log::Info("crashpoke uiclip: a click outside the scroll area landed (click {})",
				  m_clicks);
	});
	m_button->fireOnPress = true; // acts on the press: no push to wait out
	m_button->debugName = "clippoke.button";
	thrower->outside = m_button;
}

void ClipThrowPoke::Layout(float width, float height) { m_ui.Update(Input{}, width, height); }

gfx::Rect ClipThrowPoke::ButtonRect() const { return m_button->Pixel(); }
gfx::Rect ClipThrowPoke::AreaRect() const { return m_area->Pixel(); }

void ClipThrowPoke::Arm() { *m_armed = true; }

void ClipThrowPoke::Update(const Input& input, float width, float height) {
	m_ui.Update(input, width, height);
}

// --- the nesting -------------------------------------------------------------

std::string RunNestedClipCheck(ui::FontLibrary& fonts, float designHeight,
							   gfx::SpriteBatch& batch, float width, float height) {
	ui::UIContext ui(fonts, ui::FontRole::Body, designHeight);
	auto* tabs = ui.Add<ui::TabControl>(gfx::Rect{0.10f, 0.10f, 0.40f, 0.60f}, 0.1f);
	const size_t tab = tabs->AddTab("clippoke");
	// The page's rows in ADD order: a sibling, the inner area, a sibling, and a
	// spacer past the page's bottom so the page scrolls - and clips.
	auto* before = tabs->AddChild<Probe>(tab, gfx::Rect{0.05f, 0.05f, 0.90f, 0.12f});
	auto* inner = tabs->AddChild<ui::ScrollArea>(tab, gfx::Rect{0.05f, 0.25f, 0.90f, 0.25f});
	inner->Add<ui::Widget>()->bounds = kTaller;
	auto* after = tabs->AddChild<Probe>(tab, gfx::Rect{0.05f, 0.60f, 0.90f, 0.12f});
	tabs->AddChild<ui::Widget>(tab)->bounds = {0.0f, 1.0f, 1.0f, 0.5f};

	ui.Update(Input{}, width, height); // lay out, so the probes' rects are real
	const bool pageClips = tabs->Page(tab)->ClipsChildren();
	const bool innerClips = inner->ClipsChildren();
	ui.Update(ClickAt(before->Pixel()), width, height);
	ui.Update(ClickAt(after->Pixel()), width, height);
	ui.Render(batch, width, height);
	const bool leftover = ui::ActiveClip() != nullptr;

	std::string failed;
	const auto fail = [&](std::string_view what) {
		if (!failed.empty()) failed += "; ";
		failed += what;
	};
	// Not a check of the fix: without both clips there is no nesting to test.
	if (!pageClips || !innerClips) fail("the tree did not nest two clips");
	if (!before->clicked) fail("the sibling before the inner area missed its click");
	if (!after->clicked) fail("the sibling after the inner area missed its click");
	if (!before->drew || !before->unclipped) fail("the sibling before the inner area was clipped");
	if (!after->drew || !after->unclipped) fail("the sibling after the inner area was clipped");
	if (leftover) fail("a clip was still in force after the walks");
	return failed.empty()
			   ? std::string("clippoke: PASS - both siblings of a nested scroll area took "
							 "their clicks and drew under the page's clip")
			   : std::format("clippoke: FAIL - {}", failed);
}

} // namespace dungeon::game
