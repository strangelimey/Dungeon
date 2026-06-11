#include "Assets/Wav.h"

#include "Core/Log.h"

#include <dr_wav.h>

namespace dungeon::assets {

std::optional<SoundData> LoadWavFile(const std::string& path) {
    unsigned int channels = 0, sampleRate = 0;
    drwav_uint64 frameCount = 0;
    drwav_int16* samples = drwav_open_file_and_read_pcm_frames_s16(
        path.c_str(), &channels, &sampleRate, &frameCount, nullptr);
    if (!samples) {
        log::Warn("Failed to load WAV: {}", path);
        return std::nullopt;
    }
    SoundData sound;
    sound.channels = channels;
    sound.sampleRate = sampleRate;
    sound.samples.assign(samples, samples + frameCount * channels);
    drwav_free(samples, nullptr);
    return sound;
}

} // namespace dungeon::assets
