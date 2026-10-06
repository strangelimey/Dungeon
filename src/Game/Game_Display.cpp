// ============================================================================
// Game/Game_Display.cpp - the display lifecycle (see Game.h).
//
// A display choice applied to the window and the swapchain - at boot from the
// settings, and by the Settings page's Video Apply - the relaunch an adapter
// change needs (RestartApp), and the dev console's `video`, which reads what
// the Video tab has STAGED, what is RUNNING and what is SAVED, and drives the
// tab's own Apply and the relaunch. Until `video` (code-review batch 68) no
// check reached either path: both were a click on a page no harness opened.
// ============================================================================
#include "Game/Game.h"

#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/DevCommandArgs.h"
#include "Platform/Process.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon::game {

namespace {

const char* ModeName(gfx::FullscreenMode mode) {
	switch (mode) {
	case gfx::FullscreenMode::Windowed: return "windowed";
	case gfx::FullscreenMode::Borderless: return "borderless";
	case gfx::FullscreenMode::Exclusive: return "exclusive";
	}
	return "?";
}

bool ParseMode(std::string_view word, gfx::FullscreenMode& out) {
	for (const gfx::FullscreenMode m : {gfx::FullscreenMode::Windowed, gfx::FullscreenMode::Borderless,
										gfx::FullscreenMode::Exclusive})
		if (word == ModeName(m)) {
			out = m;
			return true;
		}
	return false;
}

// "<w>x<h>", both above zero.
bool ParseSize(std::string_view word, gfx::DisplayMode& out) {
	const size_t x = word.find('x');
	if (x == std::string_view::npos) return false;
	u32 w = 0, h = 0;
	const std::string_view ws = word.substr(0, x), hs = word.substr(x + 1);
	const auto rw = std::from_chars(ws.data(), ws.data() + ws.size(), w);
	const auto rh = std::from_chars(hs.data(), hs.data() + hs.size(), h);
	if (rw.ec != std::errc{} || rw.ptr != ws.data() + ws.size() || rh.ec != std::errc{} ||
		rh.ptr != hs.data() + hs.size() || w == 0 || h == 0)
		return false;
	out = {w, h};
	return true;
}

// x,y,w,h - the readout's one spelling of a rectangle.
std::string RectText(int x, int y, int w, int h) { return std::format("{},{},{},{}", x, y, w, h); }
std::string RectText(const ScreenRect& r) { return RectText(r.x, r.y, r.width, r.height); }

// The command line a relaunch runs (code-review C398). THIS exe, by the path it
// is running from - the old one assumed "Dungeon.exe" beside it - and the
// arguments this run was given, each quoted back the way it was read, so
// `-project` keeps its world and `-headless` / `-unattended` / `-warp` their
// run. LEFT OUT: an `-eval` and its scripts (Main's rule - every argument after
// it up to the next flag), which the child would run again, so a script that
// ends in a restart would restart for ever; and an earlier `-relaunched <pid>`,
// replaced by this run's own, which makes the child wait for it to exit. A
// child that keeps `-headless` and so loses its script has nothing to drive it:
// Main makes such a run quit once its boot load lands (Game::QuitOnceLoaded),
// so a relaunch from a headless script run ends instead of idling unseen.
std::string RelaunchCommandLine() {
	std::string cmd = platform::QuoteArgument(paths::ExecutablePath());
	const std::vector<std::string> args = platform::CommandLineArguments();
	for (size_t i = 0; i < args.size(); ++i) {
		if (args[i] == "-eval") {
			while (i + 1 < args.size() && !args[i + 1].starts_with('-')) ++i;
			continue;
		}
		if (args[i] == "-relaunched") {
			if (i + 1 < args.size()) ++i;
			continue;
		}
		cmd += ' ';
		cmd += platform::QuoteArgument(args[i]);
	}
	cmd += std::format(" -relaunched {}", platform::CurrentProcessId());
	return cmd;
}

} // namespace

// ============================================================================
// Applying a display choice
// ============================================================================

