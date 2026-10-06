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
//   AnimTest --contract [--self-test]   Play's promises (below); exit 0 = PASS
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
// THE CONTRACT (--contract, code-review C395) needs no assets: a two-joint rig
// built here, whose clips move it in ways a palette shows. Animator.h promises
// that re-Playing the active clip is a no-op while it loops OR is still fading
// in, so a host may Play a held state every frame; a mid-fade re-Play used to
// re-freeze the half-blended pose and begin again, so a per-frame Play never
// finished a fade. Each case runs an Animator beside a REFERENCE one that is
// never re-Played and demands the two palettes agree, frame by frame; the
// restarts the contract keeps (a one-shot whose fade is over) and an empty
// name (which plays nothing) are checked the other way round. --self-test
// swaps every no-op re-Play for one that restarts by contract (the other
// `loop`), and passes only if exactly those cases then fail - the comparison
// is not blind.
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

// ----------------------------------------------------------------------------
// The contract (--contract): Play's promises on a rig built here
// ----------------------------------------------------------------------------

// A root and one child a unit above it. `walk` carries the root forward and
// swings the child; `wave` holds the root and turns the child a quarter; `fall`
// (played one-shot) drops the root and pitches it. Every clip is a second long.
struct Rig {
	assets::SkeletonData skeleton;
	std::vector<assets::AnimationClipData> clips;
};

Vec4 TurnZ(float radians) {
	return {0.0f, 0.0f, std::sin(radians * 0.5f), std::cos(radians * 0.5f)};
}

assets::AnimationClipData MakeClip(const char* name, const std::vector<assets::ChannelKeys>& keys) {
	assets::AnimationClipData clip;
	clip.name = name;
	clip.duration = 1.0f;
	for (const assets::ChannelKeys& k : keys) clip.Add(k);
	return clip;
}

Rig MakeRig() {
	using assets::ChannelPath;
	Rig rig;
	assets::JointData root, child;
	root.name = "root";
	child.name = "child";
	child.parent = 0;
	child.restTranslation = {0.0f, 1.0f, 0.0f};
	rig.skeleton.joints = {root, child};
	rig.clips.push_back(MakeClip(
		"walk", {{0, ChannelPath::Translation, {0.0f, 1.0f}, {{0, 0, 0, 0}, {0, 0, 2, 0}}},
				 {1, ChannelPath::Rotation, {0.0f, 0.5f, 1.0f}, {TurnZ(0), TurnZ(0.6f), TurnZ(0)}}}));
	rig.clips.push_back(
		MakeClip("wave", {{1, ChannelPath::Rotation, {0.0f, 1.0f}, {TurnZ(0), TurnZ(1.5708f)}}}));
	rig.clips.push_back(MakeClip(
		"fall", {{0, ChannelPath::Translation, {0.0f, 1.0f}, {{0, 0, 0, 0}, {1, -0.5f, 0, 0}}},
				 {0, ChannelPath::Rotation, {0.0f, 1.0f}, {TurnZ(0), TurnZ(1.2f)}}}));
	return rig;
}

float PaletteDiff(const anim::Animator& a, const anim::Animator& b) {
	float worst = 0.0f;
	for (size_t j = 0; j < a.JointCount() && j < b.JointCount(); ++j) {
		const float* fa = &a.Palette()[j]._11;
		const float* fb = &b.Palette()[j]._11;
		for (int k = 0; k < 16; ++k) worst = std::max(worst, std::fabs(fa[k] - fb[k]));
	}
	return worst;
}

struct CaseResult {
	const char* name;
	bool injected; // --self-test feeds this case a restart in place of its no-op
	bool pass;
	std::string detail;
};

// After the re-Play under test, `a` and `ref` (never re-Played) must agree on
// the palette and on whether a fade is running, frame by frame. `held` is the
// case's own precondition (what Play returned, and the fade it found), `what`
// says it in words.
CaseResult Agree(const char* name, anim::Animator& a, anim::Animator& ref, bool held,
				 const std::string& what, bool injected) {
	float worst = PaletteDiff(a, ref);
	bool fadeAgrees = a.Fading() == ref.Fading();
	for (int f = 0; f < 12; ++f) {
		a.Update(0.05f);
		ref.Update(0.05f);
		worst = std::max(worst, PaletteDiff(a, ref));
		fadeAgrees = fadeAgrees && a.Fading() == ref.Fading();
	}
	const bool pass = held && worst <= kTolerance && fadeAgrees;
	return {name, injected, pass,
			std::format("{}; palettes differ by up to {:g}; fades {}", what, worst,
						fadeAgrees ? "agree" : "DISAGREE")};
}

