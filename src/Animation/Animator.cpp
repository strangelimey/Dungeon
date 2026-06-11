#include "Animation/Animator.h"

#include "Core/Log.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace dungeon::anim {

Animator::Animator(const assets::SkeletonData* skeleton,
				   const std::vector<assets::AnimationClipData>* clips)
	: m_skeleton(skeleton), m_clips(clips) {
	if (!HasSkeleton()) return;
	const size_t n = m_skeleton->joints.size();
	m_translations.resize(n);
	m_rotations.resize(n);
	m_scales.resize(n);
	m_globals.resize(n);
	m_palette.resize(n, Mat4Identity());
	SamplePose(0.0f); // rest pose until a clip plays
}

bool Animator::Play(const std::string& name, bool loop) {
	if (!m_clips || m_clips->empty()) return false;
	m_current = nullptr;
	if (name.empty()) {
		m_current = &m_clips->front();
	} else {
		for (const auto& clip : *m_clips)
			if (clip.name == name) { m_current = &clip; break; }
	}
	if (!m_current) {
		log::Warn("Animation clip not found: {}", name);
		return false;
	}
	m_loop = loop;
	m_time = 0.0f;
	return true;
}

void Animator::Update(float dt) {
	if (!HasSkeleton()) return;
	if (m_current) {
		m_time += dt;
		if (m_loop && m_current->duration > 0.0f)
			m_time = std::fmod(m_time, m_current->duration);
		else
			m_time = std::min(m_time, m_current->duration);
	}
	SamplePose(m_time);
}

namespace {

// Finds the keyframe pair bracketing `time` and the interpolation factor.
struct KeyLerp {
	size_t a, b;
	float t;
};

KeyLerp FindKeys(const std::vector<float>& times, float time) {
	if (times.empty()) return {0, 0, 0.0f};
	if (time <= times.front()) return {0, 0, 0.0f};
	if (time >= times.back()) return {times.size() - 1, times.size() - 1, 0.0f};
	auto it = std::upper_bound(times.begin(), times.end(), time);
	const size_t b = static_cast<size_t>(it - times.begin());
	const size_t a = b - 1;
	const float span = times[b] - times[a];
	return {a, b, span > 0.0f ? (time - times[a]) / span : 0.0f};
}

} // namespace

// ----------------------------------------------------------------------------
// Pose evaluation. Channels are sparse: a clip may animate only some joints
// and only some paths (T/R/S), so we always start from the rest pose and let
// channels overwrite what they own. Rotations slerp; T/S lerp.
// ----------------------------------------------------------------------------
void Animator::SamplePose(float time) {
	const auto& joints = m_skeleton->joints;

	// Start from the rest pose.
	for (size_t i = 0; i < joints.size(); ++i) {
		m_translations[i] = joints[i].restTranslation;
		m_rotations[i] = joints[i].restRotation;
		m_scales[i] = joints[i].restScale;
	}

	// Overlay animated channels.
	if (m_current) {
		for (const auto& ch : m_current->channels) {
			if (ch.joint < 0 || ch.joint >= static_cast<int>(joints.size())) continue;
			if (ch.times.empty() || ch.values.empty()) continue;
			const KeyLerp k = FindKeys(ch.times, time);
			const Vec4& va = ch.values[std::min(k.a, ch.values.size() - 1)];
			const Vec4& vb = ch.values[std::min(k.b, ch.values.size() - 1)];

			switch (ch.path) {
			case assets::ChannelPath::Translation:
				m_translations[ch.joint] = Lerp({va.x, va.y, va.z}, {vb.x, vb.y, vb.z}, k.t);
				break;
			case assets::ChannelPath::Scale:
				m_scales[ch.joint] = Lerp({va.x, va.y, va.z}, {vb.x, vb.y, vb.z}, k.t);
				break;
			case assets::ChannelPath::Rotation: {
				const XMVECTOR qa = XMLoadFloat4(&va);
				const XMVECTOR qb = XMLoadFloat4(&vb);
				XMStoreFloat4(&m_rotations[ch.joint],
							  XMQuaternionNormalize(XMQuaternionSlerp(qa, qb, k.t)));
				break;
			}
			}
		}
	}

	// Local → global → palette. Joints are ordered parent-before-child.
	for (size_t i = 0; i < joints.size(); ++i) {
		const XMMATRIX local =
			XMMatrixScaling(m_scales[i].x, m_scales[i].y, m_scales[i].z) *
			XMMatrixRotationQuaternion(XMLoadFloat4(&m_rotations[i])) *
			XMMatrixTranslation(m_translations[i].x, m_translations[i].y,
								m_translations[i].z);
		XMMATRIX global = local;
		if (joints[i].parent >= 0)
			global = local * XMLoadFloat4x4(&m_globals[joints[i].parent]);
		XMStoreFloat4x4(&m_globals[i], global);

		const XMMATRIX invBind = XMLoadFloat4x4(&joints[i].inverseBind);
		XMStoreFloat4x4(&m_palette[i], invBind * global);
	}
}

} // namespace dungeon::anim
