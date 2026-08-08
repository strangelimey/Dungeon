// ============================================================================
// ImportSound.h — brings a bought/downloaded sound into the engine format.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <string>

namespace dungeon::baker {

// How a source file is normalized on the way in. The defaults describe the
// COMMON case — a positional one-shot — so an ambient bed is the one that has
// to say what it is.
struct SoundImportOptions {
	u32 rate = 44100;      // resample target
	bool mono = true;      // POSITIONAL: downmix, because X3DAudio needs mono
	bool loop = false;     // a bed/flight loop: seam-checked, never trimmed
	bool trim = true;      // strip silence either end (ignored when loop)
	bool normalize = true; // scale to peakDbfs
	float peakDbfs = -1.0f;
};

// Imports <src> (a .wav file, or a folder of them) into <assetsDir>/sounds as
// <name>.wav — or, for a folder, <name>_1.wav .. <name>_N.wav, the numbered
// VARIANTS a footstep needs so it doesn't machine-gun.
bool ImportSound(const std::string& src, const std::string& assetsDir,
				 const std::string& name, const SoundImportOptions& opts);

} // namespace dungeon::baker
