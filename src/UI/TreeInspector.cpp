// ============================================================================
// UI/TreeInspector.cpp — see TreeInspector.h.
// ============================================================================
#include "UI/TreeInspector.h"

#include "UI/Controls.h"
#include "UI/UIContext.h"
#include "UI/Widget.h"

#include <algorithm>
#include <format>
#include <set>
#include <typeinfo>
#include <vector>

namespace dungeon::ui::inspect {

namespace {

bool g_enabled = false;

// One tint per nesting level, cycled — sibling boxes at the same depth read as
// a set, and a child's box reads as a different level from its parent's.
constexpr Vec4 kDepthTint[] = {
	{0.35f, 0.75f, 1.00f, 1.0f}, // 0 root
	{0.45f, 1.00f, 0.55f, 1.0f}, // 1 top-level widgets
	{1.00f, 0.85f, 0.35f, 1.0f}, // 2
	{1.00f, 0.50f, 0.75f, 1.0f}, // 3
	{0.70f, 0.60f, 1.00f, 1.0f}, // 4
};
constexpr size_t kDepthTintCount = sizeof(kDepthTint) / sizeof(kDepthTint[0]);

Vec4 TintFor(size_t depth, float alpha) {
	Vec4 c = kDepthTint[depth % kDepthTintCount];
	c.w = alpha;
	return c;
}

// Every visible widget, outlined. Deeper boxes draw later so they sit on top of
// their parent's edge where the two coincide.
void DrawOutlines(const Widget& widget, gfx::SpriteBatch& batch, size_t depth) {
	if (!widget.visible) return;
	DrawBorder(batch, widget.Pixel(), TintFor(depth, 0.55f));
	for (const auto& child : widget.Children())
		DrawOutlines(*child, batch, depth + 1);
}

// The chain from the root down to the widget under the cursor. Among the
// children containing the point it descends into the SMALLEST, not the
// last-added — deliberately NOT the input rule (reverse add order), because a
// widget whose bounds overstate what it draws would otherwise swallow every
// readout. MessageLog is exactly that: bounds {0,0,1,1} over the whole window,
// added last, sizing itself down in Draw — hovering a party slot reported the
// log. Smallest-first names the thing under the pointer; the input walk still
// resolves as Widget.h describes, and a widget that claims a pixel it doesn't
// draw on is a layout bug to fix, not something to model here.
//
// A widget with an empty rect (a popup positioning itself in absolute pixels,
// e.g. ContextMenu) can't be hit — it has no bounds to test.
void BuildChain(Widget& widget, float mx, float my,
				std::vector<Widget*>& chain) {
	if (!widget.visible || !widget.Pixel().Contains(mx, my)) return;
	chain.push_back(&widget);
	Widget* best = nullptr;
	float bestArea = 0.0f;
	for (const auto& child : widget.Children()) {
		if (!child->visible || !child->Pixel().Contains(mx, my)) continue;
		const float area = child->Pixel().w * child->Pixel().h;
		if (!best || area <= bestArea) { // ties go to the later-added child
			best = child.get();
			bestArea = area;
		}
	}
	if (best) BuildChain(*best, mx, my, chain);
}

std::string RectText(const gfx::Rect& r) {
	return std::format("{:.0f},{:.0f} {:.0f}x{:.0f}", r.x, r.y, r.w, r.h);
}

// A widget that SET a role is the interesting one — it starts a re-faced
// subtree. Marking the inheritors too would tag almost every line.
std::string RoleText(const Widget& widget) {
	return widget.fontRole
			   ? std::format("  font {}", FontRoleName(*widget.fontRole))
			   : std::string();
}

void DumpNode(const Widget& widget, size_t depth,
			  const std::function<void(const std::string&)>& out) {
	const gfx::Rect& b = widget.bounds;
	out(std::format("{}{}{}  px {}  bounds {:.3f},{:.3f} {:.3f}x{:.3f}{}",
					std::string(depth * 2, ' '), Name(widget),
					widget.visible ? "" : " (hidden)", RectText(widget.Pixel()),
					b.x, b.y, b.w, b.h, RoleText(widget)));
	for (const auto& child : widget.Children())
		DumpNode(*child, depth + 1, out);
}

} // namespace

bool Enabled() { return g_enabled; }
void SetEnabled(bool on) { g_enabled = on; }

std::string Name(const Widget& widget) {
	if (widget.debugName) return widget.debugName;
	std::string name = typeid(widget).name(); // MSVC: "class dungeon::ui::Button"
	if (const size_t pos = name.rfind("::"); pos != std::string::npos)
		return name.substr(pos + 2);
	if (name.rfind("class ", 0) == 0) return name.substr(6);
	return name;
}

void Draw(UIContext& ctx, gfx::SpriteBatch& batch) {
	if (!g_enabled) return;
	Widget& root = ctx.Root();
	DrawOutlines(root, batch, 0);

	// The chain under the cursor, from the pointer position the last Update
	// saw (Render has no Input of its own).
	std::vector<Widget*> chain;
	BuildChain(root, ctx.MouseX(), ctx.MouseY(), chain);
	if (chain.empty()) return;

	// Wash + a solid border over each link, so the nesting reads as boxes
	// inside boxes rather than one highlighted rect.
	for (size_t i = 0; i < chain.size(); ++i) {
		const gfx::Rect& px = chain[i]->Pixel();
		batch.DrawRect(px, TintFor(i, 0.10f));
		DrawBorder(batch, px, TintFor(i, 1.0f));
	}

	// The breadcrumb, beside the pointer and clamped on screen: one indented
	// line per link, deepest last.
	Font& font = ctx.GetFont();
	const float lineH = font.LineAdvance();
	const float pad = 6.0f;
	float widest = 0.0f;
	std::vector<std::string> lines;
	lines.reserve(chain.size());
	for (size_t i = 0; i < chain.size(); ++i) {
		// The chain shows the RESOLVED role, not just where it was set: under the
		// cursor you want to know which face this leaf actually draws in.
		lines.push_back(std::format("{}{}  {}  [{}]", std::string(i * 2, ' '),
									Name(*chain[i]), RectText(chain[i]->Pixel()),
									FontRoleName(chain[i]->ResolvedRole())));
		widest = std::max(widest, font.MeasureWidth(lines.back()));
	}
	gfx::Rect box{ctx.MouseX() + 16.0f, ctx.MouseY() + 16.0f, widest + 2 * pad,
				  lineH * static_cast<float>(lines.size()) + 2 * pad};
	box.x = std::min(box.x, ctx.Width() - box.w - 4.0f);
	box.y = std::min(box.y, ctx.Height() - box.h - 4.0f);
	batch.DrawRect(box, {0.02f, 0.02f, 0.03f, 0.88f});
	DrawBorder(batch, box, TintFor(chain.size() - 1, 1.0f));
	for (size_t i = 0; i < lines.size(); ++i)
		font.Draw(batch, lines[i], box.x + pad,
				  box.y + pad + lineH * static_cast<float>(i), TintFor(i, 1.0f));
}

void Dump(const UIContext& ctx,
		  const std::function<void(const std::string&)>& out) {
	DumpNode(ctx.Root(), 0, out);
}

// --- the overlap audit -------------------------------------------------------

namespace {

std::function<void(const std::string&)> g_auditOut;
int g_auditFrames = 0; // frames left armed
// Findings already reported this run. The window spans more than one frame and
// the same pair collides on each of them; a report that repeated itself would
// read as more broken than the UI is.
std::set<std::string> g_auditSeen;

// Overlap AREA, not just "do they touch": adjacent rows share an edge exactly,
// and a rounding hair of that is not a collision anyone can see. A pair has to
// share more than a hairline in BOTH axes to be worth reporting.
bool Collides(const gfx::Rect& a, const gfx::Rect& b) {
	constexpr float kSlack = 1.5f; // pixels; below this it is a shared edge
	const float x = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x);
	const float y = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
	return x > kSlack && y > kSlack;
}

bool Auditable(const Widget& w) {
	if (!w.visible || w.overlapOk) return false;
	const gfx::Rect& px = w.Pixel();
	return px.w > 0.0f && px.h > 0.0f;
}

void AuditNode(const Widget& parent, const std::string& path,
			   const std::function<void(const std::string&)>& out) {
	const auto& kids = parent.Children();
	for (size_t i = 0; i < kids.size(); ++i) {
		if (!Auditable(*kids[i])) continue;
		for (size_t j = i + 1; j < kids.size(); ++j) {
			if (!Auditable(*kids[j])) continue;
			if (!Collides(kids[i]->InkRect(), kids[j]->InkRect())) continue;
			std::string line =
				std::format("  {} > {} [{}] overlaps {} [{}]", path,
							Name(*kids[i]), RectText(kids[i]->InkRect()),
							Name(*kids[j]), RectText(kids[j]->InkRect()));
			if (g_auditSeen.insert(line).second) out(line);
		}
	}
	for (const auto& child : kids) {
		if (!child->visible) continue;
		AuditNode(*child, path + " > " + Name(*child), out);
	}
}

} // namespace

void ArmOverlapAudit(std::function<void(const std::string&)> out) {
	g_auditOut = std::move(out);
	// Two frames, not one: a context that was rebuilt this frame has not laid
	// its tree out yet, and a tree whose rects are all zero reports nothing.
	g_auditFrames = 2;
	g_auditSeen.clear();
}

void RunOverlapAudit(UIContext& ctx) {
	if (g_auditFrames <= 0 || !g_auditOut) return;
	AuditNode(ctx.Root(), "root", g_auditOut);
}

void EndOverlapAuditFrame() {
	if (g_auditFrames <= 0) return;
	if (--g_auditFrames > 0) return;
	if (g_auditOut)
		g_auditOut(g_auditSeen.empty()
					   ? "uioverlap: clean — every widget kept to its own area"
					   : std::format("uioverlap: {} overlapping pairs",
									 g_auditSeen.size()));
	g_auditOut = nullptr;
	g_auditSeen.clear();
}

} // namespace dungeon::ui::inspect