std::vector<CaseResult> RunContract(bool selfTest) {
	const Rig rig = MakeRig();
	auto fresh = [&] { return anim::Animator(&rig.skeleton, &rig.clips); };
	std::vector<CaseResult> out;

	{ // A looping clip re-Played while it fades in: nothing restarts.
		anim::Animator a = fresh(), ref = fresh();
		for (anim::Animator* x : {&a, &ref}) {
			x->Play("walk", true);
			x->Update(0.4f);
			x->Play("wave", true, 0.5f);
			x->Update(0.1f);
		}
		const bool fading = a.Fading();
		const bool ok = a.Play("wave", /*loop*/ !selfTest, 0.5f);
		out.push_back(Agree("a looping clip re-Played mid-fade is a no-op", a, ref, ok && fading,
							std::format("re-Played {} the fade, Play returned {}",
										fading ? "inside" : "OUTSIDE", ok),
							true));
	}
	{ // The held state Played EVERY frame, as DriveMonsterAnim may: the fade ends.
		anim::Animator a = fresh();
		a.Play("walk", true);
		a.Update(0.4f);
		float t = 0.0f;
		bool finished = false;
		for (int f = 0; f < 40 && !finished; ++f) {
			a.Play("wave", selfTest ? (f % 2 == 1) : true, 0.5f);
			a.Update(0.05f);
			t += 0.05f;
			finished = !a.Fading();
		}
		out.push_back({"a held state Played every frame finishes its 0.5 s fade", true,
					   finished && t <= 0.56f,
					   finished ? std::format("finished after {:.2f} s", t)
								: "still fading after 2 s"});
	}
	{ // A one-shot re-Played while it fades in: nothing restarts.
		anim::Animator a = fresh(), ref = fresh();
		for (anim::Animator* x : {&a, &ref}) {
			x->Play("walk", true);
			x->Update(0.3f);
			x->Play("fall", false, 0.4f);
			x->Update(0.1f);
		}
		const bool fading = a.Fading();
		const bool ok = a.Play("fall", /*loop*/ selfTest, 0.4f);
		out.push_back(Agree("a one-shot re-Played mid-fade is a no-op", a, ref, ok && fading,
							std::format("re-Played {} the fade, Play returned {}",
										fading ? "inside" : "OUTSIDE", ok),
							true));
	}
	{ // A held loop, not fading: the plain no-op Animator.h always promised.
		anim::Animator a = fresh(), ref = fresh();
		for (anim::Animator* x : {&a, &ref}) {
			x->Play("wave", true);
			x->Update(0.3f);
		}
		const bool ok = a.Play("wave", /*loop*/ !selfTest);
		out.push_back(Agree("a held loop re-Played is a no-op", a, ref, ok && !a.Fading(),
							std::format("Play returned {}", ok), true));
	}
	{ // A one-shot whose fade is over DOES restart: back to its first frame.
		anim::Animator a = fresh(), start = fresh();
		a.Play("fall", false);
		a.Update(0.6f);
		start.Play("fall", false);
		start.Update(0.0f);
		const float moved = PaletteDiff(a, start); // non-vacuous: 0.6 s in is elsewhere
		const bool ok = a.Play("fall", false);
		a.Update(0.0f);
		const float back = PaletteDiff(a, start);
		out.push_back({"a one-shot re-Played after its fade restarts", false,
					   ok && moved > 0.1f && back <= kTolerance,
					   std::format("{:g} from its first frame before, {:g} after", moved, back)});
	}
	{ // An empty name names no clip: false, and the pose carries on as it was.
		anim::Animator a = fresh(), ref = fresh();
		for (anim::Animator* x : {&a, &ref}) {
			x->Play("walk", true);
			x->Update(0.3f);
		}
		const bool played = a.Play("", true);
		out.push_back(Agree("an empty name plays nothing", a, ref, !played,
							std::format("Play(\"\") returned {}", played), false));
	}
	return out;
}

int Contract(bool selfTest) {
	const std::vector<CaseResult> cases = RunContract(selfTest);
	int failures = 0, injected = 0, injectedFailed = 0, otherFailed = 0;
	for (const CaseResult& c : cases) {
		std::printf("  %s %s%s\n", c.pass ? "[ok  ]" : "[FAIL]", c.name,
					c.injected && selfTest ? " (fed a restart)" : "");
		std::printf("         %s\n", c.detail.c_str());
		failures += !c.pass;
		injected += c.injected;
		injectedFailed += c.injected && !c.pass;
		otherFailed += !c.injected && !c.pass;
	}
	if (!selfTest) {
		std::printf("animtest RESULT=%s contract=%zu failures=%d\n", failures ? "FAIL" : "PASS",
					cases.size(), failures);
		return failures ? 1 : 0;
	}
	// Exactly the cases fed a restart must fail, and only those.
	const bool ok = injectedFailed == injected && otherFailed == 0;
	std::printf("animtest RESULT=%s self_test=1 fed=%d failed=%d other_failures=%d\n",
				ok ? "PASS" : "FAIL", injected, injectedFailed, otherFailed);
	return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
	alloc::Init();
	if (argc >= 2 && std::string(argv[1]) == "--contract")
		return Contract(argc >= 3 && std::string(argv[2]) == "--self-test");
	const std::string mode = argc >= 3 ? argv[1] : "";
	if (argc < 3 || (mode != "--dump" && mode != "--compare")) {
		std::printf("usage: AnimTest --dump <file> | --compare <file> | --contract [--self-test]\n");
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
