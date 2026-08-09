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
	// Normalize by LOUDNESS (RMS) to this target instead of by peak. Peak
	// normalization is right for a one-shot, whose peak IS its loudness, and
	// wrong for a bed: a quiet room tone with one door-slam in it gets shoved to
	// full scale by the slam. Negative = off. The peak ceiling still applies as
	// a limit, so this can only ever make something quieter than peak-normalizing.
	float rmsDbfs = -1000.0f;
	// Force ambisonic B-format handling (take W). Auto-detected from the
	// filename for 4-channel sources — b-format / ambix / ambisonic / fuma —
	// because a 4-channel file is otherwise indistinguishable from a quad
	// recording, and the two want opposite folds.
	bool ambisonic = false;

	// CUT a seamless loop out of a long recording. Field recordings are not
	// loops and never will be — a bed has to be made from one. Zero = off.
	float loopSeconds = 0.0f;
	float loopFromSeconds = -1.0f; // < 0 = pick the most typical stretch
	float loopFadeMs = 250.0f;     // equal-power crossfade across the join
};

// Imports <src> (a .wav file, or a folder of them) into <assetsDir>/sounds as
// <name>.wav — or, for a folder, <name>_1.wav .. <name>_N.wav, the numbered
// VARIANTS a footstep needs so it doesn't machine-gun.
bool ImportSound(const std::string& src, const std::string& assetsDir,
				 const std::string& name, const SoundImportOptions& opts);

} // namespace dungeon::baker
