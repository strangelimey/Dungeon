// ============================================================================
// Assets/Wav.h — WAV loading (dr_wav). Any input format is converted to
// 16-bit interleaved PCM; audio::AudioEngine plays it as-is (zero copy, so
// keep the SoundData alive while it plays).
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <expected>
#include <string>
#include <vector>

namespace dungeon::assets {

// PCM sound data, 16-bit interleaved.
struct SoundData {
    u32 channels = 0;
    u32 sampleRate = 0;
    std::vector<i16> samples;
};

std::expected<SoundData, std::string> LoadWavFile(const std::string& path);

} // namespace dungeon::assets
