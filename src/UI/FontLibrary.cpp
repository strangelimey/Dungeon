#include "UI/FontLibrary.h"

#include "Core/Log.h"

#include <algorithm>
#include <cmath>

namespace dungeon::ui {

namespace {
// A live font is a face at one size: its atlas is 256-1024px square for Latin.
// Well past this many and something is asking for a size that moves every
// frame — see the lifetime note in the header.
constexpr size_t kFontCountWarn = 64;

constexpr const char* kRoleNames[kFontRoleCount] = {"body", "display", "script",
												   "mono"};
} // namespace

const char* FontRoleName(FontRole role) {
	const int i = static_cast<int>(role);
	return (i >= 0 && i < kFontRoleCount) ? kRoleNames[i] : "body";
}

bool FontRoleFromName(std::string_view name, FontRole& out) {
	for (int i = 0; i < kFontRoleCount; ++i) {
		if (name == kRoleNames[i]) {
			out = static_cast<FontRole>(i);
			return true;
		}
	}
	return false;
}

FontLibrary::FontLibrary(gfx::GraphicsDevice& device) : m_device(device) {}

void FontLibrary::SetFace(FontRole role, FaceSpec spec) {
	if (spec.scale <= 0.0f) spec.scale = 1.0f;
	m_roles[static_cast<int>(role)] = std::move(spec);
	// Fonts already built for the OLD face are left alone: another role may
	// still resolve to them, and any reference an owner cached must stay valid.
	// They are simply no longer reachable through this role.
}

const FaceSpec& FontLibrary::Face(FontRole role) const {
	return m_roles[static_cast<int>(role)];
}

FaceData FontLibrary::FaceFor(const std::string& path) {
	if (auto it = m_faces.find(path); it != m_faces.end()) return it->second;
	// LoadFace falls back to a system face when `path` is empty or missing, so
	// the result is never null and the miss is cached either way — a bad path
	// is probed (and logged) once, not once per size.
	FaceData face = LoadFace(path);
	m_faces.emplace(path, face);
	log::Info("Font face loaded: {} ({} KB)",
			  path.empty() ? "(system fallback)" : path.c_str(),
			  face->size() / 1024);
	return face;
}

Font& FontLibrary::Get(FontRole role, float pixelHeight) {
	const FaceSpec& spec = m_roles[static_cast<int>(role)];
	FaceData face = FaceFor(spec.path);

	// Fold in the role's optical correction, then quantize: the atlas is
	// rasterized at integer pixels, so a fractional request would otherwise
	// spawn a near-duplicate font per sub-pixel size.
	const int px = std::max(1, static_cast<int>(std::lround(pixelHeight * spec.scale)));

	const Key key{face.get(), px};
	if (auto it = m_fonts.find(key); it != m_fonts.end()) return *it->second;

	auto font = std::make_unique<Font>(m_device, face, static_cast<float>(px));
	Font& ref = *font;
	m_fonts.emplace(key, std::move(font));

	if (!m_warnedCount && m_fonts.size() > kFontCountWarn) {
		m_warnedCount = true;
		log::Warn("FontLibrary: {} live fonts — a caller is likely asking for a "
				  "size that changes every frame (see UI/FontLibrary.h)",
				  m_fonts.size());
	}
	return ref;
}

void FontLibrary::CommitAll() {
	for (auto& [key, font] : m_fonts) font->Commit();
}

std::vector<FontLibrary::Live> FontLibrary::LiveFonts() const {
	std::vector<Live> out;
	out.reserve(m_fonts.size());
	for (const auto& [key, font] : m_fonts) {
		// Recover the path by identity — faces are shared per path, so exactly
		// one entry in m_faces owns this blob.
		std::string path = "(unknown)";
		for (const auto& [p, f] : m_faces) {
			if (f.get() == key.first) {
				path = p.empty() ? "(system fallback)" : p;
				break;
			}
		}
		out.push_back({std::move(path), key.second});
	}
	return out;
}

} // namespace dungeon::ui
