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

#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace dungeon::game {

class DevConsole {
public:
	DevConsole(gfx::GraphicsDevice& device, threads::Manager& threadManager);

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

private:
	void Execute(const std::string& line);

	ui::Font m_font;
	PerfMonitor m_perf;
	threads::Manager& m_threadMgr;

	// Per-thread control-button rects, rebuilt by Render each frame and hit-tested
	// by the next Update (the panel is static, so one frame of lag is invisible).
	// This keeps the button layout in one place — Render — instead of duplicated.
	struct ThreadHit {
		threads::WorkerId id;
		gfx::Rect pause, slower, faster, kill;
	};
	std::vector<ThreadHit> m_threadHits;

	bool m_open = false;
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
