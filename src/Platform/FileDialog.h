// ============================================================================
// Platform/FileDialog.h — native Win32 open-file / open-folder dialogs.
//
// Modal IFileOpenDialog wrappers for the in-game editor's asset import (P4):
// pick a source model file or a texture folder to hand to AssetBaker. Both
// block the message pump while shown (the editor is paused anyway) and return
// the chosen path as UTF-8, or empty on cancel. `owner` parents the dialog for
// correct modality; nothing outside Platform sees a Win32/COM type.
// ============================================================================
#pragma once

#include <string>

struct HWND__;

namespace dungeon::platform {

// Open-file dialog filtered to one file type (e.g. label "glTF models",
// pattern "*.gltf;*.glb"); an empty pattern shows all files.
std::string PickFile(HWND__* owner, const std::wstring& filterLabel,
					 const std::wstring& filterPattern);

// Folder picker (for a texture set's source directory).
std::string PickFolder(HWND__* owner);

} // namespace dungeon::platform
