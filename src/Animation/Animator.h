#pragma once

#include "Assets/Model.h"
#include "Core/MathTypes.h"

#include <string>
#include <vector>

namespace dungeon::anim {

// Plays one clip at a time over a skeleton and produces the skinning palette
// (jointGlobal * inverseBind) consumed by the GPU skinning shader.
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
