// ============================================================================
// Animation/Animator.h — skeletal animation playback.
//
// One Animator = one animated instance. It borrows (does NOT own) the
// skeleton and clip data, so several instances can share one ModelData —
// every monster of a kind animates from the same clips with its own time.
// LIFETIME: the ModelData must outlive every Animator pointing into it.
//
// Per Update the pipeline is:
//   rest pose ─► overlay animated channels ─► local TRS per joint
//             ─► globals (parents first)    ─► palette = invBind * global
// The palette feeds straight into the skinning constant buffer (b2) in
// assets/shaders/scene.hlsl.
// ============================================================================
#pragma once

#include "Assets/Model.h"
#include "Core/MathTypes.h"

#include <string>
#include <vector>

namespace dungeon::anim {
class Animator {
public:
	Animator() = default;
	Animator(const assets::SkeletonData* skeleton,
			 const std::vector<assets::AnimationClipData>* clips);

	bool HasSkeleton() const { return m_skeleton && !m_skeleton->joints.empty(); }

	// Starts the named clip (or the first clip if name is empty). Returns
	// false if no such clip exists.
	//
	// fade > 0 CROSS-FADES into the new clip: the current evaluated pose is
	// frozen and blended toward the new clip over `fade` seconds (snapshot
	// cross-fade — robust to mid-fade interruption, no second clock). fade == 0
	// is a hard cut. Re-Playing the clip that is already the active LOOPING
	// clip (not mid-fade) is a no-op, so a host may call Play every frame for a
	// held state without restarting it; one-shot (loop == false) clips always
	// restart.
	bool Play(const std::string& name = {}, bool loop = true, float fade = 0.0f);

	void Update(float dt);

	bool Fading() const { return m_fadeDuration > 0.0f; }

	const std::vector<Mat4>& Palette() const { return m_palette; }
	size_t JointCount() const { return m_palette.size(); }

private:
	// Samples `clip` (null = rest pose) at `time` into the given local TRS
	// arrays. Channels are sparse, so it always seeds from the rest pose first.
	void SampleClip(const assets::AnimationClipData* clip, float time,
					std::vector<Vec3>& outT, std::vector<Quat>& outR,
					std::vector<Vec3>& outS) const;
	// Builds m_globals + m_palette from the current local TRS arrays.
	void BuildPalette();

	const assets::SkeletonData* m_skeleton = nullptr;
	const std::vector<assets::AnimationClipData>* m_clips = nullptr;
	const assets::AnimationClipData* m_current = nullptr;
	bool m_loop = true;
	float m_time = 0.0f;

	// Cross-fade: a frozen snapshot of the pose at the moment Play(fade>0) was
	// called, blended toward the active clip as m_fade ramps to m_fadeDuration.
	float m_fade = 0.0f;
	float m_fadeDuration = 0.0f; // 0 = not fading
	std::vector<Vec3> m_snapTranslations;
	std::vector<Quat> m_snapRotations;
	std::vector<Vec3> m_snapScales;

	// Working state, joint-indexed (the active clip's evaluated local pose,
	// blended in place with the snapshot while fading).
	std::vector<Vec3> m_translations;
	std::vector<Quat> m_rotations;
	std::vector<Vec3> m_scales;
	std::vector<Mat4> m_globals;
	std::vector<Mat4> m_palette;
};

} // namespace dungeon::anim
