// ============================================================================
// Platform/Process.cpp — see Process.h.
// ============================================================================
#include "Platform/Process.h"

#include "Core/StringUtil.h"

#include <Windows.h>

namespace dungeon::platform {

Process::~Process() { Close(); }

void Process::Close() {
	if (m_handle) {
		CloseHandle(m_handle);
		m_handle = nullptr;
	}
}

bool Process::Start(const std::string& commandLine) {
	Close();
	m_exitCode = -1;

	// CreateProcessW may write into the command-line buffer, so give it a
	// mutable copy.
	std::wstring cmd = str::Widen(commandLine);
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
						CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
		return false;
	CloseHandle(pi.hThread); // we only track the process
	m_handle = pi.hProcess;
	return true;
}

bool Process::Running() {
	if (!m_handle) return false;
	DWORD code = 0;
	if (GetExitCodeProcess(m_handle, &code) && code == STILL_ACTIVE) return true;
	m_exitCode = static_cast<int>(code);
	Close();
	return false;
}

} // namespace dungeon::platform