void Game::ApplyDisplaySettings() {
	DisplayChoice choice;
	choice.adapterLuid = m_settings.adapterLuid;
	choice.output = m_settings.displayOutput;
	choice.width = static_cast<u32>(std::max(0, m_settings.displayWidth));
	choice.height = static_cast<u32>(std::max(0, m_settings.displayHeight));
	choice.mode = m_settings.fullscreen;
	ApplyDisplay(choice);
}

void Game::ApplyDisplay(const DisplayChoice& choice) {
	// Resolve the active adapter's outputs so we can position a borderless window
	// or target a monitor for exclusive full-screen.
	const std::vector<gfx::AdapterInfo> adapters = gfx::EnumerateAdapters();
	const gfx::AdapterInfo* active = nullptr;
	for (const gfx::AdapterInfo& a : adapters)
		if (a.luid == m_device.AdapterLuid()) {
			active = &a;
			break;
		}
	const int out = choice.output;
	const gfx::OutputInfo* output =
		(active && out >= 0 && out < static_cast<int>(active->outputs.size()))
			? &active->outputs[static_cast<size_t>(out)]
			: nullptr;

	switch (choice.mode) {
	case gfx::FullscreenMode::Windowed: {
		const u32 w = choice.width > 0 ? choice.width : m_window.Width();
		const u32 h = choice.height > 0 ? choice.height : m_window.Height();
		m_device.SetFullscreen(false, 0, 0, 0); // drop any exclusive state first
		// Centred in the CHOSEN monitor's work area and shrunk to fit it (code-
		// review C196: it centred on the primary whatever Monitor said). With no
		// output to name - a GPU with none - the window's own monitor.
		ScreenRect work;
		if (output) work = {output->workX, output->workY, output->workWidth, output->workHeight};
		m_window.SetWindowed(w, h, output ? &work : nullptr);
		break;
	}
	case gfx::FullscreenMode::Borderless: {
		m_device.SetFullscreen(false, 0, 0, 0);
		if (output)
			m_window.SetBorderless(output->x, output->y,
								   static_cast<u32>(output->width),
								   static_cast<u32>(output->height));
		break;
	}
	case gfx::FullscreenMode::Exclusive: {
		// A HIDDEN window (`-headless`) never holds a monitor: Exclusive goes
		// through the swapchain, which would switch a display nobody is looking
		// at (code-review C391; the boot already skips a saved mode for it).
		if (m_window.IsHidden()) {
			log::Info("display: Exclusive is not taken by a hidden window");
			break;
		}
		u32 w = choice.width;
		u32 h = choice.height;
		if ((w == 0 || h == 0) && output) { // default to the monitor's native size
			w = static_cast<u32>(output->width);
			h = static_cast<u32>(output->height);
		}
		m_device.SetFullscreen(true, static_cast<u32>(out > 0 ? out : 0), w, h);
		break;
	}
	}
}

// ============================================================================
// The relaunch
// ============================================================================

bool Game::RestartApp() {
	const std::string cmd = RelaunchCommandLine();
	// Started FIRST, and this run quits only when it did: a relaunch that failed
	// used to quit anyway, leaving nothing running at all.
	if (!m_restart.Start(cmd)) {
		log::Error("could not relaunch the game ({}) - this run goes on as it was; a choice "
				   "saved for the relaunch takes effect at the next launch",
				   cmd);
		return false;
	}
	log::Info("relaunching: pid {} started as {} - it waits for this run to exit", m_restart.Id(),
			  cmd);
	// Leave any exclusive full-screen so the new process can claim the display -
	// after the start, so a failed one left the display as it was. The child does
	// nothing until this process has gone (Main's `-relaunched` wait).
	m_device.SetFullscreen(false, 0, 0, 0);
	m_quitRequested = true;
	return true;
}

// ============================================================================
// `video` - the dev console's way into the Video tab
// ============================================================================

