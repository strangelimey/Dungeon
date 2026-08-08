// ============================================================================
// Game/AssetDialog.cpp — see AssetDialog.h.
// ============================================================================
#include "Game/AssetDialog.h"

#include "Assets/Image.h"
#include "Assets/Model.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/DialogLayout.h"
#include "Platform/FileDialog.h"
#include "UI/Controls.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <format>

namespace dungeon::game {

namespace {
// The mesh a texture set is previewed on: the clean (non-worn) wall block, which
// is baked for every project and reads as "a wall" at a glance.
constexpr const char* kSwatchMesh = "wall_block.gltf";

// The panel, as window fractions; the card inside it is stacked
// (Game/DialogLayout.h) — the form on the left, the preview on the right.
constexpr gfx::Rect kPanel{0.14f, 0.09f, 0.72f, 0.82f};
constexpr float kFormFill = 1.0f, kPaneFill = 0.85f, kGutterRow = 1.0f;

// Catalog ids are whitespace-tokenised in .map/.ent records and become file-safe
// asset names, so the field accepts only these (the door/level name rule).
bool IdChar(char c) {
	const unsigned char u = static_cast<unsigned char>(c);
	return std::isalnum(u) || c == '_' || c == '-';
}
} // namespace

AssetDialog::AssetDialog(gfx::GraphicsDevice& device, Window& window)
	: m_device(device), m_window(window) {
	m_ui = std::make_unique<ui::UIContext>(device, "", 18.0f);
	m_ui->Root().fontScale = ui::kDialogTextScale; // inherits — see LevelSettings
	m_closeIcon = CloseIcon(device);
}

void AssetDialog::Open(const std::string& category, const std::string& catalogKey,
					   bool textureSet, std::vector<std::string> existing,
					   const ui::Theme& theme, Source source,
					   const std::string& asset) {
	m_open = true;
	m_busy = false;
	m_uiRebuild = false;
	m_error.clear();
	m_category = category;
	m_catalogKey = catalogKey;
	m_textureSet = textureSet;
	// Preset by the type editor's Duplicate (source + the entry to copy); the
	// palette's "+ New..." passes neither and lands on Import with nothing picked.
	m_source = source;
	m_name.clear(); // never prefilled: the clone has to be told what it IS
	m_group.clear();
	m_sourcePath.clear();
	m_asset = asset;
	m_flipGreen = false;
	m_existing = std::move(existing);
	m_found = {};
	m_orbit = 0.0f;
	// Prior preview resources may still be referenced by in-flight frames.
	if (m_previewMesh || m_previewAlbedo) m_device.WaitIdle();
	m_previewMesh.reset();
	m_previewAlbedo.reset();
	m_previewNormal.reset();
	m_previewMr.reset();
	m_material = {}; // metallic 0, roughness 0.9, height 0, white
	m_neutral = m_material; // Create writes only the sliders moved off these
	m_theme = theme;
	Rebuild(theme);
	// Opened preset on an entry (the type editor's Duplicate): show it straight
	// away. Rebuild's own seeding only fires when nothing is picked yet.
	if (!m_asset.empty()) RefreshPreview();
}

gfx::Rect AssetDialog::PreviewRect(float, float) const {
	// The pane widget's own rect, from the layout that just ran — one truth for
	// the backing it draws and the model the owner blits over it.
	return m_pane ? m_pane->Pixel() : gfx::Rect{0.0f, 0.0f, 0.0f, 0.0f};
}

std::string AssetDialog::Validate() const {
	const std::string name = m_nameField ? m_nameField->text : m_name;
	if (name.empty()) return loc::Tr("newasset.err.noname");
	if (std::find(m_existing.begin(), m_existing.end(), name) != m_existing.end())
		return loc::Format("newasset.err.dup", name);
	switch (m_source) {
	case Source::Import:
		if (m_sourcePath.empty()) return loc::Tr("newasset.err.nosource");
		// A texture folder with no albedo can't be imported at all — say so here
		// rather than letting AssetBaker fail a few seconds later.
		if (m_textureSet && !m_found.Usable()) return loc::Tr("newasset.err.noalbedo");
		break;
	case Source::Installed:
		if (m_asset.empty()) return loc::Tr("newasset.err.noasset");
		break;
	case Source::Duplicate:
		if (m_asset.empty()) return loc::Tr("newasset.err.nosourcetype");
		break;
	}
	return {};
}

void AssetDialog::Rebuild(const ui::Theme& theme) {
	m_ui->SetTheme(theme);
	m_ui->Clear();
	m_nameField = nullptr;
	m_groupField = nullptr;
	m_pathLabel = nullptr;
	m_problemLabel = nullptr;
	m_pane = nullptr;

	DialogChrome chrome =
		BuildDialogChrome(*m_ui, kPanel, loc::Format("newasset.title", m_category),
						  m_closeIcon, [this] { Close(); });
	// Two columns: the form on the left, the preview and what the import found
	// on the right. Both are stacks, so neither can run into the other.
	chrome.body->horizontal = true;
	ui::Stack* form = chrome.body->Row<ui::Stack>(ui::Len::Fill(kFormFill));
	form->debugName = "form";
	form->gapRem = 0.5f;
	chrome.body->Space(ui::Len::Fixed(kGutterRow));
	ui::Stack* right = chrome.body->Row<ui::Stack>(ui::Len::Fill(kPaneFill));
	right->debugName = "preview";
	right->gapRem = 0.4f;

	// --- source mode ---------------------------------------------------------
	{
		ui::Stack* row = form->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Label>(ui::Len::Fill(0.8f), loc::Tr("newasset.source"))->centerV =
			true;
		std::vector<std::string> modes = {loc::Tr("newasset.src.import"),
										  loc::Tr("newasset.src.installed"),
										  loc::Tr("newasset.src.duplicate")};
		row->Row<ui::DropDown>(ui::Len::Fill(1.2f), modes,
							   static_cast<int>(m_source), [this](int i) {
								   m_source = static_cast<Source>(i);
								   m_asset.clear();
								   m_error.clear();
								   m_uiRebuild = true; // the form's middle changes
							   });
	}

	// --- id + group ----------------------------------------------------------
	m_nameField = form->Row<ui::TextField>(FormRow(), m_name);
	m_nameField->placeholder = loc::Tr("newasset.name");
	m_nameField->maxLength = 32;
	{
		ui::TextField* raw = m_nameField;
		raw->onChange = [this, raw] {
			std::erase_if(raw->text, [](char c) { return !IdChar(c); });
			m_name = raw->text;
		};
	}
	m_groupField = form->Row<ui::TextField>(FormRow(), m_group);
	m_groupField->placeholder = loc::Tr("newasset.group");
	m_groupField->maxLength = 24;
	{
		ui::TextField* raw = m_groupField;
		raw->onChange = [this, raw] {
			std::erase_if(raw->text, [](char c) { return !IdChar(c); });
			m_group = raw->text;
		};
	}

	// --- the source's own controls ------------------------------------------
	if (m_source == Source::Import) {
		ui::Stack* row = form->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Button>(
			ui::Len::Fill(0.9f),
			loc::Tr(m_textureSet ? "newasset.browse_folder" : "newasset.browse_model"),
			[this] { Browse(); });
		m_pathLabel = row->Row<ui::Label>(ui::Len::Fill(1.1f), loc::Tr("newasset.none"));
		m_pathLabel->centerV = true;
		m_pathLabel->dim = m_sourcePath.empty();
		if (!m_sourcePath.empty()) {
			const size_t slash = m_sourcePath.find_last_of("\\/");
			m_pathLabel->text = slash == std::string::npos ? m_sourcePath
														   : m_sourcePath.substr(slash + 1);
		}
		if (m_textureSet && !m_sourcePath.empty())
			form->Row<ui::Checkbox>(FormRow(), loc::Tr("newasset.flipgreen"),
									m_flipGreen, [this](bool on) { m_flipGreen = on; });
	} else if (m_source == Source::Installed) {
		// The POOL: hundreds of entries whose names say nothing, so this is the
		// picker's job, not a dropdown's (the type editor's asset fields the same).
		ui::Stack* row = form->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Label>(ui::Len::Fill(0.8f), loc::Tr("newasset.asset"))->centerV =
			true;
		row->Row<ui::Button>(
			ui::Len::Fill(1.2f),
			m_asset.empty() ? loc::Tr("map.type.none") : m_asset, [this] {
				if (onPickAsset)
					onPickAsset(m_textureSet, m_asset, [this](const std::string& picked) {
						m_asset = picked;
						RefreshPreview();
						m_uiRebuild = true; // the button's face is its value
					});
			});
	} else {
		// Duplicate: this catalog's own ids — a short, meaningful list.
		const std::vector<std::string>& items = m_existing;
		std::vector<std::string> shown = items;
		if (shown.empty()) shown.push_back(loc::Tr("newasset.err.noasset"));
		int sel = 0;
		for (size_t i = 0; i < items.size(); ++i)
			if (items[i] == m_asset) { sel = static_cast<int>(i); break; }
		ui::Stack* row = form->Row<ui::Stack>(FormRow(), true);
		row->gapRem = 0.5f;
		row->Row<ui::Label>(ui::Len::Fill(0.8f), loc::Tr("newasset.copyfrom"))
			->centerV = true;
		row->Row<ui::DropDown>(ui::Len::Fill(1.2f), shown, sel,
							   [this, items](int i) {
								   if (i >= 0 && i < static_cast<int>(items.size())) {
									   m_asset = items[static_cast<size_t>(i)];
									   RefreshPreview();
								   }
							   });
		// Seed the pick so a single-entry list isn't silently "unset".
		if (m_asset.empty() && !items.empty()) {
			m_asset = items.front();
			RefreshPreview();
		}
	}

	// --- material ------------------------------------------------------------
	form->Row<ui::Separator>(ui::Len::Fixed(0.6f));
	form->Row<ui::Slider>(FormRow(1.9f), loc::Tr("newasset.metallic"), 0.0f, 1.0f,
						  m_material.metallic,
						  [this](float v) { m_material.metallic = v; });
	form->Row<ui::Slider>(FormRow(1.9f), loc::Tr("newasset.roughness"), 0.0f, 1.0f,
						  m_material.roughness,
						  [this](float v) { m_material.roughness = v; });
	form->Row<ui::Slider>(FormRow(1.9f), loc::Tr("newasset.height"), 0.0f, 0.1f,
						  m_material.heightScale,
						  [this](float v) { m_material.heightScale = v; });
	form->Row<ui::ColorPicker>(FormRow(), loc::Tr("newasset.color"),
							   m_material.baseColor,
							   [this](const Vec4& c) { m_material.baseColor = c; });
	form->Space(ui::Len::Fill()); // the rows sit at the top of the column

	// Why Create is refused (or the last bake failure). Its text is rewritten
	// every frame in Update: the reason depends on what is typed, which does not
	// rebuild the tree.
	m_problemLabel = chrome.footer->Row<ui::Label>(ui::Len::Fill(), "");
	m_problemLabel->centerV = true;
	chrome.footer->Row<ui::Button>(FooterButton(), loc::Tr("newasset.create"), [this] {
							  if (!Validate().empty()) return; // the label says why
							  CreateRequest req;
							  req.category = m_category;
							  req.catalogKey = m_catalogKey;
							  req.textureSet = m_textureSet;
							  req.source = m_source;
							  req.name = m_nameField ? m_nameField->text : m_name;
							  req.group = m_groupField ? m_groupField->text : m_group;
							  req.sourcePath = m_sourcePath;
							  req.asset = m_asset;
							  req.flipGreen = m_flipGreen;
							  req.material = m_material;
							  // Only sliders moved off their opening values persist to
							  // the catalog (see CreateRequest) — an untouched form
							  // leaves an imported model's own maps authoritative.
							  auto moved = [](float a, float b) {
								  return std::abs(a - b) > 1e-4f;
							  };
							  req.metallicSet = moved(m_material.metallic, m_neutral.metallic);
							  req.roughnessSet =
								  moved(m_material.roughness, m_neutral.roughness);
							  req.heightSet =
								  moved(m_material.heightScale, m_neutral.heightScale);
							  req.colorSet =
								  moved(m_material.baseColor.x, m_neutral.baseColor.x) ||
								  moved(m_material.baseColor.y, m_neutral.baseColor.y) ||
								  moved(m_material.baseColor.z, m_neutral.baseColor.z) ||
								  moved(m_material.baseColor.w, m_neutral.baseColor.w);
							  m_error.clear();
							  if (onCreate) onCreate(req);
							  // An import stays open in the busy state; the other
							  // sources are done the moment the catalog is written.
							  if (!m_busy) Close();
						  });

	// Right column: the preview, then what the import found in the folder — one
	// row per recognised map, with the loud cases (no albedo = can't import; no
	// height = flat parallax) called out. They used to be drawn from a y cursor
	// running down from the pane's bottom edge, off the end of the panel if the
	// list ever grew.
	m_pane = right->Row<PreviewPane>(ui::Len::Fill());
	m_pane->border = true;
	if (m_source == Source::Import && m_textureSet && !m_sourcePath.empty()) {
		const auto row = [&](const char* labelKey, const std::string& path, bool warn) {
			const std::string name =
				path.empty() ? loc::Tr("newasset.map.missing")
							 : std::filesystem::path(path).filename().string();
			ui::Label* l =
				right->Row<ui::Label>(FormRow(0.8f), loc::Tr(labelKey) + ": " + name);
			l->dim = path.empty() && !warn;
			l->accent = path.empty() && warn;
		};
		row("newasset.map.albedo", m_found.albedo, true);
		row("newasset.map.normal", m_found.normal, false);
		row("newasset.map.height", m_found.height, true);
		row("newasset.map.rough", m_found.roughness, false);
		row("newasset.map.ao", m_found.ao, false);
		if (m_found.height.empty())
			right->Row<ui::Label>(FormRow(0.8f), loc::Tr("newasset.warn.noheight"))
				->accent = true;
	}
}

void AssetDialog::Browse() {
	const std::string path =
		m_textureSet
			? platform::PickFolder(m_window.Handle())
			: platform::PickFile(m_window.Handle(), L"3D models", L"*.gltf;*.glb;*.obj");
	if (path.empty()) return;
	m_sourcePath = path;
	m_error.clear();
	if (m_textureSet) {
		// What the baker will make of this folder, decided by the SAME code the
		// baker runs (Assets/PbrMaps.h) — reported before committing to a bake.
		m_found = assets::DiscoverPbrMaps(path);
		m_flipGreen = m_found.normalLooksGl; // pre-tick what the name implies
	}
	// A default name from the folder/file stem, so the common case is one click.
	if (m_name.empty()) {
		std::string stem = std::filesystem::path(path).stem().string();
		std::erase_if(stem, [](char c) { return !IdChar(c); });
		std::ranges::transform(stem, stem.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		m_name = stem;
	}
	RefreshPreview();
	m_uiRebuild = true; // the path label, flip-green row and name all changed
}

void AssetDialog::RefreshPreview() {
	// The old mesh/textures may still be referenced by in-flight frames.
	if (m_previewMesh || m_previewAlbedo) m_device.WaitIdle();
	m_previewMesh.reset();
	m_previewAlbedo.reset();
	m_previewNormal.reset();
	m_previewMr.reset();
	m_material.albedo = nullptr;
	m_material.normalMap = nullptr;
	m_material.metalRough = nullptr;

	// The pool asset to show. Installed picks one directly; Duplicate picks a
	// catalog ID, whose entry names it — resolve that, or a clone previews
	// nothing at all (which is exactly how it looked).
	const std::string pool =
		m_source == Source::Duplicate
			? (fieldOfType ? fieldOfType(m_catalogKey, m_asset,
										 m_textureSet ? "texture" : "model")
						   : std::string())
			: m_asset;

	// --- a texture set: the shared block mesh, wearing those maps -------------
	if (m_textureSet) {
		std::string albedoPath, normalPath;
		if (m_source != Source::Import && !pool.empty()) {
			// Installed sets are baked: load by stem at 2k (always present).
			const std::string stem = paths::Asset("textures\\" + pool + "_2k");
			m_previewAlbedo = TryLoadTextureFile(m_device, stem, /*srgb*/ true);
			m_previewNormal = TryLoadTextureFile(m_device, stem + "_n");
			m_previewMr = TryLoadTextureFile(m_device, stem + "_mr");
		} else if (m_source == Source::Import && m_found.Usable()) {
			// A download folder: the loose source images, straight off disk. The
			// packed maps don't exist yet — this is the point of previewing.
			if (auto img = assets::LoadImageFile(m_found.albedo))
				m_previewAlbedo = std::make_unique<gfx::Texture>(m_device, *img, true);
			if (!m_found.normal.empty())
				if (auto img = assets::LoadImageFile(m_found.normal))
					m_previewNormal = std::make_unique<gfx::Texture>(m_device, *img, false);
		}
		if (!m_previewAlbedo) return;
		auto model = assets::LoadModel(paths::Asset(std::string("models\\") + kSwatchMesh));
		if (!model || model->meshes.empty()) {
			log::Warn("asset preview: {} is missing", kSwatchMesh);
			return;
		}
		m_previewModel = std::move(*model);
		m_previewMesh = std::make_unique<gfx::Mesh>(m_device, m_previewModel.meshes[0]);
		m_material.albedo = m_previewAlbedo.get();
		m_material.normalMap = m_previewNormal.get();
		m_material.metalRough = m_previewMr.get();
		return;
	}

	// --- a model -------------------------------------------------------------
	std::string modelPath;
	if (m_source == Source::Import) modelPath = m_sourcePath;
	else if (!pool.empty()) {
		// A pool model is named without its extension (InstalledModels strips it)
		// and the bought/authored ones are .glb, so try both rather than assuming.
		modelPath = paths::Asset("models\\" + pool + ".gltf");
		if (!std::filesystem::exists(modelPath))
			modelPath = paths::Asset("models\\" + pool + ".glb");
	}
	if (modelPath.empty()) return;
	auto model = assets::LoadModel(modelPath);
	if (!model || model->meshes.empty()) {
		// .obj and some packs only load once the baker has normalized them.
		log::Warn("asset preview: could not load {}", modelPath);
		return;
	}
	m_previewModel = std::move(*model);
	m_previewMesh = std::make_unique<gfx::Mesh>(m_device, m_previewModel.meshes[0]);
	if (!m_previewModel.materials.empty()) {
		m_material.baseColor = m_previewModel.materials[0].baseColorFactor;
		// The seeded color is the model's own — neutral follows, so it only
		// persists to the catalog if the user then changes it.
		m_neutral.baseColor = m_material.baseColor;
	}
}

void AssetDialog::Update(const Input& input, float width, float height, float dt) {
	if (!m_open) return;
	m_ui->GetFont().Commit(); // flush glyphs cached last frame, before this frame draws
	m_orbit += dt * 0.6f;
	if (m_uiRebuild) { // deferred from a widget callback (Clear kills the caller)
		m_uiRebuild = false;
		Rebuild(m_theme);
	}
	if (m_busy) return; // a bake is running — ignore form input until it finishes
	m_ui->Update(input, width, height);
	// The refusal reason follows what is TYPED, which does not rebuild the tree.
	if (m_problemLabel) {
		const std::string problem = m_error.empty() ? Validate() : m_error;
		m_problemLabel->text = problem;
		m_problemLabel->accent = !m_error.empty();
		m_problemLabel->dim = m_error.empty();
	}
}

void AssetDialog::Render(gfx::SpriteBatch& batch, float width, float height) {
	if (!m_open) return;
	const ui::Theme& th = m_ui->GetTheme();
	batch.DrawRect({0, 0, width, height}, {0, 0, 0, 0.6f}); // dim the editor behind

	const gfx::Rect panel{kPanel.x * width, kPanel.y * height, kPanel.w * width,
						  kPanel.h * height};
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);
	// Title, form, preview pane, map report and footer are all widgets; the
	// owner blits the rendered model into PreviewRect afterwards.
	m_ui->Render(batch, width, height);

	ui::Font& font = m_ui->GetFont();

	// While baking, freeze the form behind a notice (the owner runs AssetBaker).
	if (m_busy) {
		batch.DrawRect(panel, {0.0f, 0.0f, 0.0f, 0.55f});
		const std::string msg = loc::Tr("newasset.baking");
		font.Draw(batch, msg, panel.x + (panel.w - font.MeasureWidth(msg)) * 0.5f,
				  panel.y + panel.h * 0.5f - font.Height() * 0.5f, th.accent);
	}
}

} // namespace dungeon::game
