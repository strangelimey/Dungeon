// ============================================================================
// Game/AssetPicker.cpp — see AssetPicker.h.
// ============================================================================
#include "Game/AssetPicker.h"

#include "Assets/Dds.h"
#include "Assets/Image.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/DungeonWorld.h" // kIconSize: an icon target must match the bake
#include "UI/Controls.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>

namespace dungeon::game {

namespace {
// Panel + region geometry as window fractions — a large card, since the whole
// point is to see many assets at once.
constexpr gfx::Rect kPanel{0.08f, 0.07f, 0.84f, 0.86f};
constexpr gfx::Rect kTitle{0.10f, 0.095f, 0.40f, 0.04f};
constexpr gfx::Rect kSearch{0.10f, 0.145f, 0.26f, 0.040f};
constexpr gfx::Rect kClear{0.365f, 0.145f, 0.030f, 0.040f};
constexpr gfx::Rect kUsedChip{0.41f, 0.145f, 0.14f, 0.040f};
constexpr gfx::Rect kSurfaceChip{0.41f, 0.185f, 0.14f, 0.040f};
// The grid, and the details column beside it.
constexpr gfx::Rect kGrid{0.10f, 0.235f, 0.46f, 0.63f};
constexpr float kDetailX = 0.60f;
constexpr gfx::Rect kChoose{0.60f, 0.815f, 0.10f, 0.045f};

// Tile metrics inside the grid (fractions of the WINDOW, like everything else).
constexpr int kCols = 4;
constexpr float kTileW = 0.108f, kTileH = 0.175f, kTilePad = 0.007f;
constexpr float kScrollW = 0.010f;
// A thumbnail is the set's mip chain trimmed to levels this size or smaller.
constexpr u32 kThumbPx = 128;
// How many tile images to keep. Each is ~16 KB of VRAM and one SRV slot, so
// this is generous; it exists so a long scroll can't grow without bound.
constexpr size_t kThumbCap = 160;

std::string Lower(std::string s) {
	std::ranges::transform(s, s.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return s;
}

// "1k 2k 4k" for the tile's badge line.
std::string ResBadge(u32 mask) {
	std::string out;
	const std::pair<u32, const char*> tags[] = {
		{kRes1k, "1k"}, {kRes2k, "2k"}, {kRes4k, "4k"}};
	for (const auto& [bit, tag] : tags)
		if (mask & bit) {
			if (!out.empty()) out += ' ';
			out += tag;
		}
	return out;
}

std::string SizeText(u64 bytes) {
	if (bytes >= 1024ull * 1024ull)
		return std::format("{:.0f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
	return std::format("{} KB", bytes / 1024);
}

// The smallest installed resolution tag for a set — the cheapest file to read.
const char* SmallestRes(u32 mask) {
	if (mask & kRes1k) return "_1k";
	if (mask & kRes2k) return "_2k";
	if (mask & kRes4k) return "_4k";
	return "_2k";
}
} // namespace

AssetPicker::AssetPicker(gfx::GraphicsDevice& device)
	: m_device(device), m_font(device, "", 18.0f), m_ui(device, "", 18.0f) {
	m_closeIcon = TryLoadTextureFile(device, paths::Asset("ui\\icon_close"));
}

void AssetPicker::Open(Mode mode, const std::string& current,
					   const std::string& label, const ui::Theme& theme) {
	m_open = true;
	m_uiRebuild = false;
	m_mode = mode;
	m_label = label;
	m_selected = current;
	m_search.clear();
	m_onlyUsed = false;
	m_onlySurface = false;
	m_scroll = 0.0f;
	m_hover = -1;
	m_lastClickTile = -1;
	m_theme = theme;
	m_items = mode == Mode::Textures ? InstalledTextureSetInfo() : InstalledModelInfo();
	m_used = usedAssets ? usedAssets() : std::vector<std::string>{};
	// Thumbnails are per-pool: a model tile and a texture tile of the same name
	// are different images, and the cache is keyed by name alone.
	m_thumbs.clear();
	ApplyFilter();
	Rebuild();
	RefreshPreview();
	RefreshFacts();
	// Scroll the current value into view: opening a hundred-tile grid at the top
	// when the field already names one is the dropdown's failing all over again.
	for (size_t i = 0; i < m_shown.size(); ++i)
		if (m_items[m_shown[i]].name == m_selected) {
			const int row = static_cast<int>(i) / kCols;
			m_scroll = std::max(0.0f, static_cast<float>(row) * (kTileH + kTilePad) -
										  kGrid.h * 0.4f);
			break;
		}
}

// --- filtering ---------------------------------------------------------------

void AssetPicker::ApplyFilter() {
	const std::string needle = Lower(m_search);
	m_shown.clear();
	for (size_t i = 0; i < m_items.size(); ++i) {
		const AssetInfo& a = m_items[i];
		if (!needle.empty() && Lower(a.name).find(needle) == std::string::npos)
			continue;
		if (m_onlySurface && m_mode == Mode::Textures && !a.worn) continue;
		if (m_onlyUsed && std::ranges::find(m_used, a.name) == m_used.end()) continue;
		m_shown.push_back(i);
	}
	m_scroll = 0.0f;
}

// --- geometry ----------------------------------------------------------------

gfx::Rect AssetPicker::GridRect(float w, float h) const {
	return {kGrid.x * w, kGrid.y * h, kGrid.w * w, kGrid.h * h};
}

gfx::Rect AssetPicker::TileRect(float w, float h, size_t shownIndex) const {
	const gfx::Rect grid = GridRect(w, h);
	const int col = static_cast<int>(shownIndex) % kCols;
	const int row = static_cast<int>(shownIndex) / kCols;
	return {grid.x + static_cast<float>(col) * (kTileW + kTilePad) * w,
			grid.y + static_cast<float>(row) * (kTileH + kTilePad) * h - m_scroll * h,
			kTileW * w, kTileH * h};
}

float AssetPicker::MaxScroll(float w, float h) const {
	const int rows = (static_cast<int>(m_shown.size()) + kCols - 1) / kCols;
	const float content = static_cast<float>(rows) * (kTileH + kTilePad);
	return std::max(0.0f, content - kGrid.h);
}

int AssetPicker::TileAt(float w, float h, float mx, float my) const {
	const gfx::Rect grid = GridRect(w, h);
	if (!grid.Contains(mx, my)) return -1;
	for (size_t i = 0; i < m_shown.size(); ++i) {
		const gfx::Rect t = TileRect(w, h, i);
		if (t.y + t.h <= grid.y || t.y >= grid.y + grid.h) continue; // scrolled out
		if (t.Contains(mx, my)) return static_cast<int>(i);
	}
	return -1;
}

// --- thumbnails --------------------------------------------------------------

std::unique_ptr<gfx::Texture> AssetPicker::LoadThumb(const std::string& name) const {
	if (m_mode != Mode::Textures) return nullptr; // models are baked by the owner
	const auto it = std::ranges::find(m_items, name, &AssetInfo::name);
	if (it == m_items.end()) return nullptr;
	const std::string stem =
		paths::Asset("textures\\" + name + SmallestRes(it->resolutions));

	// The baked chain, with its big levels dropped: a tile wants 128px, not the
	// 2048px the set installs at. Same file, a four-hundredth of the memory.
	if (auto chain = assets::LoadDdsFile(stem + ".dds")) {
		assets::MipChain thumb;
		thumb.format = chain->format;
		for (const assets::TextureLevel& level : chain->levels) {
			if (level.width > kThumbPx) continue; // the levels a tile can't use
			if (thumb.levels.empty()) {
				thumb.width = level.width;
				thumb.height = level.height;
			}
			thumb.levels.push_back(level);
		}
		if (!thumb.levels.empty())
			return std::make_unique<gfx::Texture>(m_device, thumb, /*srgb*/ true);
	}
	// No baked chain (a source-only set): the PNG, at whatever size it is.
	if (auto img = assets::LoadImageFile(stem + ".png"))
		return std::make_unique<gfx::Texture>(m_device, *img, /*srgb*/ true);
	return nullptr;
}

const gfx::Texture* AssetPicker::ThumbFor(const std::string& name) {
	Thumb& slot = m_thumbs[name];
	slot.lastSeen = m_frame;
	// Only a TEXTURE tile can load its own image. A model tile's icon has to be
	// baked by the owner (SetModelIcon), so this must not mark it tried — that
	// is the flag PendingModelIcons uses to know what still needs baking.
	if (!slot.texture && !slot.tried && m_mode == Mode::Textures) {
		slot.tried = true;
		slot.texture = LoadThumb(name);
	}
	return slot.texture.get();
}

void AssetPicker::EvictThumbs() {
	if (m_thumbs.size() <= kThumbCap) return;
	// Oldest first, and never this frame's (they are on screen right now).
	std::vector<std::pair<u64, std::string>> aged;
	aged.reserve(m_thumbs.size());
	for (const auto& [name, thumb] : m_thumbs)
		if (thumb.lastSeen != m_frame) aged.emplace_back(thumb.lastSeen, name);
	if (aged.empty()) return;
	std::ranges::sort(aged);
	// About to free GPU resources that in-flight frames may still reference — a
	// model tile's mesh is referenced by the bake recorded up to kFrameCount-1
	// frames ago (the preview-reset rule).
	m_device.WaitIdle();
	for (const auto& [when, name] : aged) {
		if (m_thumbs.size() <= kThumbCap) break;
		// The Texture destructor returns its SRV slot to the free list.
		m_thumbs.erase(name);
	}
}

void AssetPicker::PrepareModelIcons(size_t max) {
	if (m_mode != Mode::Models) return;
	size_t made = 0;
	for (size_t i = 0; i < m_shown.size() && made < max; ++i) {
		const AssetInfo& a = m_items[m_shown[i]];
		Thumb& slot = m_thumbs[a.name];
		if (slot.texture || slot.tried) continue;
		slot.tried = true; // one attempt per asset, however it goes
		slot.lastSeen = m_frame;
		auto model = assets::LoadModel(paths::Asset("models\\" + a.file));
		if (!model || model->meshes.empty()) {
			log::Warn("asset picker: no icon for {} (could not load)", a.file);
			continue;
		}
		const assets::MeshData& data = model->meshes[0];
		slot.lo = {1e9f, 1e9f, 1e9f};
		slot.hi = {-1e9f, -1e9f, -1e9f};
		for (const auto& v : data.vertices) {
			slot.lo = {std::min(slot.lo.x, v.position.x), std::min(slot.lo.y, v.position.y),
					   std::min(slot.lo.z, v.position.z)};
			slot.hi = {std::max(slot.hi.x, v.position.x), std::max(slot.hi.y, v.position.y),
					   std::max(slot.hi.z, v.position.z)};
		}
		slot.mesh = std::make_unique<gfx::Mesh>(m_device, data);
		// The bake's viewport and shared depth target are DungeonWorld::kIconSize,
		// so a target we create ourselves has to be exactly that.
		slot.texture = gfx::Texture::RenderTarget(m_device, DungeonWorld::kIconSize);
		slot.needsBake = true;
		++made;
	}
}

std::vector<AssetPicker::PendingBake> AssetPicker::PendingBakes(size_t max) const {
	std::vector<PendingBake> out;
	for (const auto& [name, thumb] : m_thumbs) {
		if (out.size() >= max) break;
		if (!thumb.needsBake || !thumb.mesh || !thumb.texture) continue;
		out.push_back({name, thumb.mesh.get(), thumb.texture.get(), thumb.lo, thumb.hi});
	}
	return out;
}

void AssetPicker::MarkBaked(const std::string& name) {
	const auto it = m_thumbs.find(name);
	if (it != m_thumbs.end()) it->second.needsBake = false;
}

// --- selection, preview, facts ----------------------------------------------

void AssetPicker::SelectIndex(int shownIndex) {
	if (shownIndex < 0 || shownIndex >= static_cast<int>(m_shown.size())) return;
	m_selected = m_items[m_shown[static_cast<size_t>(shownIndex)]].name;
	RefreshPreview();
	RefreshFacts();
}

gfx::Rect AssetPicker::PreviewRect(float w, float h) const {
	const float side = std::min(0.26f * w, 0.34f * h);
	return {kDetailX * w, 0.24f * h, side, side};
}

void AssetPicker::RefreshPreview() {
	// In-flight frames may still reference the old resources (the SRV rule).
	if (m_previewMesh || m_previewAlbedo) m_device.WaitIdle();
	m_previewMesh.reset();
	m_previewAlbedo.reset();
	m_previewNormal.reset();
	m_previewMr.reset();
	m_material = {};
	if (m_selected.empty()) return;

	if (m_mode == Mode::Textures) {
		// The shared wall block wearing the set, as the create dialog previews it.
		const auto it = std::ranges::find(m_items, m_selected, &AssetInfo::name);
		const u32 res = it == m_items.end() ? kRes2k : it->resolutions;
		const std::string stem =
			paths::Asset("textures\\" + m_selected + SmallestRes(res));
		m_previewAlbedo = TryLoadTextureFile(m_device, stem, /*srgb*/ true);
		m_previewNormal = TryLoadTextureFile(m_device, stem + "_n");
		m_previewMr = TryLoadTextureFile(m_device, stem + "_mr");
		if (!m_previewAlbedo) return;
		auto model = assets::LoadModel(paths::Asset("models\\wall_block.gltf"));
		if (!model || model->meshes.empty()) return;
		m_previewModel = std::move(*model);
		m_previewMesh = std::make_unique<gfx::Mesh>(m_device, m_previewModel.meshes[0]);
		m_material.albedo = m_previewAlbedo.get();
		m_material.normalMap = m_previewNormal.get();
		m_material.metalRough = m_previewMr.get();
		return;
	}

	// A model previews itself, with whatever its own glTF material says.
	const auto it = std::ranges::find(m_items, m_selected, &AssetInfo::name);
	const std::string file = it == m_items.end() ? m_selected + ".gltf" : it->file;
	auto model = assets::LoadModel(paths::Asset("models\\" + file));
	if (!model || model->meshes.empty()) {
		log::Warn("asset picker: could not load {}", file);
		return;
	}
	m_previewModel = std::move(*model);
	m_previewMesh = std::make_unique<gfx::Mesh>(m_device, m_previewModel.meshes[0]);
}

void AssetPicker::RefreshFacts() {
	m_facts.clear();
	if (m_selected.empty()) return;
	const auto it = std::ranges::find(m_items, m_selected, &AssetInfo::name);
	if (it == m_items.end()) return;

	if (m_mode == Mode::Textures) {
		m_facts.push_back(loc::Format("pick.fact.res", ResBadge(it->resolutions)));
		std::string maps = loc::Tr("pick.map.albedo");
		if (it->normal) maps += ", " + loc::Tr("pick.map.normal");
		if (it->orm) maps += ", " + loc::Tr("pick.map.orm");
		m_facts.push_back(maps);
		// Is the parallax height real? It rides the normal map's ALPHA, and a
		// flat export (some Poly Haven sets) leaves it constant — which looks
		// like a broken height_scale slider later. One decode, on selection.
		if (it->normal) {
			const std::string npath = paths::Asset(
				"textures\\" + m_selected + SmallestRes(it->resolutions) + "_n.png");
			if (auto img = assets::LoadImageFile(npath)) {
				u8 lo = 255, hi = 0;
				for (size_t p = 3; p < img->pixels.size(); p += 4 * 64) {
					lo = std::min(lo, img->pixels[p]);
					hi = std::max(hi, img->pixels[p]);
				}
				m_facts.push_back(hi - lo > 8 ? loc::Tr("pick.height.real")
											  : loc::Tr("pick.height.flat"));
			}
		} else {
			m_facts.push_back(loc::Tr("pick.height.none"));
		}
		if (it->worn) m_facts.push_back(loc::Tr("pick.worn"));
	}
	m_facts.push_back(SizeText(it->bytes));
	if (std::ranges::find(m_used, m_selected) != m_used.end())
		m_facts.push_back(loc::Tr("pick.inuse"));
	if (sourceOf) {
		const std::string src = sourceOf(m_selected);
		if (!src.empty()) m_facts.push_back(loc::Format("pick.fact.source", src));
	}
}

// --- widgets -----------------------------------------------------------------

void AssetPicker::Rebuild() {
	m_ui.SetTheme(m_theme);
	m_ui.Clear();
	m_searchField = nullptr;

	m_searchField = m_ui.Add<ui::TextField>(kSearch, m_search);
	m_searchField->placeholder = loc::Tr("pick.search");
	m_searchField->maxLength = 40;
	ui::TextField* raw = m_searchField;
	raw->onChange = [this, raw] {
		m_search = raw->text;
		ApplyFilter();
	};
	m_ui.Add<ui::Button>(kClear, "x", [this] {
		m_search.clear();
		ApplyFilter();
		m_uiRebuild = true; // the field's text is its own state — rebuild it
	});
	m_ui.Add<ui::Checkbox>(kUsedChip, loc::Tr("pick.chip.used"), m_onlyUsed,
						   [this](bool on) {
							   m_onlyUsed = on;
							   ApplyFilter();
						   });
	if (m_mode == Mode::Textures)
		m_ui.Add<ui::Checkbox>(kSurfaceChip, loc::Tr("pick.chip.surface"),
							   m_onlySurface, [this](bool on) {
								   m_onlySurface = on;
								   ApplyFilter();
							   });
	m_ui.Add<ui::Button>(kChoose, loc::Tr("pick.choose"), [this] {
		if (m_selected.empty()) return;
		const std::string picked = m_selected;
		Close();
		if (onChoose) onChoose(picked);
	});
	ui::AddCloseButton(m_ui, kPanel, m_closeIcon.get(), [this] { Close(); });
}

// --- input -------------------------------------------------------------------

void AssetPicker::Update(const Input& input, float w, float h, float dt) {
	if (!m_open) return;
	++m_frame;
	m_time += dt;
	m_orbit += dt * 0.6f;
	m_font.Commit();
	const float fh = std::clamp(h * 0.020f, 12.0f, 24.0f);
	m_font.SetHeight(fh);
	m_ui.GetFont().SetHeight(fh);

	if (m_uiRebuild) {
		m_uiRebuild = false;
		Rebuild();
	}

	// Esc closes — unless the search box has focus, where it just drops focus
	// (the palette filter's rule, so Esc never skips a step).
	if (input.WasKeyPressed(VK_ESCAPE)) {
		if (m_searchField && m_searchField->Focused()) {
			m_searchField->SetFocused(false);
			return;
		}
		Close();
		return;
	}

	m_ui.Update(input, w, h);

	const float mx = input.MouseX(), my = input.MouseY();
	const float maxScroll = MaxScroll(w, h);
	m_scroll = std::clamp(m_scroll, 0.0f, maxScroll);

	// Wheel over the grid scrolls a row at a time.
	const gfx::Rect grid = GridRect(w, h);
	if (grid.Contains(mx, my) && input.WheelDelta() != 0.0f)
		m_scroll = std::clamp(m_scroll - input.WheelDelta() * (kTileH + kTilePad),
							  0.0f, maxScroll);

	// Scrollbar drag, in the gutter at the grid's right edge.
	if (maxScroll > 0.0f) {
		const gfx::Rect track{grid.x + grid.w - kScrollW * w, grid.y, kScrollW * w,
							  grid.h};
		const float thumbH =
			std::max(track.h * (kGrid.h / (kGrid.h + maxScroll)), 24.0f);
		const float t = maxScroll > 0.0f ? m_scroll / maxScroll : 0.0f;
		const gfx::Rect thumb{track.x, track.y + (track.h - thumbH) * t, track.w,
							  thumbH};
		if (m_scrollDragging && !input.IsMouseDown(MouseButton::Left))
			m_scrollDragging = false;
		if (thumb.Contains(mx, my) && input.WasMousePressed(MouseButton::Left)) {
			m_scrollDragging = true;
			m_scrollGrab = my - thumb.y;
		}
		if (m_scrollDragging) {
			const float range = track.h - thumbH;
			if (range > 0.0f)
				m_scroll = std::clamp((my - m_scrollGrab - track.y) / range * maxScroll,
									  0.0f, maxScroll);
			return; // the drag owns the mouse
		}
	}

	// Tiles: hover, click to select, double-click to choose.
	m_hover = TileAt(w, h, mx, my);
	if (m_hover >= 0 && input.WasMousePressed(MouseButton::Left)) {
		if (m_searchField) m_searchField->SetFocused(false); // typing goes nowhere now
		const bool again = m_hover == m_lastClickTile && m_time - m_lastClickTime < 0.4;
		SelectIndex(m_hover);
		m_lastClickTile = m_hover;
		m_lastClickTime = m_time;
		if (again && !m_selected.empty()) {
			const std::string picked = m_selected;
			Close();
			if (onChoose) onChoose(picked);
			return;
		}
	}

	// Ready a couple of model icons per frame — the loads and the render-target
	// creation both stall the GPU, so they belong here and not in the frame's
	// recording (see PendingBake). Textures load themselves, in Render.
	PrepareModelIcons(2);
	EvictThumbs();
}

// --- drawing -----------------------------------------------------------------

void AssetPicker::Render(gfx::SpriteBatch& batch, float w, float h) {
	if (!m_open) return;
	const ui::Theme& th = m_ui.GetTheme();
	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.6f});
	const gfx::Rect panel{kPanel.x * w, kPanel.y * h, kPanel.w * w, kPanel.h * h};
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);

