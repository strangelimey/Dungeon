// ============================================================================
// tools/AnimTest/Main.cpp - does a change to animation storage or sampling
// change what a skinned model LOOKS like?
//
// The skinning palette (inverseBind * jointGlobal, one matrix per joint) is the
// whole of what animation hands the renderer, so two builds that produce the
// same palettes at the same moments draw the same pixels. This samples every
// clip of every animated model in assets\models through the real loader and the
// real anim::Animator, and either records the palettes or compares them against
// a recording:
//
//   AnimTest --dump    <file>   record the current build's palettes
//   AnimTest --compare <file>   recompute and compare; exit 0 = PASS
//
// The workflow is before/after: dump on the commit you trust, change the code,
// compare. It is NOT a golden file in git - a recording is only meaningful
// against the build that made it, and the models are gitignored anyway.
//
// WHAT IS SAMPLED, per clip: kSamples evenly spaced moments from 0 to the clip's
// duration inclusive (the endpoints are where key lookup clamps), played as a
// one-shot; then a CROSS-FADE into the clip from the model's first clip, since
// the fade blends a snapshot against the sampled pose and is the other path a
// storage change could disturb.
//
// TOLERANCE, and why there is one: a constant channel stored as one key samples
// through slerp(q, q, 0) where it used to go through slerp(q, q, t), and those
// can differ in the last bit of a float. A difference that small is invisible
// and is not what this is looking for; a wrong key, a wrong joint or a dropped
// channel moves a matrix by whole units. The report prints the WORST difference
// seen so the margin is visible rather than assumed.
//
// Also reports what loading cost (allocations, channels kept) so the effect of a
// storage change can be read off the same run.
//
// One machine-readable verdict line:  animtest RESULT=PASS
// ============================================================================
#include "Animation/Animator.h"
#include "Assets/Model.h"
#include "Core/AllocTrack.h"
#include "Core/Paths.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace dungeon;

namespace {

constexpr int kSamples = 24;
constexpr float kFade = 0.3f;
constexpr float kTolerance = 1e-4f;

// Whether a glTF names animations, read off its JSON without decoding anything
// (a GLB's JSON is its first chunk). Loading every model in the pool fully would
// decode hundreds of MB of embedded textures to find the handful that animate.
bool HasAnimations(const std::filesystem::path& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) return false;
	char magic[4] = {};
	in.read(magic, 4);
	std::string json;
	if (std::memcmp(magic, "glTF", 4) == 0) {
		unsigned int header[4] = {}; // version, total length, chunk length, chunk type
		in.read(reinterpret_cast<char*>(header), sizeof(header));
		json.resize(header[2]);
		in.read(json.data(), static_cast<std::streamsize>(json.size()));
	} else {
		in.seekg(0);
		json.assign(std::istreambuf_iterator<char>(in), {});
	}
	return json.find("\"animations\"") != std::string::npos;
}

using Recording = std::map<std::string, std::vector<float>>;

void Append(std::vector<float>& out, const anim::Animator& a) {
	for (const Mat4& m : a.Palette()) {
		const float* f = &m._11;
		out.insert(out.end(), f, f + 16);
	}
}

struct Totals {
	int models = 0, clips = 0;
	size_t channels = 0, keys = 0;
};

Recording Sample(Totals& totals) {
	Recording rec;
	std::vector<std::filesystem::path> files;
	for (const auto& e : std::filesystem::directory_iterator(paths::Asset("models"))) {
		const std::string ext = e.path().extension().string();
		if ((ext == ".gltf" || ext == ".glb") && HasAnimations(e.path()))
			files.push_back(e.path());
	}
	std::ranges::sort(files);

	for (const auto& path : files) {
		const alloc::Counters before = alloc::ThisThread();
		auto model = assets::LoadModel(path.string());
		const alloc::Counters after = alloc::ThisThread();
		if (!model || model->skeleton.joints.empty() || model->clips.empty()) continue;
		const std::string stem = path.filename().string();

		size_t channels = 0, keys = 0;
		for (const auto& clip : model->clips) {
			channels += clip.channels.size();
			for (const auto& ch : clip.channels) keys += clip.Times(ch).size();
		}
		std::printf("  %-26s %3zu joints %3zu clips %6zu channels %8zu keys  %7llu allocs\n",
					stem.c_str(), model->skeleton.joints.size(), model->clips.size(),
					channels, keys,
					static_cast<unsigned long long>(after.allocs - before.allocs));
		++totals.models;
		totals.clips += static_cast<int>(model->clips.size());
		totals.channels += channels;
		totals.keys += keys;

		for (const auto& clip : model->clips) {
			std::vector<float>& out = rec[stem + " / " + clip.name];
			for (int s = 0; s < kSamples; ++s) {
				anim::Animator a(&model->skeleton, &model->clips);
				a.Play(clip.name, /*loop*/ false);
				a.Update(clip.duration * static_cast<float>(s) / (kSamples - 1));
				Append(out, a);
			}
			// The cross-fade path: from the first clip, part-way in, into this one.
			anim::Animator a(&model->skeleton, &model->clips);
			a.Play(model->clips.front().name, /*loop*/ true);
			a.Update(model->clips.front().duration * 0.37f);
			a.Play(clip.name, /*loop*/ true, kFade);
			a.Update(kFade * 0.5f);
			Append(out, a);
		}
	}
	return rec;
}

