// ============================================================================
// Platform/Process.h — a polled child process (for running AssetBaker).
//
// Start() launches a command line with no console window; Running() is polled
// each frame (non-blocking) until the process exits, then ExitCode() holds its
// result. The editor uses this to bake imported assets without freezing the
// frame (BC7 encode is slow). One process per instance; Start() replaces any
// previous one. Nothing outside Platform sees a Win32 HANDLE.
// ============================================================================
#pragma once

#include <string>

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

private:
	void Close();
	void* m_handle = nullptr; // HANDLE; null when not running
	int m_exitCode = -1;
};

} // namespace dungeon::platform
