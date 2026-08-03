// ============================================================================
// Game/DevConsole.h — the Quake-style developer console (toggle with `~`).
//
// A fullscreen overlay drawn on top of everything else: the upper region is a
// live performance panel (FPS, CPU/GPU utilization, system/GPU/process memory,
// adapter name — Task-Manager style), the lower region is a scrollback log and
// a command input line. It is dev-facing, so all text stays English (no Loc).
//
// The console does NOT pause the game — while open the world keeps simulating;
// the Game just routes input here (so the party doesn't move while you type)
// and freezes nothing. Esc closes it. Commands come from a small registry:
// the console seeds the generic ones (help/clear/echo) and the Game registers
// the gameplay-aware ones (quit/fps/quality/lang/tp).
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Core/ThreadManager.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "Platform/PerfMonitor.h"
#include "UI/Font.h"
#include "UI/FontLibrary.h"

#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace dungeon::game {

class DevConsole {
public:
	DevConsole(gfx::GraphicsDevice& device, ui::FontLibrary& fonts,
			   threads::Manager& threadManager);

	bool IsOpen() const { return m_open; }
	void Toggle();

	// Latest smoothed frame rate (sampled every frame, even when closed).
	float Fps() const { return m_perf.Get().fps; }

	// Called every frame (the FPS sampler keeps ticking even when closed).
	// While open, consumes typed characters and editing/history/scroll keys.
	void Update(const Input& input, float dt, float windowW, float windowH);
	// Drawn inside the caller's SpriteBatch Begin/End, after the HUD/overlays.
	void Render(gfx::SpriteBatch& batch, const gfx::GraphicsDevice& device,
				float width, float height);

	// Command registry. `fn` receives the whitespace-split arguments (without
	// the command name). Print appends a line to the scrollback.
	void Register(std::string name, std::string help,
				  std::function<void(const std::vector<std::string>&)> fn);
	void Print(std::string line);

	// Gates command EXECUTION (typing/scrollback stay live). The Game disables
	// commands while a staged load is mid-flight — the world is only partially
	// built then, and a handler that reaches into it (cast/save/quality/...)
	// would touch objects a later load task creates. A gated Enter prints a
	// notice and keeps the line in history for an easy re-run after the load.
	void SetCommandsEnabled(bool enabled) { m_commandsEnabled = enabled; }

private:
	void Execute(const std::string& line);

	ui::FontLibrary& m_fonts;
	// Borrowed from the library (Mono: this is a column-aligned readout).
	// Re-pointed every Update, so a face swap lands next frame.
	const ui::Font* m_font = nullptr;
	PerfMonitor m_perf;
	threads::Manager& m_threadMgr;

	// Per-thread control-button rects, rebuilt by Render each frame and hit-tested
	// by the next Update (the panel is static, so one frame of lag is invisible).
	// This keeps the button layout in one place — Render — instead of duplicated.
	struct ThreadHit {
		threads::WorkerId id;
		gfx::Rect pause, slower, faster, kill; // live-worker controls
		gfx::Rect boot;                        // dead-worker reboot (others empty)
	};
	std::vector<ThreadHit> m_threadHits;

	bool m_open = false;
	bool m_commandsEnabled = true;   // false while a staged load is mid-flight
	std::string m_input;             // current edit line
	std::deque<std::string> m_output; // scrollback (oldest front)
	std::vector<std::string> m_history;
	int m_historyIndex = -1; // -1 = editing a fresh line
	int m_scroll = 0;        // lines scrolled up from the bottom
	float m_caretBlink = 0.0f;

	struct Command {
		std::string name;
		std::string help;
		std::function<void(const std::vector<std::string>&)> fn;
	};
	std::vector<Command> m_commands;
};

} // namespace dungeon::game
