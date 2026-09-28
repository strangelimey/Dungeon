// ============================================================================
// Game/DevConsole_Panel.h - what the developer console's SECTIONS share.
//
// The console is one class drawn by five files. DevConsole.cpp owns the frame:
// commands, input, the scrollback, and the readout panel's layout and scroll.
// Each section of that panel owns its own sampling, height, drawing and clicks
// in its own file - DevConsole_Perf.cpp, _Profile.cpp, _Health.cpp and
// _Threads.cpp. This header is the seam between them and is included by those
// five files only: the palette, the one graph routine two sections draw with,
// the per-frame layout every section is handed, and the profile tree's rows
// (which the frame holds between measuring that section and drawing it).
// ============================================================================
#pragma once

#include "Game/DevConsole.h"

namespace dungeon::game {

namespace devcon {

// Console palette (dev-facing, not themed).
constexpr Vec4 kBackground{0.03f, 0.03f, 0.05f, 0.92f};
constexpr Vec4 kPerfBg{0.06f, 0.06f, 0.09f, 1.0f};
constexpr Vec4 kBorder{0.35f, 0.38f, 0.48f, 1.0f};
constexpr Vec4 kText{0.85f, 0.88f, 0.92f, 1.0f};
constexpr Vec4 kDim{0.55f, 0.58f, 0.66f, 1.0f};
constexpr Vec4 kAccent{0.55f, 0.85f, 0.55f, 1.0f};
constexpr Vec4 kWarn{0.95f, 0.65f, 0.35f, 1.0f}; // a readout near a hard ceiling
constexpr Vec4 kGaugeBg{0.13f, 0.13f, 0.17f, 1.0f};

// --- the profile panel's rows -----------------------------------------------
// Flattened ahead of drawing because the panel's HEIGHT has to be known before
// its background is filled, and because it bounds the work: a tree is walked
// once into a fixed array rather than twice against a live snapshot.
//
// Names are COPIED. A zone's name is a string literal and would survive, but a
// thread's lives in the ThreadReport, which is a local of the builder — pointing
// at it would dangle the moment the array outlived the snapshot. The node DATA
// is fine to reach through: it lives in registry storage until that thread's
// next publish.
// Sized for a tree with detail RAISED, not for the default one. Turning a branch
// up is the whole point of the tree's click-to-expand control, and it is exactly
// what multiplies the row count — at 44 (what fitted on screen before the panel
// scrolled) two raised subtrees ran the array out and the rest of the tree
// silently stopped existing. Overflow is reported now rather than trusted not to
// happen; this is a stack array in two frames, ~26 KB each.
constexpr int kMaxProfRows = 256;
// Matches the series pool: every measure that HAS history can be graphed, since
// the panel scrolls now and no longer has to fit them all on screen at once.
// What keeps the cost down is culling the ones scrolled out of view, not a cap.
constexpr int kMaxGraphs = 32;

struct ProfRow {
	char name[32] = {};
	u32 tid = 0;  // owning thread + node index: the stable key the graph view
	u32 node = 0; // uses to follow one measure across frames as the tree grows
	u32 slot = 0; // registry slot: the key that addresses ONE source (see below)
	int depth = 0;
	// The two halves of this node's detail state. `detail` is its own override
	// (-1 = inherit) and `inherited` what its ancestors grant it; together they
	// say what the row's control should show and what a click on it should do.
	// Kept apart rather than pre-combined because "raised here" and "raised by a
	// parent" are different things to look at and only the first is clearable.
	i8 detail = -1;
	i8 inherited = 0;
	double inclMs = 0.0;
	double exclMs = 0.0;
	double maxMs = 0.0;
	u64 calls = 0;
	float frac = 0.0f;  // of this thread's whole period, for the bar
	bool header = false; // a thread's title row rather than a zone
	u64 periods = 0;
	bool dropped = false; // that thread has lost scopes to a full pool
};

// One frame's profile tree, flattened. The panel's height has to be known
// before its background is filled, so the tree is walked ONCE, up front, and
// the same rows are then measured for height and drawn. The counts ride along
// because both of those need them and neither should have to recount.
struct ProfileFrame {
	ProfRow rows[kMaxProfRows];
	int count = 0;   // rows filled
	int total = 0;   // rows the tree holds - more than `count` once it outgrows the array
	int zones = 0;   // of `count`, the zone rows (the rest are thread headers)
	int visible = 0; // zones whose graph is showing
	int hidden = 0;  // zones collapsed to a one-line row in the graph view
};

// Draws one scrolling line graph: newest sample at the RIGHT, oldest at the left,
// with `head` naming the next write slot (and therefore the oldest value).
//
// `scale` is the value at the top of the plot, passed in rather than derived,
// because the two callers want opposite things. A profile timing has no natural
// ceiling, so it autoscales to its own window. A percentage or a memory total
// DOES have one, and autoscaling those would redraw 3% CPU as a full graph and
// make an idle machine look pegged.
//
// SpriteBatch has no line primitive, so a segment is a thin rect rotated onto the
// vector between two points (DrawRectRotated was already there for the map's
// facing arrows).
//
// `background` and `fill` exist so several series can share one plot. An overlay
// draws the frame first, filled, as the envelope everything else sits inside,
// then the others as bare lines over it — three translucent bands stacked on one
// another turn to mud and stop reading as anything.
void DrawSeriesGraph(gfx::SpriteBatch& batch, const gfx::Rect& plot, const float* samples,
					 int count, int head, float scale, const Vec4& color,
					 bool background = true, bool fill = true);

} // namespace devcon

// The layout every section is drawn against, worked out once a frame by Render.
// `panelH` and `sy` are only known once every section has reported its height,
// so a section's Height function must not read them.
struct DevConsole::PanelCtx {
	gfx::SpriteBatch& batch;
	const gfx::GraphicsDevice& device;
	float width = 0.0f;
	float line = 0.0f;       // the font's line advance
	float pad = 0.0f;        // half a line
	float labelX = 0.0f;     // the left margin every section's text starts at
	float rowAdvance = 0.0f; // a list row: a line plus a little air
	float graphH = 0.0f;     // one graph cell, its title line included
	float graphGapY = 0.0f;
	float panelH = 0.0f; // the visible height, for culling what is scrolled away
	float sy = 0.0f;     // added to every content-space y: the scroll offset
};

} // namespace dungeon::game
