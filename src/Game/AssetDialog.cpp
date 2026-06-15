// ============================================================================
// Game/AssetDialog.cpp — see AssetDialog.h.
// ============================================================================
#include "Game/AssetDialog.h"

#include "Assets/Model.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Platform/FileDialog.h"
#include "UI/Controls.h"

#include <algorithm>

namespace dungeon::game {

AssetDialog::AssetDialog(gfx::GraphicsDevice& device, Window& window)
	: m_device(device), m_window(window) {
	m_ui = std::make_unique<ui::UIContext>(device, "", 18.0f);
}

void AssetDialog::Open(const std::string& category, bool textureSet,
					   const ui::Theme& theme) {
	m_open = true;
	m_category = category;
	m_textureSet = textureSet;
	m_name.clear();
	m_sourcePath.clear();
	m_orbit = 0.0f;
	m_previewMesh.reset();
	m_material = {}; // metallic 0, roughness 0.9, height 0, white
	Rebuild(theme);
}

gfx::Rect AssetDialog::PreviewRect(float w, float h) const {
	const float side = std::min(0.30f * w, 0.50f * h);
	return {0.55f * w, 0.22f * h, side, side};
}

void AssetDialog::Rebuild(const ui::Theme& theme) {
	m_ui->SetTheme(theme);
	m_ui->Clear();
	m_nameField = nullptr;
	m_pathLabel = nullptr;

	m_ui->Add<ui::Label>(gfx::Rect{0.18f, 0.14f, 0.64f, 0.05f},
						 loc::Format("newasset.title", m_category));

	m_nameField = m_ui->Add<ui::TextField>(gfx::Rect{0.18f, 0.24f, 0.30f, 0.045f});
	m_nameField->placeholder = loc::Tr("newasset.name");

	m_ui->Add<ui::Button>(
		gfx::Rect{0.18f, 0.31f, 0.16f, 0.045f},
		loc::Tr(m_textureSet ? "newasset.browse_folder" : "newasset.browse_model"),
		[this] { Browse(); });
	m_pathLabel = m_ui->Add<ui::Label>(gfx::Rect{0.35f, 0.31f, 0.30f, 0.045f},
									   loc::Tr("newasset.none"));
	m_pathLabel->dim = true;

	m_ui->Add<ui::Slider>(gfx::Rect{0.18f, 0.42f, 0.30f, 0.04f},
						  loc::Tr("newasset.metallic"), 0.0f, 1.0f, m_material.metallic,
						  [this](float v) { m_material.metallic = v; });
	m_ui->Add<ui::Slider>(gfx::Rect{0.18f, 0.49f, 0.30f, 0.04f},
						  loc::Tr("newasset.roughness"), 0.0f, 1.0f, m_material.roughness,
						  [this](float v) { m_material.roughness = v; });
	m_ui->Add<ui::Slider>(gfx::Rect{0.18f, 0.56f, 0.30f, 0.04f},
						  loc::Tr("newasset.height"), 0.0f, 0.1f, m_material.heightScale,
						  [this](float v) { m_material.heightScale = v; });
	m_ui->Add<ui::ColorPicker>(gfx::Rect{0.18f, 0.63f, 0.30f, 0.045f},
							   loc::Tr("newasset.color"), m_material.baseColor,
							   [this](const Vec4& c) { m_material.baseColor = c; });

	m_ui->Add<ui::Button>(gfx::Rect{0.18f, 0.76f, 0.14f, 0.05f},
						  loc::Tr("newasset.create"), [this] {
							  CreateRequest req;
							  req.category = m_category;
							  req.textureSet = m_textureSet;
							  req.name = m_nameField ? m_nameField->text : std::string();
							  req.sourcePath = m_sourcePath;
							  req.material = m_material;
							  if (onCreate) onCreate(req);
							  Close();
						  });
	m_ui->Add<ui::Button>(gfx::Rect{0.34f, 0.76f, 0.14f, 0.05f},
						  loc::Tr("newasset.cancel"), [this] { Close(); });
}

void AssetDialog::Browse() {
	const std::string path =
		m_textureSet
			? platform::PickFolder(m_window.Handle())
			: platform::PickFile(m_window.Handle(), L"3D models", L"*.gltf;*.glb;*.obj");
	if (path.empty()) return;
	m_sourcePath = path;
	if (m_pathLabel) {
		const size_t slash = path.find_last_of("\\/");
		m_pathLabel->text = slash == std::string::npos ? path : path.substr(slash + 1);
		m_pathLabel->dim = false;
	}

	// Live preview for model imports (the engine loads .gltf/.glb; .obj and
	// texture-only sets show no mesh until baked).
	m_previewMesh.reset();
	if (m_textureSet) return;
	auto model = assets::LoadModel(path);
	if (!model || model->meshes.empty()) {
		log::Warn("asset preview: could not load {}", path);
		return;
	}
	m_previewModel = std::move(*model);
	m_previewMesh = std::make_unique<gfx::Mesh>(m_device, m_previewModel.meshes[0]);
	if (!m_previewModel.materials.empty())
		m_material.baseColor = m_previewModel.materials[0].baseColorFactor;
}

void AssetDialog::Update(const Input& input, float width, float height, float dt) {
	if (!m_open) return;
	m_orbit += dt * 0.6f;
	m_ui->Update(input, width, height);
}

void AssetDialog::Render(gfx::SpriteBatch& batch, float width, float height) {
	if (!m_open) return;
	const ui::Theme& th = m_ui->GetTheme();
	batch.DrawRect({0, 0, width, height}, {0, 0, 0, 0.6f}); // dim the editor behind

	const gfx::Rect panel{0.14f * width, 0.10f * height, 0.72f * width, 0.80f * height};
	batch.DrawRect(panel, th.panel);
	ui::DrawBorder(batch, panel, th.panelBorder);

	// Preview pane backing (the owner blits the rendered model on top).
	const gfx::Rect pv = PreviewRect(width, height);
	batch.DrawRect(pv, {0.02f, 0.02f, 0.03f, 1.0f});
	ui::DrawBorder(batch, pv, th.panelBorder);

	m_ui->Render(batch, width, height);
}

} // namespace dungeon::game
