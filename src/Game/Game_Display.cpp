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
	choice.adapterLuid = m_device.AdapterLuid(); // the boot already chose it
	choice.monitor = m_settings.displayMonitor;
	choice.width = static_cast<u32>(std::max(0, m_settings.displayWidth));
	choice.height = static_cast<u32>(std::max(0, m_settings.displayHeight));
	choice.mode = m_settings.fullscreen;
	ApplyDisplay(choice);
}

bool Game::ApplyDisplay(const DisplayChoice& choice) {
	// The monitor, BY DEVICE NAME, in the one display list - every monitor,
	// whichever GPU it hangs off (C198: looked up among the RENDERING adapter's
	// outputs, a hybrid laptop's discrete GPU had none, and Borderless did
	// nothing). One the list no longer has (unplugged, renamed by a dock) is the
	// monitor the window is on, said in the log; with no list at all, none.
	const gfx::DisplayList& list = m_ui.Displays();
	int at = list.MonitorIndex(choice.monitor);
	if (at < 0) {
		at = list.MonitorIndexOf(m_window.Monitor());
		if (!choice.monitor.empty())
			log::Warn("display: the monitor {} is not connected - using the one the window is on ({})",
					  choice.monitor,
					  at >= 0 ? list.monitors[static_cast<size_t>(at)].device : std::string("none"));
	}
	const gfx::OutputInfo* output = at >= 0 ? &list.monitors[static_cast<size_t>(at)] : nullptr;

	switch (choice.mode) {
	case gfx::FullscreenMode::Windowed: {
		const u32 w = choice.width > 0 ? choice.width : m_window.Width();
		const u32 h = choice.height > 0 ? choice.height : m_window.Height();
		m_device.SetFullscreen(false, {}, 0, 0); // drop any exclusive state first
		// Centred in the CHOSEN monitor's work area and shrunk to fit it (code-
		// review C196: it centred on the primary whatever Monitor said). With no
		// monitor to name, the window's own.
		ScreenRect work;
		if (output) work = {output->workX, output->workY, output->workWidth, output->workHeight};
		m_window.SetWindowed(w, h, output ? &work : nullptr);
		return true;
	}
	case gfx::FullscreenMode::Borderless: {
		m_device.SetFullscreen(false, {}, 0, 0);
		if (!output) {
			log::Warn("display: Borderless has no monitor to cover - nothing is applied");
			return false;
		}
		m_window.SetBorderless(output->x, output->y, static_cast<u32>(output->width),
							   static_cast<u32>(output->height));
		return true;
	}
	case gfx::FullscreenMode::Exclusive: {
		// A HIDDEN window (`-headless`) never holds a monitor: Exclusive goes
		// through the swapchain, which would switch a display nobody is looking
		// at (code-review C391; the boot already skips a saved mode for it).
		if (m_window.IsHidden()) {
			log::Info("display: Exclusive is not taken by a hidden window");
			return false;
		}
		u32 w = choice.width;
		u32 h = choice.height;
		if ((w == 0 || h == 0) && output) { // default to the monitor's native size
			w = static_cast<u32>(output->width);
			h = static_cast<u32>(output->height);
		}
		// The window goes onto the monitor FIRST: a monitor of another adapter
		// may be refused as the target, and DXGI then takes the output the
		// window is on (GraphicsDevice::SetFullscreen).
		if (output)
			m_window.SetBorderless(output->x, output->y, static_cast<u32>(output->width),
								   static_cast<u32>(output->height));
		if (!m_device.SetFullscreen(true, output ? std::string_view(output->device) : std::string_view(),
									w, h)) {
			log::Warn("display: Exclusive was refused - the window stays Borderless on that "
					  "monitor, and the choice is not saved");
			return false;
		}
		return true;
	}
	}
	return false;
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
	m_device.SetFullscreen(false, {}, 0, 0);
	m_quitRequested = true;
	return true;
}

// ============================================================================
// `video` - the dev console's way into the Video tab
// ============================================================================

