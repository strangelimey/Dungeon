// ============================================================================
// Platform/FileDialog.cpp — see FileDialog.h.
// ============================================================================
#include "Platform/FileDialog.h"

#include "Core/StringUtil.h"

#include <Windows.h>
#include <shobjidl.h>

namespace dungeon::platform {

namespace {

std::string RunDialog(HWND__* owner, bool pickFolder, const std::wstring& label,
					  const std::wstring& pattern) {
	std::string result;
	// The dialog needs an STA. CoInitializeEx is reference-counted; only balance
	// it when we were the ones to initialize this thread.
	const HRESULT init =
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	const bool didInit = SUCCEEDED(init);

	IFileOpenDialog* dialog = nullptr;
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
			IShellItem* item = nullptr;
			if (SUCCEEDED(dialog->GetResult(&item))) {
				PWSTR path = nullptr;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
					result = str::Narrow(path);
					CoTaskMemFree(path);
				}
				item->Release();
			}
		}
		dialog->Release();
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