void Game::PrintVideoStatus() {
	const std::vector<gfx::AdapterInfo>& adapters = m_ui.VideoAdapters();
	const auto adapterAt = [&](int i) -> const gfx::AdapterInfo* {
		return i >= 0 && i < static_cast<int>(adapters.size()) ? &adapters[static_cast<size_t>(i)]
																: nullptr;
	};
	const auto outputAt = [](const gfx::AdapterInfo* a, int o) -> const gfx::OutputInfo* {
		return a && o >= 0 && o < static_cast<int>(a->outputs.size())
				   ? &a->outputs[static_cast<size_t>(o)]
				   : nullptr;
	};

	// STAGED: what the Video tab's Apply would commit. `work` is the chosen
	// monitor's work area - where a Windowed window must land.
	const DisplayChoice staged = m_ui.StagedVideo();
	const gfx::AdapterInfo* stagedA = adapterAt(m_ui.StagedAdapterIndex());
	const gfx::OutputInfo* stagedO = outputAt(stagedA, staged.output);
	m_console.Print(std::format(
		"video staged: adapter={} monitor={} mode={} size={}x{} work={} adaptername={}",
		stagedA ? m_ui.StagedAdapterIndex() : -1, staged.output, ModeName(staged.mode),
		staged.width, staged.height,
		stagedO ? RectText(stagedO->workX, stagedO->workY, stagedO->workWidth, stagedO->workHeight)
				: std::string("-"),
		stagedA ? stagedA->name : std::string("-")));

	// RUNNING: read off the device and the window, never the settings - a
	// script's apply is not saved, and a size the work area could not hold was
	// shrunk. -1 = not in the lists (WARP; a window on a monitor of another GPU).
	int runA = -1;
	for (size_t i = 0; i < adapters.size(); ++i)
		if (adapters[i].luid == m_device.AdapterLuid()) runA = static_cast<int>(i);
	int runO = -1;
	if (const gfx::AdapterInfo* a = adapterAt(runA)) {
		const void* monitor = m_window.Monitor();
		for (size_t o = 0; o < a->outputs.size(); ++o)
			if (a->outputs[o].monitor == monitor) runO = static_cast<int>(o);
	}
	const gfx::FullscreenMode runMode = m_device.IsExclusive() ? gfx::FullscreenMode::Exclusive
										: m_window.IsBorderless() ? gfx::FullscreenMode::Borderless
																  : gfx::FullscreenMode::Windowed;
	m_console.Print(std::format(
		"video running: adapter={} monitor={} mode={} size={}x{} window={} hidden={} adaptername={}",
		runA, runO, ModeName(runMode), m_window.Width(), m_window.Height(),
		RectText(m_window.FrameRect()), m_window.IsHidden() ? 1 : 0, m_device.AdapterName()));

	// SAVED: settings.ini's display fields (adapter 0 = auto).
	m_console.Print(std::format(
		"video saved: adapter={} monitor={} mode={} size={}x{}",
		m_settings.adapterLuid ? std::format("{:016x}", m_settings.adapterLuid) : std::string("auto"),
		m_settings.displayOutput, ModeName(m_settings.fullscreen), m_settings.displayWidth,
		m_settings.displayHeight));

	// The staged adapter's monitors, which `video apply ... monitor <n>` picks from.
	if (stagedA)
		for (size_t o = 0; o < stagedA->outputs.size(); ++o) {
			const gfx::OutputInfo& out = stagedA->outputs[o];
			m_console.Print(std::format("video monitor {}: desktop={} work={} name={}", o,
										RectText(out.x, out.y, out.width, out.height),
										RectText(out.workX, out.workY, out.workWidth, out.workHeight),
										out.name));
		}
}