bool Write(const std::string& file, const Recording& rec) {
	std::ofstream out(file, std::ios::binary);
	if (!out) return false;
	for (const auto& [key, floats] : rec) {
		const unsigned int kl = static_cast<unsigned int>(key.size());
		const unsigned int n = static_cast<unsigned int>(floats.size());
		out.write(reinterpret_cast<const char*>(&kl), 4);
		out.write(key.data(), kl);
		out.write(reinterpret_cast<const char*>(&n), 4);
		out.write(reinterpret_cast<const char*>(floats.data()), n * sizeof(float));
	}
	return static_cast<bool>(out);
}

bool Read(const std::string& file, Recording& rec) {
	std::ifstream in(file, std::ios::binary);
	if (!in) return false;
	unsigned int kl = 0;
	while (in.read(reinterpret_cast<char*>(&kl), 4)) {
		std::string key(kl, '\0');
		unsigned int n = 0;
		in.read(key.data(), kl);
		in.read(reinterpret_cast<char*>(&n), 4);
		std::vector<float> floats(n);
		in.read(reinterpret_cast<char*>(floats.data()), n * sizeof(float));
		if (!in) return false;
		rec.emplace(std::move(key), std::move(floats));
	}
	return true;
}

} // namespace

int main(int argc, char** argv) {
	alloc::Init();
	const std::string mode = argc >= 3 ? argv[1] : "";
	if (argc < 3 || (mode != "--dump" && mode != "--compare")) {
		std::printf("usage: AnimTest --dump <file> | --compare <file>\n");
		return 2;
	}
	const std::string file = argv[2];

	Totals totals;
	const Recording now = Sample(totals);
	std::printf("  total: %d models, %d clips, %zu channels, %zu keys\n", totals.models,
				totals.clips, totals.channels, totals.keys);
	if (totals.models == 0) {
		std::printf("animtest RESULT=FAIL reason=no-animated-models\n");
		return 1;
	}

	if (mode == "--dump") {
		if (!Write(file, now)) {
			std::printf("animtest RESULT=FAIL reason=cannot-write\n");
			return 1;
		}
		std::printf("animtest RESULT=DUMPED clips=%zu file=%s\n", now.size(), file.c_str());
		return 0;
	}

	Recording then;
	if (!Read(file, then)) {
		std::printf("animtest RESULT=FAIL reason=cannot-read\n");
		return 1;
	}
	int failures = 0;
	float worst = 0.0f;
	std::string worstKey;
	for (const auto& [key, ref] : then) {
		const auto it = now.find(key);
		if (it == now.end()) {
			std::printf("  MISSING  %s\n", key.c_str());
			++failures;
			continue;
		}
		if (it->second.size() != ref.size()) {
			std::printf("  SIZE     %s: %zu floats, recorded %zu\n", key.c_str(),
						it->second.size(), ref.size());
			++failures;
			continue;
		}
		float clipWorst = 0.0f;
		for (size_t i = 0; i < ref.size(); ++i)
			clipWorst = std::max(clipWorst, std::fabs(it->second[i] - ref[i]));
		if (clipWorst > worst) { worst = clipWorst; worstKey = key; }
		if (clipWorst > kTolerance) {
			std::printf("  DIFFERS  %s: max |d| = %g\n", key.c_str(), clipWorst);
			++failures;
		}
	}
	for (const auto& [key, floats] : now)
		if (!then.contains(key)) {
			std::printf("  NEW      %s\n", key.c_str());
			++failures;
		}
	std::printf("  compared %zu clips; worst |d| = %g (%s), tolerance %g\n", then.size(),
				worst, worstKey.empty() ? "-" : worstKey.c_str(), kTolerance);
	std::printf("animtest RESULT=%s clips=%zu failures=%d worst=%g\n",
				failures ? "FAIL" : "PASS", then.size(), failures, worst);
	return failures ? 1 : 0;
}
