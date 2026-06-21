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
	m_snapTranslations.resize(n);
	m_snapRotations.resize(n);
	m_snapScales.resize(n);
	m_globals.resize(n);
	m_palette.resize(n, Mat4Identity());
	SampleClip(nullptr, 0.0f, m_translations, m_rotations, m_scales); // rest pose
	BuildPalette();
}

bool Animator::Play(const std::string& name, bool loop, float fade) {
	if (!m_clips || m_clips->empty()) return false;
	const assets::AnimationClipData* next = nullptr;
	if (name.empty()) {
		next = &m_clips->front();
	} else {
		for (const auto& clip : *m_clips)
			if (clip.name == name) { next = &clip; break; }
	}
	if (!next) {
		log::Warn("Animation clip not found: {}", name);
		return false;
	}

	// Holding a looping state: re-Playing the already-active clip (not mid-fade)
	// is a no-op so the host can call Play every frame without restarting it.
	if (next == m_current && loop && m_loop && m_fadeDuration <= 0.0f) return true;

	// Cross-fade: freeze the pose evaluated last Update as the blend source.
	if (fade > 0.0f && HasSkeleton()) {
		m_snapTranslations = m_translations;
		m_snapRotations = m_rotations;
		m_snapScales = m_scales;
		m_fade = 0.0f;
		m_fadeDuration = fade;
	} else {
		m_fadeDuration = 0.0f;
	}

	m_current = next;
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

	// Active clip's local pose into the working arrays.
	SampleClip(m_current, m_time, m_translations, m_rotations, m_scales);

	// Blend in the frozen snapshot while fading (snapshot → active as w: 0 → 1).
	if (m_fadeDuration > 0.0f) {
		m_fade += dt;
		float w = m_fade / m_fadeDuration;
		w = std::clamp(w, 0.0f, 1.0f);
		w = w * w * (3.0f - 2.0f * w); // smoothstep ease
		for (size_t i = 0; i < m_translations.size(); ++i) {
			m_translations[i] = Lerp(m_snapTranslations[i], m_translations[i], w);
			m_scales[i] = Lerp(m_snapScales[i], m_scales[i], w);
			const XMVECTOR qa = XMLoadFloat4(&m_snapRotations[i]);
			const XMVECTOR qb = XMLoadFloat4(&m_rotations[i]);
			XMStoreFloat4(&m_rotations[i],
						  XMQuaternionNormalize(XMQuaternionSlerp(qa, qb, w)));
		}
		if (m_fade >= m_fadeDuration) m_fadeDuration = 0.0f; // fade complete
	}

	BuildPalette();
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
// Clip sampling. Channels are sparse: a clip may animate only some joints and
// only some paths (T/R/S), so we always start from the rest pose and let
// channels overwrite what they own. Rotations slerp; T/S lerp. A null clip
// leaves the pure rest pose (used for the initial pose and fade-to-rest).
// ----------------------------------------------------------------------------
void Animator::SampleClip(const assets::AnimationClipData* clip, float time,
						  std::vector<Vec3>& outT, std::vector<Quat>& outR,
						  std::vector<Vec3>& outS) const {
	const auto& joints = m_skeleton->joints;

	// Start from the rest pose.
	for (size_t i = 0; i < joints.size(); ++i) {
		outT[i] = joints[i].restTranslation;
		outR[i] = joints[i].restRotation;
		outS[i] = joints[i].restScale;
	}

	// Overlay animated channels.
	if (clip) {
		for (const auto& ch : clip->channels) {
			if (ch.joint < 0 || ch.joint >= static_cast<int>(joints.size())) continue;
			if (ch.times.empty() || ch.values.empty()) continue;
			const KeyLerp k = FindKeys(ch.times, time);
			const Vec4& va = ch.values[std::min(k.a, ch.values.size() - 1)];
			const Vec4& vb = ch.values[std::min(k.b, ch.values.size() - 1)];

			switch (ch.path) {
			case assets::ChannelPath::Translation:
				outT[ch.joint] = Lerp({va.x, va.y, va.z}, {vb.x, vb.y, vb.z}, k.t);
				break;
			case assets::ChannelPath::Scale:
				outS[ch.joint] = Lerp({va.x, va.y, va.z}, {vb.x, vb.y, vb.z}, k.t);
				break;
			case assets::ChannelPath::Rotation: {
				const XMVECTOR qa = XMLoadFloat4(&va);
				const XMVECTOR qb = XMLoadFloat4(&vb);
				XMStoreFloat4(&outR[ch.joint],
							  XMQuaternionNormalize(XMQuaternionSlerp(qa, qb, k.t)));
				break;
			}
			}
		}
	}
}

// ----------------------------------------------------------------------------
// Local → global → palette from the current (possibly fade-blended) local TRS.
// Joints are ordered parent-before-child, so one forward pass suffices.
// ----------------------------------------------------------------------------
void Animator::BuildPalette() {
	const auto& joints = m_skeleton->joints;
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