void Game::RegisterDisplayCommands() {
	m_console.Register(
		{.name = "video",
		 .group = CmdGroup::Settings,
		 .params = "status\n"
				   "apply [windowed|borderless|exclusive] [<w>x<h>|native] [monitor <n>|last]\n"
				   "restage\n"
				   "restart",
		 .summary = "the Video tab's display choice: show it, stage and apply it (a script's is "
					"not saved), re-stage it as opening Settings does, or relaunch"},
		[this](const std::vector<std::string>& args) {
			const std::string verb = args.empty() ? "status" : args[0];
			if (verb == "status" && args.size() <= 1) {
				PrintVideoStatus();
				return;
			}
			if (verb == "restage" && args.size() == 1) {
				// Exactly what opening the Settings page does to it (both menus'
				// entry call this one function), so the re-seed that makes a fresh
				// edit stage the window as it is now can be checked by a script.
				m_ui.RefreshSettingsPage();
				const DisplayChoice s = m_ui.StagedVideo();
				m_console.Print(std::format("video: re-staged as opening Settings does - {} {}x{} "
											"on monitor {}",
											ModeName(s.mode), s.width, s.height, s.output));
				return;
			}
			if (verb == "restart" && args.size() == 1) {
				if (!RestartApp()) {
					m_console.Refuse("the relaunch did not start (see the log) - this run goes on");
					return;
				}
				m_console.Print(std::format("video: relaunching as pid {} - this run quits",
											m_restart.Id()));
				return;
			}
			if (verb != "apply") {
				m_console.RefuseUsage();
				return;
			}

			// Stage what is named - each word as the tab's own control would set
			// it - then press its Apply.
			std::optional<gfx::FullscreenMode> mode;
			std::optional<gfx::DisplayMode> size;
			std::optional<int> monitor;
			for (size_t i = 1; i < args.size(); ++i) {
				const std::string& a = args[i];
				gfx::FullscreenMode m{};
				gfx::DisplayMode s{};
				if (ParseMode(a, m)) {
					mode = m;
				} else if (a == "native") {
					size = gfx::DisplayMode{0, 0}; // the staged monitor's own
				} else if (ParseSize(a, s)) {
					size = s;
				} else if (a == "monitor" && i + 1 < args.size()) {
					const std::string& n = args[++i];
					if (n == "last") {
						const std::vector<gfx::AdapterInfo>& list = m_ui.VideoAdapters();
						const int sel = m_ui.StagedAdapterIndex();
						const int count = sel >= 0 && sel < static_cast<int>(list.size())
											  ? static_cast<int>(list[static_cast<size_t>(sel)].outputs.size())
											  : 0;
						monitor = count - 1; // -1 when there is none: StageVideo refuses it
					} else {
						int v = -1;
						const auto r = std::from_chars(n.data(), n.data() + n.size(), v);
						if (r.ec != std::errc{} || r.ptr != n.data() + n.size()) {
							m_console.Refuse(std::format("monitor must be a number or `last`, not '{}'", n));
							return;
						}
						monitor = v;
					}
				} else {
					m_console.RefuseUsage();
					return;
				}
			}
			if (m_ui.VideoAdapters().empty()) {
				m_console.Refuse("no display adapter was enumerated - the Video tab has nothing to apply");
				return;
			}
			const gfx::FullscreenMode effective = mode ? *mode : m_ui.StagedVideo().mode;
			if (effective == gfx::FullscreenMode::Exclusive && m_window.IsHidden()) {
				m_console.Refuse("a hidden window takes no monitor - Exclusive is not applied headless");
				return;
			}
			if (const std::string why = m_ui.StageVideo(mode, size, monitor); !why.empty()) {
				m_console.Refuse(why);
				return;
			}
			const DisplayChoice staged = m_ui.StagedVideo();
			if (staged.adapterLuid != m_device.AdapterLuid()) {
				// Apply opens the restart question for an adapter change; this
				// applies nothing in place, so it says so as a refusal.
				m_ui.ApplyVideo(!EvalRunning());
				m_console.Refuse("the staged adapter is not the running one - Apply asks to "
								 "restart (the question is up); nothing applied in place");
				return;
			}
			// A script's apply is applied and never saved (see onVideoApply).
			const bool persist = !EvalRunning();
			m_ui.ApplyVideo(persist);
			m_console.Print(std::format("video: applied {} {}x{} on monitor {} - {}",
										ModeName(staged.mode), staged.width, staged.height,
										staged.output, persist ? "saved" : "a script's, not saved"));
		});
}

} // namespace dungeon::game
