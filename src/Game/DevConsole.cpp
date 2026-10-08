// ============================================================================
// Game/DevConsole.cpp - see DevConsole.h.
//
// The frame of the console: commands, input, the scrollback, and the readout
// panel's layout and scrolling. Each SECTION of that panel lives in its own
// file (DevConsole_Perf / _Profile / _Health / _Threads.cpp, plus the
// profile's _Snapshots.cpp, sharing
// DevConsole_Panel.h); Render asks each for its height, fills the panel, then
// has each draw itself in turn.
// ============================================================================
#include "Game/DevConsole_Panel.h"

#include "Core/Log.h"
#include "Core/Profile.h"
#include "Core/Utf8.h"
#include "UI/Controls.h" // ui::DrawBorder

#include <Windows.h> // VK_* codes

#include <algorithm>
#include <cmath>
#include <format>
#include <sstream>

namespace dungeon::game {

using namespace devcon;

namespace {
constexpr float kDesignWindowH = 900.0f; // font authored against this height
constexpr float kFontH = 16.0f;          // console font px at the design height
constexpr size_t kMaxOutput = 500;       // scrollback cap

std::vector<std::string> Tokenize(const std::string& line) {
	std::vector<std::string> tokens;
	std::istringstream stream(line);
	std::string token;
	while (stream >> token) tokens.push_back(token);
	return tokens;
}
} // namespace

// Declared, and explained, in DevConsole_Panel.h: the perf gauges and the
// profile both graph through it.
void devcon::DrawSeriesGraph(gfx::SpriteBatch& batch, const gfx::Rect& plot,
							 const float* samples, int count, int head, float scale,
							 const Vec4& color, bool background, bool fill) {
	if (background) {
		batch.DrawRect(plot, kGaugeBg);
		ui::DrawBorder(batch, plot, kBorder);
	}
	if (count < 2 || scale <= 0.0f) return;

	const float stepX = plot.w / static_cast<float>(count - 1);
	const float base = plot.y + plot.h;
	auto yFor = [&](float v) {
		return base - std::clamp(v / scale, 0.0f, 1.0f) * plot.h;
	};

	// FILLED UNDER THE LINE, and this is not decoration. On a fixed scale a real
	// reading can be a small fraction of its ceiling — VRAM at 1 GB of an 11 GB
	// budget is 9%, which on a plot this tall is a 1.5px line four pixels off the
	// floor and reads as an EMPTY graph. A band cannot be mistaken for nothing.
	if (fill) {
		const Vec4 fillColor{color.x, color.y, color.z, 0.22f};
		for (int k = 0; k < count; ++k) {
			const float top = yFor(samples[(head + k) % count]);
			if (base - top < 0.5f) continue;
			const float fx = plot.x + static_cast<float>(k) * stepX;
			const float fw = std::min(stepX + 1.0f, plot.x + plot.w - fx);
			if (fw > 0.0f) batch.DrawRect({fx, top, fw, base - top}, fillColor);
		}
	}

	float lx = plot.x;
	float ly = yFor(samples[head]);
	for (int k = 1; k < count; ++k) {
		const float nx = plot.x + static_cast<float>(k) * stepX;
		const float ny = yFor(samples[(head + k) % count]);
		const float dx = nx - lx, dy = ny - ly;
		const float len = std::sqrt(dx * dx + dy * dy);
		if (len >= 0.01f)
			batch.DrawRectRotated({(lx + nx) * 0.5f, (ly + ny) * 0.5f}, {len, 1.5f},
								  std::atan2(dy, dx), color);
		lx = nx;
		ly = ny;
	}
}

DevConsole::DevConsole(ui::FontLibrary& fonts, threads::Manager& threadManager)
	: m_fonts(fonts), m_font(&fonts.Get(ui::FontRole::Mono, kFontH)),
	  m_threadMgr(threadManager) {
	// Every OS query in the readout runs on its own worker rather than on the
	// frame — see PerfMonitor::StartOsSampler for what they measured at.
	m_perf.StartOsSampler(threadManager);

	// Generic built-ins. Gameplay-aware commands are registered by the Game.
	RegisterHelp();
	Register({.name = "clear", .group = CmdGroup::Console, .summary = "clear the console output"},
			 [this](const std::vector<std::string>&) {
				 m_output.clear();
				 m_scroll = 0;
			 });
	RegisterProfileCommand();
	Register({.name = "echo",
			  .group = CmdGroup::Console,
			  .params = "<text...>",
			  .summary = "print the arguments back"},
			 [this](const std::vector<std::string>& args) {
				 std::string line;
				 for (size_t i = 0; i < args.size(); ++i)
					 line += (i ? " " : "") + args[i];
				 Print(line);
			 });

	Print("Developer console - type 'help' for commands, or start typing one.");
}

void DevConsole::Toggle() {
	m_open = !m_open;
	if (m_open) {
		m_scroll = 0;
		m_caretBlink = 0.0f;
		m_historyIndex = -1;
	}
}

void DevConsole::Print(std::string line) { PrintLine({.text = std::move(line)}); }

void DevConsole::PrintLine(OutLine line) {
	// Mirrored BEFORE the move, and through the ordinary log so a mirrored run
	// interleaves correctly with everything else the frame wrote — the point
	// is to read ONE file and see the whole story in order.
	if (m_mirrorToLog) log::Info("console: {}", line.text);
	m_output.push_back(std::move(line));
	while (m_output.size() > kMaxOutput) m_output.pop_front();
	m_scroll = 0; // jump to the newest line
}

// Printed like anything else; the flag is what the eval runner reads. Kept next
// to Print rather than inline in the header so the two stay visibly the same
// operation with one extra bit — see the header for what earns a Refuse.
void DevConsole::Refuse(std::string line) {
	m_refused = true;
	Print(std::move(line));
}

bool DevConsole::Execute(const std::string& line) {
	Print("> " + line);
	// Cleared before dispatch, never after: a handler that refuses sets it, and
	// the runner reads it immediately. Clearing afterwards would race the very
	// read it exists for.
	m_refused = false;
	m_gateRefused = false;
	const std::vector<std::string> tokens = Tokenize(line);
	if (tokens.empty()) return true; // a blank line is not a failure

	std::string name = tokens[0];
	std::ranges::transform(name, name.begin(), [](char c) {
		return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	});
	const std::vector<std::string> args(tokens.begin() + 1, tokens.end());

	const Command* cmd = FindCommand(name);
	if (!cmd) {
		Print("unknown command: " + name + " (type 'help' to list them)");
		return false;
	}
	if (gate) {
		if (std::string why = gate(name); !why.empty()) {
			Refuse(std::move(why));
			m_gateRefused = true;
			return true;
		}
	}
	// Restored rather than cleared: a handler may run another line (the eval
	// runner does), and the outer command's usage must survive the inner one.
	const Command* outer = m_running;
	m_running = cmd;
	cmd->fn(args);
	m_running = outer;
	return true;
}

void DevConsole::SampleHistory(float dt, const gfx::GraphicsDevice& device,
								const gfx::SpriteBatch& sprites) {
	SamplePerfSeries(device, sprites);

	{
		DN_PROFILE_ZONE_L(prof::kLevelDetail, "snapshot");
		SampleProfileSeries();
	}

	// A recording in progress takes every frame, not the smoothed value: it is
	// building its own average over its own window and wants the raw samples to
	// do it from.
	SnapAccumulate(dt);

	// Its own cadence, slower than the graph's: the graphs are SHAPES and want
	// the fine sampling, while the digits beside them want to sit still long
	// enough to be read. A window of 0 commits every frame, which is the old
	// unsmoothed behaviour and needs no branch of its own.
	m_profSmoothTimer += dt;
	if (m_profSmoothTimer >= m_profSmoothSec) {
		m_profSmoothTimer = 0.0f;
		CommitProfileSmooth();
	}

	m_profSampleTimer += dt;
	if (m_profSampleTimer < kProfSampleSec) return;
	m_profSampleTimer = 0.0f;

	CommitPerfSeries();
	CommitProfileSeries();
	SampleHealth();

	// One cursor for both sets, advanced once: every graph on screen shares an
	// x-axis, so a spike in one lines up with a spike in another.
	m_profHead = (m_profHead + 1) % kProfHistory;
}

void DevConsole::Update(const Input& input, float dt, float windowW, float windowH,
						const gfx::GraphicsDevice& device, const gfx::SpriteBatch& sprites) {
	{
		DN_PROFILE_ZONE_L(prof::kLevelDetail, "perfmon");
		m_perf.Tick(dt);
	}
	// Every frame, open or closed: switching to a graph view should show the last
	// twelve seconds, not start blank.
	{
		DN_PROFILE_ZONE_L(prof::kLevelDetail, "history");
		SampleHistory(dt, device, sprites);
	}
	if (!m_open) return;

	m_caretBlink += dt;
	// Re-resolve rather than re-bake: same size + same face is a map lookup.
	// GameUI::UpdateFonts commits every library font, this one included.
	m_font = &m_fonts.Get(ui::FontRole::Mono, kFontH * (windowH / kDesignWindowH));

	// Into the space Render laid its rects out in. The scale is 1 whenever the
	// window and the swapchain agree, which is the only case anything here has
	// ever run in — but multiplying by 1 costs nothing and stops the whole panel
	// silently mis-aiming the day they do not.
	const float hitScale = (windowW > 0.0f && m_renderW > 0.0f) ? m_renderW / windowW : 1.0f;
	m_hitScale = hitScale;
	const float mx = input.MouseX() * hitScale, my = input.MouseY() * hitScale;

	// THE PANEL'S CLIP IS AN INPUT CLIP (code-review C379). Drawing is scissored
	// to the panel, but the sections lay their rows out at their natural height,
	// so a row past the panel's foot sits UNSEEN under the scrollback with its
	// rect still recorded. Below m_panelH nothing on the panel is hot: no hover,
	// no click - the scrollback's own space, where a press means nothing here.
	const bool onPanel = my < m_panelH;

	// Hover is tracked every frame, not only on a click: the profile's tooltips
	// are the one thing on this panel that answers to the pointer merely resting.
	// Off the panel, nothing is hovered.
	if (onPanel) {
		ProfileHover(mx, my);
	} else {
		m_frameBarHover = false;
		m_profHeaderHover = false;
	}

	// Every clickable thing on the panel was laid out by last frame's Render, and
	// each section hit-tests its own.
	if (input.WasMousePressed(MouseButton::Left) && !onPanel) ++m_clicksBelowPanel;
	if (input.WasMousePressed(MouseButton::Left) && onPanel) {
		PerfClick(mx, my);
		ProfileClick(mx, my);
		HealthClick(mx, my);
		ThreadsClick(mx, my);
		// The graph checkboxes belong to two sections, like the helper that draws
		// them, so they are answered here.
		for (const GraphToggle& g : m_graphToggles) {
			if (!g.box.Contains(mx, my)) continue;
			if (g.perfLine >= 0) {
				m_perfHidden[g.perfLine] = !m_perfHidden[g.perfLine];
			} else {
				for (int j = 0; j < m_profSeriesCount; ++j) {
					ProfSeries& c = m_profSeries[j];
					if (c.used && c.tid == g.tid && c.node == g.node) {
						c.hidden = !c.hidden;
						break;
					}
				}
			}
			break;
		}
	}

	// The typed text, IN ORDER: characters, Backspace and Enter exactly as they
	// fell, so a frame holding `sheet 1<Enter>sh` runs `sheet 1` and starts the
	// next line with `sh` (Input::TypedChars says why it is one stream). Skip the
	// toggle key so `~`/backtick never self-types. Any edit re-opens the
	// type-ahead list; see m_suggestOpen for why a history recall does not.
	// Indexed afresh each step, not range-for: Enter runs a command, and the
	// view must not be held across whatever that command does. A step is one
	// whole UTF-8 character (C383), so Backspace never leaves half a letter.
	for (size_t i = 0; i < input.TypedChars().size();) {
		const std::string_view ch = utf8::CharAt(input.TypedChars(), i);
		i += ch.size();
		if (ch[0] == Input::kTypedEnter) {
			SubmitLine();
			// A command that shut the console (alloctest, allocpoke) takes the
			// rest of the frame's typing with it - there is nothing open to type
			// into, the same as typing while it is closed.
			if (!m_open) return;
			continue;
		}
		if (ch[0] == Input::kTypedBack) {
			if (!utf8::PopBack(m_input)) continue;
		} else {
			if (ch == "`" || ch == "~") continue;
			m_input.append(ch);
		}
		m_suggestOpen = true;
		m_suggestSel = 0;
		m_historyIndex = -1;
	}
	RefreshSuggestions();

	// The type-ahead takes Tab, Up/Down and Esc while its list shows (and Enter
	// on a half-typed name, in SubmitLine). What it does not claim falls through
	// to the ordinary keys below - so history and Esc sit behind this one flag.
	const bool suggestConsumed = UpdateSuggest(input);

	// Command history recall. A recalled line leaves the type-ahead list shut
	// (see m_suggestOpen), so the next Up steps further back.
	if (!suggestConsumed && input.WasKeyPressed(VK_UP) && !m_history.empty()) {
		if (m_historyIndex == -1)
			m_historyIndex = static_cast<int>(m_history.size()) - 1;
		else if (m_historyIndex > 0)
			--m_historyIndex;
		m_input = m_history[static_cast<size_t>(m_historyIndex)];
		m_suggestOpen = false;
	}
	if (!suggestConsumed && input.WasKeyPressed(VK_DOWN) && m_historyIndex != -1) {
		if (m_historyIndex < static_cast<int>(m_history.size()) - 1) {
			++m_historyIndex;
			m_input = m_history[static_cast<size_t>(m_historyIndex)];
		} else {
			m_historyIndex = -1;
			m_input.clear();
		}
		m_suggestOpen = false;
	}

	// The wheel goes to whatever is UNDER it: the readout panel while the cursor
	// is over the panel, the scrollback otherwise. Same rule the widget tree
	// states for ConsumeWheel — whoever can act on it claims it — arrived at here
	// by hand because the console is not part of that tree.
	const float wheel = input.WheelDelta();
	if (wheel != 0.0f && onPanel) {
		// Clamped by Render, which is the only place the content height is known.
		m_panelScroll -= wheel * m_lineH * 3.0f;
	} else {
		// Scroll the output (wheel, or PageUp/PageDown).
		int scrollLines = static_cast<int>(wheel);
		if (input.WasKeyPressed(VK_PRIOR)) scrollLines += 5;
		if (input.WasKeyPressed(VK_NEXT)) scrollLines -= 5;
		if (scrollLines != 0) {
			m_scroll = std::clamp(m_scroll + scrollLines, 0,
								  static_cast<int>(m_output.size()));
			m_caretBlink = 0.0f;
		}
	}

	// Esc shuts an open type-ahead list first (UpdateSuggest claims it), and only
	// a second Esc closes the console - the popup-first rule GameUI follows.
	if (!suggestConsumed && input.WasKeyPressed(VK_ESCAPE)) m_open = false;

	// Again, after Enter / a recall / a completion changed the line, so Render
	// never draws suggestions for the line before this frame's keys.
	RefreshSuggestions();
}

void DevConsole::SubmitLine() {
	if (SuggestTakesEnter()) return;
	if (!m_input.empty()) {
		m_history.push_back(m_input);
		// Gated while a staged load runs (see SetCommandsEnabled): the
		// world is partially built, so no handler may touch it. The line
		// stays in history - recall it with Up once the load finishes.
		// EXCEPT `quit` / `exit`, which touch no world: in Borderless or
		// Exclusive there is no close box, and a load that has wedged is
		// exactly when a way out is wanted (code-review C392).
		const size_t from = m_input.find_first_not_of(' ');
		const std::string_view word = from == std::string::npos
			? std::string_view()
			: std::string_view(m_input).substr(from, m_input.find(' ', from) - from);
		if (m_commandsEnabled || word == "quit" || word == "exit") Execute(m_input);
		else Print("commands are unavailable while loading");
		m_input.clear();
	}
	m_historyIndex = -1;
	m_suggestOpen = false;
}

// The collapse control every section header carries. Its face names what a
// click DOES, not the current state, so "hide" is the button that hides.
gfx::Rect DevConsole::DrawExpander(const PanelCtx& p, float y, bool expanded) {
	const char* face = expanded ? " hide " : " show ";
	const float bw = m_font->MeasureWidth(face);
	const gfx::Rect r{p.width - p.pad * 2.0f - bw, y, bw, p.line};
	p.batch.DrawRect(r, kGaugeBg);
	ui::DrawBorder(p.batch, r, kBorder);
	m_font->Draw(p.batch, face, r.x, y, kAccent);
	return r;
}

// Text checkboxes, because this is a monospaced dev console and "[x]" reads
// better here than a drawn box would. Registers the hit rect only if it is
// actually ON the panel: input is clipped the same way drawing is, so a
// checkbox scrolled out of view cannot be clicked through the scrollback.
float DevConsole::DrawCheckbox(const PanelCtx& p, float x, float y, bool on, int perfLine,
							   u32 tid, u32 node) {
	const char* face = on ? "[x] " : "[ ] ";
	m_font->Draw(p.batch, face, x, y, on ? kAccent : kDim);
	const gfx::Rect box{x, y, m_font->MeasureWidth(face), p.line};
	if (y + p.line > 0.0f && y < p.panelH) m_graphToggles.push_back({box, perfLine, tid, node});
	return box.w;
}

void DevConsole::Render(gfx::SpriteBatch& batch, const gfx::GraphicsDevice& device,
						float width, float height) {
	const float line = m_font->LineAdvance();
	const float pad = line * 0.5f;
	const float labelX = pad * 2.0f;

	// The canvas the rects below are recorded in, for Update's hit scale. It was
	// declared and never written, so the scale was always 1.
	m_renderW = width;
	m_renderH = height;

	// Full-screen dim background.
	batch.DrawRect({0, 0, width, height}, kBackground);

	// Graph geometry is shared by the two sections that graph, so it is part of
	// the layout every section is handed rather than a constant of either.
	PanelCtx p{.batch = batch,
			   .device = device,
			   .width = width,
			   .line = line,
			   .pad = pad,
			   .labelX = labelX,
			   .rowAdvance = line * 1.2f,
			   .graphH = line * 3.4f,
			   .graphGapY = line * 0.4f};

	const std::vector<threads::WorkerInfo> workers = m_threadMgr.SnapshotAll();
	// Flattened first: the panel's background is filled before anything is drawn
	// into it, so its height has to be known up front. In a build without
	// DN_PROFILE this holds 0 rows and the section collapses to one line saying
	// which configs have it.
	ProfileFrame prof;
	PrepareProfile(prof);

	// Top to bottom. PROFILE sits directly under the gauges; HEALTH only exists
	// once something has gone wrong; THREADS goes to the BOTTOM of the panel and
	// starts COLLAPSED. It is a control surface - halt, rate, kill, boot - rather
	// than something you watch, so it should not push the readout you are
	// actually reading down the screen to make room for buttons.
	const float perfTop = pad;
	const float profileTop = perfTop + PerfSectionHeight(p);
	const float healthTop = profileTop + ProfileSectionHeight(p, prof);
	const float threadsTop = healthTop + HealthSectionHeight(p);
	const float contentEnd = threadsTop + ThreadsSectionHeight(p, workers);

	// CONTENT height (everything, laid out) against the VISIBLE height (what the
	// window can spare above the scrollback and the prompt). The panel is the
	// smaller; the difference is what there is to scroll through.
	const float contentH = contentEnd + pad;
	const float panelH = std::min(contentH, height - line * 6.0f);
	m_panelScroll = std::clamp(m_panelScroll, 0.0f, std::max(0.0f, contentH - panelH));
	m_panelH = panelH;
	m_contentH = contentH;
	m_lineH = line;
	p.panelH = panelH;
	p.sy = -m_panelScroll; // added to every content-space y a section draws at
	batch.DrawRect({0, 0, width, panelH}, kPerfBg);
	ui::DrawBorder(batch, {0, 0, width, panelH}, kBorder);

	// Everything from here to the matching reset is CONTENT, drawn at
	// content-space y plus `sy` and clipped to the panel. Without the scissor a
	// scrolled-up row would draw over the border and out across the scrollback.
	const gfx::Rect panelClip{0, 0, width, panelH};
	batch.SetScissor(&panelClip);

	m_graphToggles.clear(); // filled by DrawCheckbox, from two of the sections
	DrawPerfSection(p, perfTop);
	DrawProfileSection(p, profileTop, prof);
	DrawHealthSection(p, healthTop);
	DrawThreadsSection(p, threadsTop, workers);

	batch.SetScissor(nullptr);

	// A thumb on the right edge, only when there is something to scroll to. The
	// panel has no visible frame of its own beyond the border, so without this
	// there is nothing to say the readout continues past the bottom.
	if (contentH > panelH) {
		const float tw = line * 0.35f;
		const float frac = panelH / contentH;
		const float th = std::max(panelH * frac, line);
		const float ty2 = (panelH - th) * (m_panelScroll / (contentH - panelH));
		batch.DrawRect({width - tw, 0, tw, panelH}, kGaugeBg);
		batch.DrawRect({width - tw, ty2, tw, th}, kBorder);
	}

	// --- output log + input line (bottom) -----------------------------------
	const float inputY = height - line - pad;
	const float promptW = m_font->MeasureWidth("> ");
	m_font->Draw(batch, "> ", labelX, inputY, kAccent);
	m_font->Draw(batch, m_input, labelX + promptW, inputY, kText);
	if (std::fmod(m_caretBlink, 1.0f) < 0.5f) {
		const float caretX = labelX + promptW + m_font->MeasureWidth(m_input);
		batch.DrawRect({caretX + 1.0f, inputY, 2.0f, line}, kText);
	}
	batch.DrawRect({0, inputY - pad * 0.5f, width, 1.0f}, kBorder);

	// Scrollback, newest at the bottom just above the input line.
	const float logTop = panelH + pad;
	const float logBottom = inputY - pad;
	int visible = static_cast<int>((logBottom - logTop) / line);
	if (visible < 0) visible = 0;
	const int total = static_cast<int>(m_output.size());
	const int end = std::max(0, total - m_scroll); // index past the last shown
	const int start = std::max(0, end - visible);
	float ly = logBottom - line;
	for (int i = end - 1; i >= start; --i) {
		const OutLine& o = m_output[static_cast<size_t>(i)];
		if (o.style == LineStyle::Header) {
			m_font->Draw(batch, o.text, labelX, ly, kAccent);
		} else if (o.style == LineStyle::Row) {
			// Mono, so each column's x is its byte offset's width - the padding
			// spaces are in the text and the three draws land on the same grid.
			const std::string_view t = o.text;
			const size_t a = std::min<size_t>(o.nameEnd, t.size());
			const size_t b = std::clamp<size_t>(o.paramsEnd, a, t.size());
			m_font->Draw(batch, t.substr(0, a), labelX, ly, kAccent);
			m_font->Draw(batch, t.substr(a, b - a), labelX + m_font->MeasureWidth(t.substr(0, a)),
						 ly, kText);
			m_font->Draw(batch, t.substr(b), labelX + m_font->MeasureWidth(t.substr(0, b)), ly,
						 kDim);
		} else {
			m_font->Draw(batch, o.text, labelX, ly, kText);
		}
		ly -= line;
	}

	// Last, so the type-ahead box sits OVER the scrollback it shares room with.
	DrawSuggest(batch, width, inputY, line, pad, labelX);
}

} // namespace dungeon::game
