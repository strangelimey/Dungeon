// ============================================================================
// Platform/FileDialog.cpp — see FileDialog.h.
// ============================================================================
#include "Platform/FileDialog.h"

#include "Core/Log.h"
#include "Core/StringUtil.h"

#include <Windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace dungeon::platform {

namespace {

std::string RunDialog(HWND__* owner, bool pickFolder, const std::wstring& label,
					  const std::wstring& pattern) {
	using Microsoft::WRL::ComPtr;
	std::string result;
	const HRESULT init =
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	// Only balance CoUninitialize when we were the ones to initialize the thread
	// (CoInitializeEx is reference-counted, and RPC_E_CHANGED_MODE takes no ref).
	const bool didInit = SUCCEEDED(init);
	// RPC_E_CHANGED_MODE means this thread already joined the MTA and cannot
	// leave it. The Common Item Dialog then DEADLOCKS instead of failing — it
	// wedged the whole process once (see AudioEngine's ctor comment), so refuse
	// the call rather than hang. Running it on a private STA thread does not
	// help: Show() messages the owner window, whose thread would be blocked.
	if (init == RPC_E_CHANGED_MODE) {
		log::Warn("file dialog: calling thread is in the MTA — dialog skipped");
		return result;
	}

	{
		// Scoped so the ComPtrs Release before the CoUninitialize below.
		ComPtr<IFileOpenDialog> dialog;
		if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
									   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
			DWORD options = 0;
			dialog->GetOptions(&options);
			if (pickFolder) {
				dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
			} else if (!pattern.empty()) {
				const COMDLG_FILTERSPEC spec{label.c_str(), pattern.c_str()};
				dialog->SetFileTypes(1, &spec);
			}

			if (SUCCEEDED(dialog->Show(owner))) {
				ComPtr<IShellItem> item;
				if (SUCCEEDED(dialog->GetResult(&item))) {
					PWSTR path = nullptr;
					if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
						result = str::Narrow(path);
						CoTaskMemFree(path);
					}
				}
			}
		}
	}

	if (didInit) CoUninitialize();
	return result;
}

} // namespace

std::string PickFile(HWND__* owner, const std::wstring& filterLabel,
					 const std::wstring& filterPattern) {
	return RunDialog(owner, /*pickFolder*/ false, filterLabel, filterPattern);
}

std::string PickFolder(HWND__* owner) {
	return RunDialog(owner, /*pickFolder*/ true, {}, {});
}

} // namespace dungeon::platform
