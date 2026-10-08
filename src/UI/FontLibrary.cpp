#include "UI/FontLibrary.h"

#include "Core/AllocTrack.h"
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
	// A first-time load, allowed inside a steady frame (see Get below).
	const alloc::Excused excuse;
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

namespace {
// Fold in the role's optical correction, then quantize: the atlas is rasterized
// at integer pixels, so a fractional request would otherwise spawn a
// near-duplicate font per sub-pixel size. ONE rule for Get and HeightAt, so a
// height measured without a font is the font's.
int PixelsFor(const FaceSpec& spec, float pixelHeight) {
	return std::max(1, static_cast<int>(std::lround(pixelHeight * spec.scale)));
}
} // namespace

float FontLibrary::HeightAt(FontRole role, float pixelHeight) const {
	return static_cast<float>(PixelsFor(m_roles[static_cast<int>(role)], pixelHeight));
}

void FontLibrary::Prewarm(std::vector<u32> codepoints) {
	// A language load - between frames, never a settled one - and every font
	// it touches uploads its atlas again: excused as a whole.
	const alloc::Excused excuse;
	m_warm = std::move(codepoints);
	for (auto& [key, font] : m_fonts) font->Prewarm(m_warm);
	log::Info("FontLibrary: {} code point(s) past Latin-1 pre-warmed into {} live font(s)",
			  m_warm.size(), m_fonts.size());
}

u64 FontLibrary::LateGlyphs() const {
	u64 late = 0;
	for (const auto& [key, font] : m_fonts) late += font->LateGlyphs();
	return late;
}

Font& FontLibrary::Get(FontRole role, float pixelHeight) {
	const FaceSpec& spec = m_roles[static_cast<int>(role)];
	FaceData face = FaceFor(spec.path);
	const int px = PixelsFor(spec, pixelHeight);

	const Key key{face.get(), px};
	if (auto it = m_fonts.find(key); it != m_fonts.end()) return *it->second;

	// A MISS is a first-time bake: the atlas for a size nothing has drawn at yet
	// (the character sheet's first open builds ~10 MB of them). That is the
	// case Core/AllocTrack.h names as allowed inside a steady frame, so it
	// excuses itself. Only the miss - the hit above allocates nothing and stays
	// guarded, and a caller asking for a new size every frame still shows up as
	// the live-font warning below.
	const alloc::Excused excuse;
	auto font = std::make_unique<Font>(m_device, face, static_cast<float>(px), m_warm);
	Font& ref = *font;
	m_fonts.emplace(key, std::move(font));
	m_peak = std::max(m_peak, m_fonts.size());

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
		out.push_back({std::move(path), key.second, font->LateGlyphs()});
	}
	return out;
}

} // namespace dungeon::ui
