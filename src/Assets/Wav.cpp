#include "Assets/Wav.h"

#include <dr_wav.h>

#include <format>

namespace dungeon::assets {

std::expected<SoundData, std::string> LoadWavFile(const std::string& path) {
    unsigned int channels = 0, sampleRate = 0;
    drwav_uint64 frameCount = 0;
    drwav_int16* samples = drwav_open_file_and_read_pcm_frames_s16(
        path.c_str(), &channels, &sampleRate, &frameCount, nullptr);
    if (!samples) return std::unexpected(std::format("failed to load WAV: {}", path));
    SoundData sound;
    sound.channels = channels;
    sound.sampleRate = sampleRate;
    sound.samples.assign(samples, samples + frameCount * channels);
    drwav_free(samples, nullptr);
    return sound;
}

} // namespace dungeon::assets
