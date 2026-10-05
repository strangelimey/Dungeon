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
#include "Game/DialogLayout.h"
#include "Game/DungeonWorld.h" // kIconSize: an icon target must match the bake
#include "UI/Controls.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>

namespace dungeon::game {

namespace {
// The panel as a window fraction — a large card, since the whole point is to
// see many assets at once. The card inside it is stacked (Game/DialogLayout.h);
// the grid gets a reserved area from that stack and everything about a tile is
// measured off THAT, not off the window.
constexpr gfx::Rect kPanel{0.08f, 0.07f, 0.84f, 0.86f};
constexpr float kGridFill = 1.0f, kDetailFill = 0.62f, kGutterRow = 1.0f;

// The grid: four tiles across, each row a horizontal stack inside a
// content-sized vertical one, all inside a ScrollArea (UI/Layout.h). A tile's
// height is TYPE — it holds an image over two lines of text — so it is rem like
// every other row extent; its width is whatever a quarter of the grid is.
constexpr int kCols = 4;
constexpr float kTileRow = 9.0f;  // rem: image + name + badge
constexpr float kTileGap = 0.45f; // rem, between tiles and between rows
// Work per frame, so opening the picker draws immediately and fills in behind
// itself: a .dds read plus its upload drains the GPU, and doing sixteen of them
// in the first frame is what made the dialog take a beat to appear.
constexpr size_t kThumbLoadsPerFrame = 2;
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

AssetPicker::AssetPicker(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
	: m_device(device), m_ui(fonts, ui::FontRole::Body, 18.0f), m_thumbs(device, kThumbCap) {
	m_ui.Root().fontScale = ui::kDialogTextScale; // inherits — see LevelSettings
	m_closeIcon = CloseIcon(device);
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
	m_lastClickTile = -1;
	m_theme = theme;
	m_items = mode == Mode::Textures ? InstalledTextureSetInfo() : InstalledModelInfo();
	// A model is drawn in its bound set; which sets exist (and at what size) is
	// one walk here rather than a probe per tile.
	m_setRes.clear();
	if (mode == Mode::Models)
		for (const AssetInfo& set : InstalledTextureSetInfo())
			m_setRes.emplace(set.name, set.resolutions);
	m_used = usedAssets ? usedAssets() : std::vector<std::string>{};
	// Thumbnails are per-pool: a model tile and a texture tile of the same name
	// are different images, and the cache is keyed by name alone.
	m_thumbs.Clear();
	ApplyFilter();
	Rebuild();
	m_tilesDirty = false; // ApplyFilter asks for a refill; Rebuild WAS it
	// The preview and the facts are the expensive part; they land next frame so
	// the dialog itself appears at once (LoadVisibleThumbs does the same for the
	// tiles). Opening used to do all of it up front, and it showed.
	m_previewDirty = true;
	m_factsDirty = true;
	// Scroll the current value into view: opening a hundred-tile grid at the top
	// when the field already names one is the dropdown's failing all over again.
	// Deferred to the first Update, because "into view" needs a view — the tiles
	// have no pixel rects until the tree has been laid out once.
	m_scrollToSelected = true;
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
	// The tiles ARE the filter's result. Only they are refilled, and deferred at
	// that — this runs from the search field's own callback, and clearing a
	// subtree that is mid-walk is the one thing the tree forbids. A new result
	// set starts at the top.
	m_tilesDirty = true;
	if (m_grid) m_grid->ScrollToTop();
	// The count follows the filter, not the frame — reformatting it every frame
	// would allocate a string in a draw path for a number that rarely changes.
	if (m_countLabel)
		m_countLabel->text =
			loc::Format("pick.count", m_shown.size(), m_items.size());
}

// One tile per shown asset, four to a row. Only the grid's rows are touched —
// the search field, the chips and the details column stand, which is what lets
// the filter change on every keystroke without the field losing focus.
void AssetPicker::RebuildTiles() {
	if (!m_tileRows) return;
	m_tileRows->ClearRows();
	m_tiles.clear();
	m_tiles.reserve(m_shown.size());
	ui::Stack* row = nullptr;
	for (size_t i = 0; i < m_shown.size(); ++i) {
		if (i % kCols == 0) {
			row = m_tileRows->Row<ui::Stack>(ui::Len::Fixed(kTileRow), true);
			row->gapRem = kTileGap;
		}
		m_tiles.push_back(row->Row<AssetTile>(ui::Len::Fill(), *this, i));
	}
	// Pad the last row so a partial one keeps its tiles the same width as the
	// full rows above it.
	if (row)
		for (size_t i = m_shown.size() % kCols; i > 0 && i < kCols; ++i)
			row->Space(ui::Len::Fill());
}

// --- the tile ----------------------------------------------------------------

const std::string& AssetPicker::AssetTile::Name() const {
	static const std::string kNone;
	if (m_index >= m_owner.m_shown.size()) return kNone;
	return m_owner.m_items[m_owner.m_shown[m_index]].name;
}

void AssetPicker::AssetTile::UpdateSelf(ui::UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	m_hot = false;
	if (!input || ctx.IsMouseConsumed()) return;
	if (!Pixel().Contains(input->MouseX(), input->MouseY())) return;
	m_hot = true;
	ctx.ConsumeMouse(); // the tile owns the pointer over itself — but NOT the
						// wheel, which belongs to the grid it sits in
	if (input->WasMousePressed(MouseButton::Left)) m_owner.OnTileClicked(m_index);
}

void AssetPicker::AssetTile::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const std::string& name = Name();
	if (name.empty()) return; // the filter moved past this slot
	const ui::Theme& th = ctx.GetTheme();
	const ui::Font& font = TextFont();
	const gfx::Rect& px = Pixel();
	const bool picked = name == m_owner.m_selected;
	batch.DrawRect(px, m_hot || picked ? th.controlHot : th.control);

	// The image is the tile's top square, but never taller than what the two
	// text lines leave — a square that fills the tile pushes them out of it.
	const float inset = Rem(0.22f);
	const float textH = 2.0f * (font.Height() + Rem(0.15f));
	const float side = std::min(px.w - 2 * inset, px.h - 2 * inset - textH);
	const gfx::Rect img{px.x + (px.w - side) * 0.5f, px.y + inset, side, side};
	if (const gfx::Texture* thumb = m_owner.ThumbFor(name))
		batch.DrawSprite(img, {0, 0, 1, 1}, *thumb, {1, 1, 1, 1});
	else
		batch.DrawRect(img, {0.10f, 0.10f, 0.12f, 1.0f});

	const AssetInfo& a = m_owner.m_items[m_owner.m_shown[m_index]];
	const float ty = img.y + img.h + Rem(0.15f);
	font.Draw(batch, name, px.x + inset, ty, picked ? th.accent : th.text);
	const std::string badge = m_owner.m_mode == Mode::Textures
								  ? ResBadge(a.resolutions)
								  : (a.file.ends_with(".glb") ? "glb" : "gltf");
	font.Draw(batch, badge, px.x + inset, ty + font.Height() + Rem(0.05f), th.textDim);
	if (picked) ui::DrawBorder(batch, px, th.accent);
}

void AssetPicker::OnTileClicked(size_t shownIndex) {
	if (m_searchField) m_searchField->SetFocused(false); // typing goes nowhere now
	const int index = static_cast<int>(shownIndex);
	const bool again = index == m_lastClickTile && m_time - m_lastClickTime < 0.4;
	SelectIndex(index);
	m_lastClickTile = index;
	m_lastClickTime = m_time;
	if (again && !m_selected.empty()) {
		const std::string picked = m_selected;
		Close();
		if (onChoose) onChoose(picked);
	}
}

// The tiles inside the grid's view — what the deferred loaders work through, so
// an off-screen tile costs nothing until it is scrolled to. Taken from the
// tiles' own rects: the grid's geometry lives in the layout now, and asking it
// beats keeping a second derivation of it here.
std::vector<AssetPicker::AssetTile*> AssetPicker::VisibleTiles() const {
	std::vector<AssetTile*> out;
	if (!m_grid) return out;
	const gfx::Rect view = m_grid->Pixel();
	for (AssetTile* tile : m_tiles) {
		const gfx::Rect& px = tile->Pixel();
		if (px.h <= 0.0f) continue;
		if (px.y + px.h > view.y && px.y < view.y + view.h) out.push_back(tile);
	}
	return out;
}

// --- thumbnails --------------------------------------------------------------

std::unique_ptr<gfx::Texture> AssetPicker::LoadThumb(const std::string& name) const {
	if (m_mode != Mode::Textures) return nullptr; // models are baked by the owner
	const auto it = std::ranges::find(m_items, name, &AssetInfo::name);
	if (it == m_items.end()) return nullptr;
	// The smallest installed resolution, trimmed to a tile (AssetUtil's
	// LoadTextureThumb, which the editor's surface swatches share).
	return LoadTextureThumb(
		m_device, paths::Asset("textures\\" + name + SmallestRes(it->resolutions)),
		kThumbPx);
}

const gfx::Texture* AssetPicker::ThumbFor(const std::string& name) {
	// DRAW-TIME ONLY: this never loads. Both kinds of tile image are made in
	// Update — textures by LoadVisibleThumbs, models by PrepareModelIcons plus
	// the owner's bake — so a frame's drawing costs nothing but the blit.
	return m_thumbs.Touch(name).data.texture.get();
}

void AssetPicker::LoadVisibleThumbs(size_t max) {
	if (m_mode != Mode::Textures) return;
	size_t loaded = 0;
	for (const AssetTile* tile : VisibleTiles()) {
		if (loaded >= max) break;
		const std::string& name = tile->Name();
		if (name.empty()) continue;
		auto* slot = m_thumbs.BeginLoad(name); // one attempt per set, however it goes
		if (!slot) continue;
		slot->data.texture = LoadThumb(name);
		++loaded;
	}
}

void AssetPicker::PrepareModelIcons(size_t max) {
	if (m_mode != Mode::Models) return;
	// Drop the sources of bakes every frame in flight has finished with: once a
	// frame slot has come round again (kFrameCount Ticks), the frame that
	// recorded the draw is done, and the tile needs only its image.
	m_thumbs.ForEach([&](const std::string&, auto& entry) {
		Thumb& t = entry.data;
		if (t.look && !t.needsBake && m_thumbs.Frame() > t.bakedAt + gfx::kFrameCount) {
			t.look.reset();
			t.palette = {};
		}
	});
	size_t made = 0;
	for (const AssetTile* tile : VisibleTiles()) {
		if (made >= max) break;
		if (tile->Name().empty()) continue;
		const AssetInfo& a = m_items[m_shown[tile->ShownIndex()]];
		auto* entry = m_thumbs.BeginLoad(a.name); // one attempt per asset, however it goes
		if (!entry) continue;
		Thumb& slot = entry->data;
		// The whole model as the preview shows it, its bound set trimmed to a
		// tile. The baked image sidecars load, not the PNGs inside the file: a
		// bought rig's six 2k decodes took long enough that a screenful of its
		// tiles was still blank seconds after opening.
		slot.look = LoadLook(a.name, a.file, DungeonWorld::kIconSize);
		if (!slot.look) {
			log::Warn("asset picker: no icon for {} (could not load)", a.file);
			continue;
		}
		// A rigged model's tile is its idle's first frame, fitted to that pose.
		if (slot.look->data && !slot.look->idleClip.empty()) {
			anim::Animator pose(&slot.look->data->skeleton, &slot.look->data->clips);
			if (pose.Play(slot.look->idleClip, /*loop*/ false)) {
				pose.Update(0.0f);
				slot.palette = pose.Palette();
				slot.look->FitToPose(slot.palette);
			}
		}
		// The bake's viewport and shared depth target are DungeonWorld::kIconSize,
		// so a target we create ourselves has to be exactly that.
		slot.texture = gfx::Texture::RenderTarget(m_device, DungeonWorld::kIconSize);
		slot.needsBake = true;
		++made;
	}
}

std::vector<AssetPicker::PendingBake> AssetPicker::PendingBakes(size_t max) const {
	std::vector<PendingBake> out;
	m_thumbs.ForEach([&](const std::string& name, const auto& entry) {
		const Thumb& thumb = entry.data;
		if (out.size() >= max) return;
		if (!thumb.needsBake || !thumb.look || !thumb.texture) return;
		out.push_back({name, thumb.look->parts, thumb.texture.get(), thumb.look->lo,
					   thumb.look->hi, thumb.palette,
					   thumb.look->ViewTilt(0.3f), thumb.look->ViewYaw(0.5f)});
	});
	return out;
}

void AssetPicker::MarkBaked(const std::string& name) {
	if (auto* entry = m_thumbs.Find(name)) {
		entry->data.needsBake = false;
		entry->data.bakedAt = m_thumbs.Frame(); // its source goes once the GPU is done
	}
}

// --- selection, preview, facts ----------------------------------------------

void AssetPicker::SelectIndex(int shownIndex) {
	if (shownIndex < 0 || shownIndex >= static_cast<int>(m_shown.size())) return;
	m_selected = m_items[m_shown[static_cast<size_t>(shownIndex)]].name;
	// Deferred to the next Update: clicking through the grid should feel instant,
	// and a click that loads three textures and decodes a normal map does not.
	m_previewDirty = true;
	m_factsDirty = true;
}

gfx::Rect AssetPicker::PreviewRect(float, float) const {
	return m_pane ? m_pane->Pixel() : gfx::Rect{0.0f, 0.0f, 0.0f, 0.0f};
}

void AssetPicker::RefreshPreview() {
	// In-flight frames may still reference the old resources (the SRV rule).
	if (m_previewMesh || m_previewAlbedo || m_previewLook) m_device.WaitIdle();
	m_previewMesh.reset();
	m_previewAlbedo.reset();
	m_previewNormal.reset();
	m_previewMr.reset();
	m_previewAnim = {}; // borrows the look's skeleton: goes first
	m_previewPosed = false;
	m_previewLook.reset();
	m_previewParts.clear();
	m_previewWall = false;
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
		gfx::MaterialParams material;
		material.albedo = m_previewAlbedo.get();
		material.normalMap = m_previewNormal.get();
		material.metalRough = m_previewMr.get();
		m_previewParts.push_back({m_previewMesh.get(), material});
		m_fitLo = {1e9f, 1e9f, 1e9f};
		m_fitHi = {-1e9f, -1e9f, -1e9f};
		for (const assets::Vertex& v : m_previewModel.meshes[0].vertices) {
			m_fitLo = {std::min(m_fitLo.x, v.position.x), std::min(m_fitLo.y, v.position.y),
					   std::min(m_fitLo.z, v.position.z)};
			m_fitHi = {std::max(m_fitHi.x, v.position.x), std::max(m_fitHi.y, v.position.y),
					   std::max(m_fitHi.z, v.position.z)};
		}
		m_stands = true;
		m_viewTilt = 0.0f;
		return;
	}