	m_font.Draw(batch, loc::Format("pick.title", m_label), kTitle.x * w,
				kTitle.y * h, th.text);
	// The count, so a filter that hides everything says so rather than looking
	// like a broken grid.
	m_font.Draw(batch,
				loc::Format("pick.count", m_shown.size(), m_items.size()),
				(kGrid.x + kGrid.w) * w - 120.0f, kTitle.y * h, th.textDim);

	// --- the grid ------------------------------------------------------------
	const gfx::Rect grid = GridRect(w, h);
	batch.DrawRect(grid, {0.0f, 0.0f, 0.0f, 0.25f});
	batch.SetScissor(&grid);
	for (size_t i = 0; i < m_shown.size(); ++i) {
		const gfx::Rect tile = TileRect(w, h, i);
		if (tile.y + tile.h <= grid.y || tile.y >= grid.y + grid.h) continue;
		const AssetInfo& a = m_items[m_shown[i]];
		const bool picked = a.name == m_selected;
		const bool hot = static_cast<int>(i) == m_hover;
		batch.DrawRect(tile, hot || picked ? th.controlHot : th.control);

		// The image is the tile's top square, but never taller than what the two
		// text lines leave — a square that fills the tile pushes them out of it.
		const float textH = 2.0f * (m_font.Height() + 3.0f);
		const float imgSide = std::min(tile.w - 8.0f, tile.h - 8.0f - textH);
		const gfx::Rect img{tile.x + (tile.w - imgSide) * 0.5f, tile.y + 4.0f, imgSide,
							imgSide};
		if (const gfx::Texture* thumb = ThumbFor(a.name))
			batch.DrawSprite(img, {0, 0, 1, 1}, *thumb, {1, 1, 1, 1});
		else
			batch.DrawRect(img, {0.10f, 0.10f, 0.12f, 1.0f});

		const float ty = img.y + img.h + 3.0f;
		m_font.Draw(batch, a.name, tile.x + 5.0f, ty, picked ? th.accent : th.text);
		const std::string badge = m_mode == Mode::Textures
									  ? ResBadge(a.resolutions)
									  : (a.file.ends_with(".glb") ? "glb" : "gltf");
		m_font.Draw(batch, badge, tile.x + 5.0f, ty + m_font.Height() + 1.0f,
					th.textDim);
		if (picked) ui::DrawBorder(batch, tile, th.accent);
	}
	batch.SetScissor(nullptr);

