#include "Assets/Wav.h"

#include <dr_wav.h>

#include <format>
#include <memory>

namespace dungeon::assets {

namespace {
// Owns what dr_wav returned from the moment it comes back, so a throw while
// copying out of it - the copy allocates - cannot leak it (code-review C231).
struct DrWavFree {
	void operator()(drwav_int16* samples) const { drwav_free(samples, nullptr); }
};
} // namespace

std::expected<SoundData, std::string> LoadWavFile(const std::string& path) {
	unsigned int channels = 0, sampleRate = 0;
	drwav_uint64 frameCount = 0;
	// dr_wav opens `path` with the narrow fopen, which reads UTF-8 (Core/Paths.h).
	const std::unique_ptr<drwav_int16, DrWavFree> samples(
		drwav_open_file_and_read_pcm_frames_s16(path.c_str(), &channels, &sampleRate,
												&frameCount, nullptr));
	if (!samples) return std::unexpected(std::format("failed to load WAV: {}", path));
	SoundData sound;
	sound.channels = channels;
	sound.sampleRate = sampleRate;
	sound.samples.assign(samples.get(), samples.get() + frameCount * channels);
	return sound;
}

} // namespace dungeon::assets
