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
#include <string_view>
#include <vector>

namespace dungeon::anim {
class Animator {
public:
	Animator() = default;
	Animator(const assets::SkeletonData* skeleton,
			 const std::vector<assets::AnimationClipData>* clips);

	bool HasSkeleton() const { return m_skeleton && !m_skeleton->joints.empty(); }

	// Starts the named clip. Returns false if no such clip exists - an empty
	// name names none (there is no "the first clip": for the bought kit that is
	// a spawn lying on the floor, never a sensible default).
	//
	// fade > 0 CROSS-FADES into the new clip: the current evaluated pose is
	// frozen and blended toward the new clip over `fade` seconds (snapshot
	// cross-fade — robust to mid-fade interruption, no second clock). fade == 0
	// is a hard cut. Re-Playing the ACTIVE clip with the same `loop` is a no-op
	// while it is looping or still fading in, so a host may call Play every
	// frame for a held state without restarting it - mid-fade included, where
	// a restart re-froze the half-blended pose and began the fade again, so a
	// per-frame Play never finished one (code-review C395). A one-shot (loop ==
	// false) clip whose fade is over restarts.
	bool Play(const std::string& name, bool loop = true, float fade = 0.0f);

	// ROOT TRAVEL. Bought Mixamo clips carry ROOT MOTION: a walk moves the
	// root joint ~0.77 units forward per cycle (a run up to ~1.3), a death
	// carries the body up to ~0.6 units off. The game already moves the
	// creature itself (a monster glides cell to cell), so a clip that ALSO
	// travels slides the body ahead of its square and snaps it back on every
	// loop. Locked, the root's HORIZONTAL (x, z) travel is taken out - its
	// height (the walk's bob, a fall to the floor) and every rotation are left
	// as authored, so is anything else a clip does in place:
	//   - a LOOPING clip loses its drift: the straight line from where the
	//     root starts to where it ends is subtracted over the cycle, so the
	//     loop closes and the sway and surge within a stride survive (measured
	//     on the skeleton kit: 0.02 units left in a walk, 0.08 in a run);
	//   - a ONE-SHOT keeps the SHAPE of its travel, scaled so the root ends no
	//     farther than `oneShotReach` from where it began - a body still lurches
	//     the way it falls, but stays in its own square.
	// A clip that does not travel is untouched either way. Measured per clip at
	// Play, applied per Update; allocates nothing. The game never calls this
	// itself: every monster Animator comes from DungeonWorld::MonsterAnimator,
	// which does.
	void LockRootTravel(float oneShotReach);

	void Update(float dt);

	bool Fading() const { return m_fadeDuration > 0.0f; }
	// The active clip's name ("" before the first Play) - a readout's word.
	std::string_view CurrentClip() const {
		return m_current ? std::string_view(m_current->name) : std::string_view{};
	}

	const std::vector<Mat4>& Palette() const { return m_palette; }
	size_t JointCount() const { return m_palette.size(); }
	// Where joint i stands in the current pose, in MODEL space (the rest pose
	// until the first Update). Lets a host attach something to the body - a
	// burning monster's plume rides its root joint.
	Vec3 JointPosition(size_t i) const {
		const Mat4& g = m_globals[i];
		return {g._41, g._42, g._43};
	}

private:
	// Samples `clip` (null = rest pose) at `time` into the given local TRS
	// arrays. Channels are sparse, so it always seeds from the rest pose first.
	void SampleClip(const assets::AnimationClipData* clip, float time,
					std::vector<Vec3>& outT, std::vector<Quat>& outR,
					std::vector<Vec3>& outS) const;
	// Builds m_globals + m_palette from the current local TRS arrays.
	void BuildPalette();
	// Measures the active clip's root travel (LockRootTravel); Play calls it.
	void MeasureRootTravel();
	// Takes that travel back out of m_translations[m_root] at m_time.
	void ApplyRootLock();

	const assets::SkeletonData* m_skeleton = nullptr;
	const std::vector<assets::AnimationClipData>* m_clips = nullptr;
	const assets::AnimationClipData* m_current = nullptr;
	bool m_loop = true;
	float m_time = 0.0f;

	// Root travel (LockRootTravel). m_root < 0 = clips play as authored.
	int m_root = -1;
	float m_rootReach = 0.0f;
	Vec3 m_rootStart{};   // the active clip's root at its first key
	Vec3 m_rootTravel{};  // ...and from there to its last, y zeroed
	float m_rootEnd = 0.0f; // the time of that last key
	float m_rootScale = 1.0f; // a one-shot's travel kept (1 = all of it)

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