	// Scrollbar, drawn after the tiles so it is never under one.
	const float maxScroll = MaxScroll(w, h);
	if (maxScroll > 0.0f) {
		const gfx::Rect track{grid.x + grid.w - kScrollW * w, grid.y, kScrollW * w,
							  grid.h};
		const float thumbH =
			std::max(track.h * (kGrid.h / (kGrid.h + maxScroll)), 24.0f);
		const float t = m_scroll / maxScroll;
		batch.DrawRect(track, th.control);
		const gfx::Rect thumb{track.x, track.y + (track.h - thumbH) * t, track.w,
							  thumbH};
		batch.DrawRect(thumb, m_scrollDragging ? th.controlActive : th.controlHot);
		ui::DrawBorder(batch, thumb, th.panelBorder);
	}

	// --- the details column --------------------------------------------------
	const gfx::Rect pv = PreviewRect(w, h);
	batch.DrawRect(pv, {0.02f, 0.02f, 0.03f, 1.0f});
	ui::DrawBorder(batch, pv, th.panelBorder);
	float y = pv.y + pv.h + 12.0f;
	if (m_selected.empty()) {
		m_font.Draw(batch, loc::Tr("pick.none"), kDetailX * w, y, th.textDim);
	} else {
		m_font.Draw(batch, m_selected, kDetailX * w, y, th.accent);
		y += m_font.Height() + 6.0f;
		for (const std::string& line : m_facts) {
			m_font.Draw(batch, line, kDetailX * w, y, th.textDim);
			y += m_font.Height() + 2.0f;
		}
	}

	m_ui.Render(batch, w, h); // search row, chips, Choose, close box
}

} // namespace dungeon::game