void Game::PrintVideoStatus() {
	const gfx::DisplayList& list = m_ui.Displays();
	const auto adapterAt = [&](int i) -> const gfx::AdapterInfo* {
		return i >= 0 && i < static_cast<int>(list.adapters.size())
				   ? &list.adapters[static_cast<size_t>(i)]
				   : nullptr;
	};
	const auto monitorAt = [&](int i) -> const gfx::OutputInfo* {
		return i >= 0 && i < static_cast<int>(list.monitors.size())
				   ? &list.monitors[static_cast<size_t>(i)]
				   : nullptr;
	};

	// STAGED: what the Video tab's Apply would commit. `work` is the chosen
	// monitor's work area - where a Windowed window must land; `device` is what
	// an Apply saves of it.
	const DisplayChoice staged = m_ui.StagedVideo();
	const gfx::AdapterInfo* stagedA = adapterAt(m_ui.StagedAdapterIndex());
	const gfx::OutputInfo* stagedO = monitorAt(staged.output);
	m_console.Print(std::format(
		"video staged: adapter={} monitor={} mode={} size={}x{} work={} device={} adaptername={}",
		stagedA ? m_ui.StagedAdapterIndex() : -1, stagedO ? staged.output : -1,
		ModeName(staged.mode), staged.width, staged.height,
		stagedO ? RectText(stagedO->workX, stagedO->workY, stagedO->workWidth, stagedO->workHeight)
				: std::string("-"),
		stagedO ? stagedO->device : std::string("-"), stagedA ? stagedA->name : std::string("-")));

	// RUNNING: read off the device and the window, never the settings - a
	// script's apply is not saved, and a size the work area could not hold was
	// shrunk. The monitor is the one the window is on, whichever GPU it hangs
	// off; -1 = not in the list.
	const int runA = list.AdapterIndex(m_device.AdapterLuid());
	const int runO = list.MonitorIndexOf(m_window.Monitor());
	const gfx::OutputInfo* runOut = monitorAt(runO);
	const gfx::FullscreenMode runMode = m_device.IsExclusive() ? gfx::FullscreenMode::Exclusive
										: m_window.IsBorderless() ? gfx::FullscreenMode::Borderless
																  : gfx::FullscreenMode::Windowed;
	m_console.Print(std::format(
		"video running: adapter={} monitor={} mode={} size={}x{} window={} hidden={} device={} "
		"adaptername={}",
		runA, runO, ModeName(runMode), m_window.Width(), m_window.Height(),
		RectText(m_window.FrameRect()), m_window.IsHidden() ? 1 : 0,
		runOut ? runOut->device : std::string("-"), m_device.AdapterName()));

	// SAVED: settings.ini's display fields - the monitor by device name and the
	// GPU by what it is ("auto" for either left unset). The identity is free
	// text, so it ends the line.
	m_console.Print(std::format(
		"video saved: monitor={} mode={} size={}x{} adapter={}",
		m_settings.displayMonitor.empty() ? std::string("auto") : m_settings.displayMonitor,
		ModeName(m_settings.fullscreen), m_settings.displayWidth, m_settings.displayHeight,
		m_settings.adapterId.empty() ? std::string("auto") : m_settings.adapterId));

	// The list itself: how many of each, and how often it has been re-read
	// (opening Settings, a display change) - `video displaychange` must move it.
	m_console.Print(std::format("video displays: adapters={} monitors={} refreshes={} "
								"displaychanges={}",
								list.adapters.size(), list.monitors.size(), m_ui.DisplayRefreshes(),
								m_window.DisplayChanges()));

	// The swapchain (code-review C194/C200/C201): its exclusive state now, what
	// the game asked for and whether a focus loss took it; the counts `video
	// drop` must move - state changes the device found, back-buffer rebuilds,
	// frames presented, Presents skipped while minimized, Exclusives re-entered;
	// the cap's aim and the exact refresh it comes from; and the window's DPI and
	// what the process is aware of.
	const gfx::GraphicsDevice::SwapchainStats sw = m_device.SwapStats();
	const gfx::RefreshRate refresh = m_device.Refresh();
	m_console.Print(std::format(
		"video swapchain: exclusive={} wanted={} lost={} statechanges={} recreates={} presents={} "
		"skippedminimized={} reentries={} activations={} cap={} refresh={}/{} interval={} dpi={} "
		"awareness={}",
		sw.exclusive ? 1 : 0, sw.wanted ? 1 : 0, sw.lost ? 1 : 0, sw.stateChanges, sw.recreates,
		sw.presents, sw.skippedMinimized, sw.reentries, m_window.Activations(),
		gfx::FrameRateText(m_device.FrameCapHz()), refresh.numerator, refresh.denominator,
		m_device.PresentInterval(), m_window.Dpi(), Window::DpiAwarenessName()));

	// Every monitor, which `video apply ... monitor <n>` picks from.
	for (size_t o = 0; o < list.monitors.size(); ++o) {
		const gfx::OutputInfo& out = list.monitors[o];
		m_console.Print(std::format(
			"video monitor {}: desktop={} work={} modes={} primary={} device={} name={}", o,
			RectText(out.x, out.y, out.width, out.height),
			RectText(out.workX, out.workY, out.workWidth, out.workHeight), out.modes.size(),
			out.primary ? 1 : 0, out.device, out.name));
	}
}

