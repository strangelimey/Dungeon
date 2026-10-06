// ============================================================================
// Platform/Process.cpp — see Process.h.
// ============================================================================
#include "Platform/Process.h"

#include "Core/StringUtil.h"

#include <Windows.h>

#include <shellapi.h> // CommandLineToArgvW

#pragma comment(lib, "Shell32.lib")

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
	m_id = pi.dwProcessId;
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

// ----------------------------------------------------------------------------
// Relaunch pieces (see the header).
// ----------------------------------------------------------------------------

std::vector<std::string> CommandLineArguments() {
	std::vector<std::string> out;
	int argc = 0;
	if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
		for (int i = 1; i < argc; ++i) out.push_back(str::Narrow(argv[i]));
		LocalFree(argv);
	}
	return out;
}

// The rule CommandLineToArgvW reads by: inside quotes, 2n backslashes before a
// quote are n backslashes and the quote ends the argument, 2n+1 are n and a
// literal quote; backslashes before anything else are literal. Bytes are safe to
// walk one at a time: in UTF-8 every byte of a multi-byte character is >= 0x80,
// so none of them is a backslash or a quote.
std::string QuoteArgument(std::string_view arg) {
	if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string_view::npos)
		return std::string(arg);
	std::string out = "\"";
	for (size_t i = 0;; ++i) {
		size_t backslashes = 0;
		while (i < arg.size() && arg[i] == '\\') {
			++backslashes;
			++i;
		}
		if (i == arg.size()) {
			// Doubled, so the closing quote is not read as an escaped one.
			out.append(backslashes * 2, '\\');
			break;
		}
		if (arg[i] == '"') {
			out.append(backslashes * 2 + 1, '\\');
		} else {
			out.append(backslashes, '\\');
		}
		out.push_back(arg[i]);
	}
	out.push_back('"');
	return out;
}

u32 CurrentProcessId() { return static_cast<u32>(GetCurrentProcessId()); }

bool WaitForProcessExit(u32 pid, u32 timeoutMs, u32& waitedMs) {
	waitedMs = 0;
	const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
	// Cannot be opened: no such process, so it has already gone (an id the
	// system handed on since is not ours to wait on either way).
	if (!process) return true;
	const ULONGLONG start = GetTickCount64();
	const DWORD result = WaitForSingleObject(process, static_cast<DWORD>(timeoutMs));
	waitedMs = static_cast<u32>(GetTickCount64() - start);
	CloseHandle(process);
	return result == WAIT_OBJECT_0;
}

} // namespace dungeon::platform
