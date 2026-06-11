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
	bool Play(const std::string& name = {}, bool loop = true);

	void Update(float dt);

	const std::vector<Mat4>& Palette() const { return m_palette; }
	size_t JointCount() const { return m_palette.size(); }

private:
	void SamplePose(float time);

	const assets::SkeletonData* m_skeleton = nullptr;
	const std::vector<assets::AnimationClipData>* m_clips = nullptr;
	const assets::AnimationClipData* m_current = nullptr;
	bool m_loop = true;
	float m_time = 0.0f;

	// Working state, joint-indexed.
	std::vector<Vec3> m_translations;
	std::vector<Quat> m_rotations;
	std::vector<Vec3> m_scales;
	std::vector<Mat4> m_globals;
	std::vector<Mat4> m_palette;
};

} // namespace dungeon::anim