void Game::RegisterDisplayCommands() {
	m_console.Register(
		{.name = "video",
		 .group = CmdGroup::Settings,
		 .params = "status\n"
				   "apply [windowed|borderless|exclusive] [<w>x<h>|native] [monitor <n>|last]\n"
				   "restage\n"
				   "restart\n"
				   "displaychange\n"
				   "ghost\n"
				   "ini\n"
				   "drop\n"
				   "activate",
		 .summary = "the Video tab's display choice: show it, stage and apply it (a script's is "
					"not saved), re-stage it as opening Settings does, relaunch, check the "
					"display list and the saved monitor, or lose and regain exclusive mode"},
		[this](const std::vector<std::string>& args) {
			const std::string verb = args.empty() ? "status" : args[0];
			if (verb == "status" && args.size() <= 1) {
				PrintVideoStatus();
				return;
			}
			if (verb == "displaychange" && args.size() == 1) {
				// The real message through the real pump: the list is re-read at
				// the top of the next frame, as for a monitor plugged in (C199).
				m_window.PostDisplayChange();
				m_console.Print(std::format("video: WM_DISPLAYCHANGE posted - the display list "
											"(read {} time(s)) is re-read next frame",
											m_ui.DisplayRefreshes()));
				return;
			}
			if (verb == "drop" && args.size() == 1) {
				// What Alt+Tab does to an Exclusive swapchain (C194): its state
				// goes behind the device's back, and the next frame must notice
				// and rebuild the back buffers before it presents. Not Exclusive
				// (a harness's run, which must take no monitor), the device is
				// made to remember a state the swapchain does not hold - the same
				// mismatch for BeginFrame to find.
				if (m_window.IsHidden()) {
					m_console.Refuse("a hidden window draws no frame - nothing would present "
									 "after the drop");
					return;
				}
				const char* how = m_device.DropExclusiveForTest();
				m_console.Print(std::format("video: exclusive state dropped ({}) - the next frame "
											"rebuilds the back buffers ({} so far)",
											how, m_device.SwapStats().recreates));
				return;
			}
			if (verb == "activate" && args.size() == 1) {
				// The real message through the real pump: what coming back to the
				// game sends, which re-enters an Exclusive a drop took (C194).
				m_window.PostActivate();
				m_console.Print(std::format("video: WM_ACTIVATEAPP posted - an Exclusive the game "
											"lost ({}) is re-entered next frame",
											m_device.ExclusiveLost() ? "lost" : "none lost"));
				return;
			}
			if (verb == "ghost" && args.size() == 1) {
				m_ui.StageGhostMonitor();
				const DisplayChoice s = m_ui.StagedVideo();
				m_console.Print(std::format("video: a monitor that is not there ({}) is listed and "
											"staged as monitor {} - a display change must drop it",
											s.monitor, s.output));
				return;
			}
			if (verb == "ini" && args.size() == 1) {
				// The staged monitor and the running GPU written as Save writes
				// them and read back as Load reads them - the text, never the file,
				// which is the settings.ini the player's build uses.
				const DisplayChoice s = m_ui.StagedVideo();
				if (s.monitor.empty()) {
					m_console.Refuse("no monitor is staged - nothing to round-trip");
					return;
				}
				GameSettings out = m_settings;
				out.displayMonitor = s.monitor;
				out.adapterId = gfx::EncodeAdapterIdentity(m_device.AdapterIdentityInfo());
				GameSettings back;
				back.Parse(out.Text());
				const bool monitorSame = back.displayMonitor == out.displayMonitor;
				const bool adapterSame = back.adapterId == out.adapterId;
				// The identity is free text, so it ends the line (as read back).
				const std::string line = std::format(
					"video ini: monitor={} monitorread={} monitorsame={} adaptersame={} adapter={}",
					out.displayMonitor, back.displayMonitor.empty() ? "-" : back.displayMonitor,
					monitorSame ? 1 : 0, adapterSame ? 1 : 0, back.adapterId);
				if (!monitorSame || !adapterSame) {
					m_console.Refuse(line);
					return;
				}
				m_console.Print(line);
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
						// -1 when there is none: StageVideo refuses it.
						monitor = static_cast<int>(m_ui.Displays().monitors.size()) - 1;
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
			if (m_ui.Displays().adapters.empty()) {
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
			if (!m_ui.ApplyVideo(persist)) {
				m_console.Refuse(std::format("{} on monitor {} did not take (see the log) - nothing "
											 "is saved",
											 ModeName(staged.mode), staged.monitor));
				return;
			}
			m_console.Print(std::format("video: applied {} {}x{} on monitor {} ({}) - {}",
										ModeName(staged.mode), staged.width, staged.height,
										staged.output, staged.monitor,
										persist ? "saved" : "a script's, not saved"));
		});
}

} // namespace dungeon::game