	// A model previews ALL of itself as the world draws it: every primitive with
	// its own material and embedded maps, plus the set a catalog binds to it on
	// whatever it does not texture itself. It used to be meshes[0] alone with a
	// blank material - a bought skeleton came out as one white shoulder plate.
	const auto it = std::ranges::find(m_items, m_selected, &AssetInfo::name);
	const std::string file = it == m_items.end() ? m_selected + ".gltf" : it->file;
	m_previewLook = LoadLook(m_selected, file, 0);
	if (!m_previewLook) {
		log::Warn("asset picker: could not load {}", file);
		return;
	}
	m_previewParts = m_previewLook->parts;
	// A rigged model stands in its idle and breathes, as it will in the world,
	// fitted to that pose (the T-pose's arms would leave it small in the pane).
	if (m_previewLook->data && !m_previewLook->idleClip.empty()) {
		m_previewAnim =
			anim::Animator(&m_previewLook->data->skeleton, &m_previewLook->data->clips);
		m_previewPosed = m_previewAnim.Play(m_previewLook->idleClip, /*loop*/ true);
		m_previewAnim.Update(0.0f);
		if (m_previewPosed) m_previewLook->FitToPose(m_previewAnim.Palette());
	}
	m_fitLo = m_previewLook->lo;
	m_fitHi = m_previewLook->hi;
	// A thing in the floor is looked down into on a turntable; a flat thing
	// tumbles; anything else (or anything rigged) turns upright.
	m_viewTilt = m_previewLook->ViewTilt(0.0f);
	m_stands = m_viewTilt != 0.0f || !m_previewLook->Flat(); // looked at from above / below
	// A wall feature or a backed panel: keep to its face.
	m_previewWall = m_previewLook->backed ||
					m_previewLook->mount == DungeonWorld::PoolModelLook::Mount::Wall;
}

std::unique_ptr<DungeonWorld::PoolModelLook> AssetPicker::LoadLook(const std::string& modelName,
																   const std::string& file,
																   u32 thumbPx) {
	auto look = DungeonWorld::LoadPoolModelLook(
		m_device, paths::Asset("models\\" + file), SetStemFor(modelName), thumbPx,
		idleClipFor ? idleClipFor(modelName) : std::string(), MountOf(modelName),
		liquidFor ? liquidFor(modelName) : nullptr);
	if (!look) return look;
	// Slenderness is the SUBJECT's, measured before any context widens it.
	const bool slender = thumbPx > 0 && look->Slender();
	if (contextFor) {
		const std::string context = contextFor(modelName);
		const auto it = context.empty() ? m_items.end()
										: std::ranges::find(m_items, context, &AssetInfo::name);
		if (it != m_items.end()) { // the leaf / mount, when installed
			auto ctx = DungeonWorld::LoadPoolModelLook(
				m_device, paths::Asset("models\\" + it->file), SetStemFor(context), thumbPx,
				{}, MountOf(context));
			if (ctx) look->AddContext(std::move(*ctx));
		}
	}
	if (slender) look->FrameSlenderTop();
	return look;
}

std::string AssetPicker::SetStemFor(const std::string& modelName) const {
	const std::string set = textureFor ? textureFor(modelName) : modelName;
	const auto it = m_setRes.find(set);
	if (set.empty() || it == m_setRes.end()) return {};
	return paths::Asset("textures\\" + set + SmallestRes(it->second));
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
	// A rebuild destroys the grid, and a fresh ScrollArea starts at the top.
	// Selecting a tile rebuilds (its facts are Label rows), so without this the
	// grid jumped home on every click. Handed back a frame later — see
	// m_restoreScroll.
	if (m_grid) m_restoreScroll = m_grid->Scroll();
	m_ui.Clear();
	m_searchField = nullptr;
	m_grid = nullptr;
	m_tileRows = nullptr;
	m_tiles.clear();
	m_countLabel = nullptr;
	m_pane = nullptr;
	m_nameLabel = nullptr;

	DialogChrome chrome =
		BuildDialogChrome(m_ui, kPanel, loc::Format("pick.title", m_label),
						  m_closeIcon, [this] { Close(); });
	// Two columns: the filter row over the tile grid, and the details beside.
	chrome.body->horizontal = true;
	ui::Stack* left = chrome.body->Row<ui::Stack>(ui::Len::Fill(kGridFill));
	left->debugName = "grid-column";
	left->gapRem = 0.5f;
	chrome.body->Space(ui::Len::Fixed(kGutterRow));
	ui::Stack* right = chrome.body->Row<ui::Stack>(ui::Len::Fill(kDetailFill));
	right->debugName = "details";
	right->gapRem = 0.4f;

	// --- filter row ----------------------------------------------------------
	ui::Stack* filter = left->Row<ui::Stack>(FormRow(), true);
	filter->gapRem = 0.5f;
	m_searchField = filter->Row<ui::TextField>(ui::Len::Fill(1.6f), m_search);
	m_searchField->placeholder = loc::Tr("pick.search");
	m_searchField->maxLength = 40;
	ui::TextField* raw = m_searchField;
	raw->onChange = [this, raw] {
		m_search = raw->text;
		ApplyFilter();
	};
	RowIcon(*filter, m_device, "clear", loc::Tr("map.btn.clear"), [this] {
		m_search.clear();
		ApplyFilter();
		m_uiRebuild = true; // the field's text is its own state — rebuild it
	});
	filter->Row<ui::Checkbox>(ui::Len::Fill(0.9f), loc::Tr("pick.chip.used"),
							  m_onlyUsed, [this](bool on) {
								  m_onlyUsed = on;
								  ApplyFilter();
							  });
	if (m_mode == Mode::Textures)
		filter->Row<ui::Checkbox>(ui::Len::Fill(1.0f), loc::Tr("pick.chip.surface"),
								  m_onlySurface, [this](bool on) {
									  m_onlySurface = on;
									  ApplyFilter();
								  });
	// The count ends the filter row — it IS what the filter left, and it belongs
	// beside the controls that produced it rather than up beside the close box,
	// where it had nothing but the panel's corner to sit against.
	m_countLabel = filter->Row<ui::Label>(
		ui::Len::Fill(0.9f),
		loc::Format("pick.count", m_shown.size(), m_items.size()));
	m_countLabel->centerV = true;
	m_countLabel->dim = true;

	// The grid: a ScrollArea (scroll, clip, scrollbar, wheel — all of it the
	// shared one now) over a content-sized Stack of tile rows. The picker used
	// to draw its own tiles from a row-snapped scroll of its own, hit-test them
	// with its own TileAt, and drag its own thumb.
	m_grid = left->Row<ui::ScrollArea>(ui::Len::Fill());
	m_grid->debugName = "grid";
	m_tileRows = m_grid->Add<ui::Stack>(gfx::Rect{0, 0, 1, 1});
	m_tileRows->fitContent = true;
	m_tileRows->gapRem = kTileGap;
	RebuildTiles();

	// --- details column ------------------------------------------------------
	m_pane = right->Row<PreviewPane>(ui::Len::Fill());
	m_pane->border = true;
	m_nameLabel = right->Row<ui::Label>(
		FormRow(0.9f), m_selected.empty() ? loc::Tr("pick.none") : m_selected);
	m_nameLabel->accent = !m_selected.empty();
	m_nameLabel->dim = m_selected.empty();
	// The facts are rebuilt with the selection (SelectIndex defers a rebuild),
	// so they can be plain rows rather than something redrawn every frame.
	for (const std::string& line : m_facts)
		right->Row<ui::Label>(FormRow(0.8f), line)->dim = true;

	chrome.footer->Space(ui::Len::Fill());
	FooterIcon(*chrome.footer, m_device, "check", loc::Tr("pick.choose"), [this] {
		if (m_selected.empty()) return;
		const std::string picked = m_selected;
		Close();
		if (onChoose) onChoose(picked);
	});
}

// --- input -------------------------------------------------------------------

void AssetPicker::Update(const Input& input, float w, float h, float dt) {
	if (!m_open) return;
	m_thumbs.Tick();
	m_time += dt;
	m_orbit += dt * 0.6f;
	if (m_previewPosed) m_previewAnim.Update(dt);
	// One font now: the title text used to be a second Font at the very same
	// size as this context's. GameUI::UpdateFonts commits every library font
	// once per frame, so there is nothing to flush here either.
	const float fh = std::clamp(h * 0.020f, 12.0f, 24.0f);
	m_ui.UseFont(ui::FontRole::Body, fh);

	if (m_uiRebuild) {
		m_uiRebuild = false;
		Rebuild();
	} else if (m_tilesDirty) { // the cheaper half: the grid's rows alone
		m_tilesDirty = false;
		RebuildTiles();
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
	if (!m_open) return; // a tile's double-click chose and closed us

	// Scroll fix-ups, both AFTER the layout for the reason in the header: the
	// grid's height is only known once its content-sized Stack has run.
	if (m_restoreScroll >= 0.0f && m_grid) {
		m_grid->SetScroll(m_restoreScroll);
		m_restoreScroll = -1.0f;
	}
	// Opening on the current value: bring its tile into view now that the tiles
	// HAVE a view to be brought into. Once only — after this the user's scroll
	// is the user's.
	if (m_scrollToSelected && m_grid) {
		m_scrollToSelected = false;
		for (AssetTile* tile : m_tiles)
			if (tile->Name() == m_selected) {
				m_grid->ScrollIntoView(*tile);
				break;
			}
	}

	// The deferred work, at most one job a frame and cheapest first: tile images,
	// then the selected asset's preview, then its facts (which decode a normal
	// map). Everything here reads a file and touches the GPU, so spreading it is
	// what keeps opening the picker — and clicking through it — immediate.
	LoadVisibleThumbs(kThumbLoadsPerFrame);
	PrepareModelIcons(kThumbLoadsPerFrame);
	if (m_previewDirty) {
		m_previewDirty = false;
		RefreshPreview();
	} else if (m_factsDirty) {
		m_factsDirty = false;
		RefreshFacts();
		// The facts are Label rows, so a new set is a new tree — rebuilt here,
		// outside the widget walk, which is where they were about to be read.
		m_uiRebuild = true;
	}
	m_thumbs.Evict();
}

// --- drawing -----------------------------------------------------------------

void AssetPicker::Render(gfx::SpriteBatch& batch, float w, float h) {
	if (!m_open) return;
	const ui::Theme& th = m_ui.GetTheme();
	batch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.6f});
	const gfx::Rect panel{kPanel.x * w, kPanel.y * h, kPanel.w * w, kPanel.h * h};
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);
	// Everything — title, count, filter row, the tile grid, preview, facts and
	// footer — is a widget now. The owner blits the 3D preview afterwards.
	m_ui.Render(batch, w, h);
}

} // namespace dungeon::game
