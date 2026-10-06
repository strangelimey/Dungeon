// ============================================================================
// Platform/Process.h - a polled child process (for running AssetBaker), and
// the pieces of a relaunch.
//
// Start() launches a command line with no console window; Running() is polled
// each frame (non-blocking) until the process exits, then ExitCode() holds its
// result. The editor uses this to bake imported assets without freezing the
// frame (BC7 encode is slow). One process per instance; Start() replaces any
// previous one. Nothing outside Platform sees a Win32 HANDLE.
//
// A RELAUNCH (Game::RestartApp, code-review C398) builds its command line from
// this process's own arguments (CommandLineArguments, each put back through
// QuoteArgument) and hands the child its id; the child waits for that id to
// exit (WaitForProcessExit) before it opens the log the two share.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <string>
#include <string_view>
#include <vector>

namespace dungeon::platform {

class Process {
public:
	Process() = default;
	~Process();
	Process(const Process&) = delete;
	Process& operator=(const Process&) = delete;

	// Launches `commandLine` (the full, already-quoted command). Returns false
	// if the process could not be started.
	bool Start(const std::string& commandLine);
	// Polls: true while the process runs, false before Start and once it exits.
	bool Running();
	// The process's exit code; valid once Running() has returned false.
	int ExitCode() const { return m_exitCode; }
	// The id of the process the last successful Start launched (0 before one).
	u32 Id() const { return m_id; }

private:
	void Close();
	void* m_handle = nullptr; // HANDLE; null when not running
	int m_exitCode = -1;
	u32 m_id = 0;
};

// The arguments this process was started with (argv[1..], UTF-8), split the
// way CommandLineToArgvW splits GetCommandLineW - the split every flag reader
// in the game uses, so what is put back is what was read.
std::vector<std::string> CommandLineArguments();

// One argument written so CommandLineToArgvW reads it back unchanged: as it is
// when it holds no space, tab or quote (and is not empty), else in quotes, with
// a quote inside escaped and every run of backslashes before a quote - or
// before the closing one - doubled.
std::string QuoteArgument(std::string_view arg);

// This process's id.
u32 CurrentProcessId();

// Waits up to `timeoutMs` for process `pid` to exit. True when it has (or no
// such process is running - it is already gone); false on the timeout.
// `waitedMs` is how long it took either way.
bool WaitForProcessExit(u32 pid, u32 timeoutMs, u32& waitedMs);

} // namespace dungeon::platform
