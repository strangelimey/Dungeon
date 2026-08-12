// ============================================================================
// Game/ProjectileInspector.h — the editor's in-flight-projectile details modal.
//
// Opened by right-clicking a live projectile (spell bolt / arrow / thrown item)
// on the editor map. Projectiles are TRANSIENT combat content — not authored,
// never saved — so this is a read-only details view (side, damage type/amount,
// accuracy, speed, remaining range) plus one action: Remove, which dismisses
// the in-flight projectile. Standalone modal (its own UIContext), like
// LevelSettingsDialog, rather than an InstanceInspector: a projectile has no
// editable facing/placement, so the base class's common strip wouldn't fit.
//
// The world is frozen while any inspector modal is up (Game returns before the
// world update), so the projectile addressed by TargetId() stays put — pair it
// with the editor's pause button to catch a fast shot mid-flight.
// ============================================================================
#pragma once

#include "Core/Types.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h"

#include <functional>
#include <string>

namespace dungeon::game {

class ProjectileInspector {
public:
	// Everything shown, resolved by the owner (Game) — side/type already
	// localized; the numbers are formatted here.
	struct Config {
		u32 id = 0;
		std::string side;    // "Party spell" / "Monster shot"
		std::string dmgType; // localized damage-type name
		float damage = 0.0f;
		float accuracy = 0.0f; // the attacker's opposed-roll bonus, in d100 points
		float speed = 0.0f;    // m/s
		float rangeLeft = 0.0f; // metres
		// What the carrier will DELIVER when it lands or expires, as ONE line
		// ("burn 3.0/s for 6.0s", or just the ids when it carries several). An em
		// dash for a plain bolt. Composed by the owner, like the localized strings
		// above — and one line rather than one row per effect so the card keeps a
		// FIXED row count: a Stack shrinks its fixed rows to fit, so rows that
		// vary with content would squeeze text drawn at an unchanged font, which
		// is the overlap the fonts audit already had to fix in this dialog once.
		std::string payload;
	};

	ProjectileInspector(gfx::GraphicsDevice& device, ui::FontLibrary& fonts);

	bool IsOpen() const { return m_open; }
	void Open(const Config& cfg);
	void Close() { m_open = false; }

	// The projectile this dialog is inspecting (0 when closed) — the owner
	// re-resolves it by id for the Remove action.
	u32 TargetId() const { return m_cfg.id; }

	// The Remove button: the owner dismisses the in-flight projectile.
	std::function<void()> onRemove;

	void Update(const Input& input, float width, float height);
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

private:
	void BuildUI();

	gfx::GraphicsDevice& m_device;
	ui::UIContext m_ui; // Remove footer button; also draws the title + info lines
	const gfx::Texture* m_closeIcon = nullptr; // shared, owned by AssetUtil

	bool m_open = false;
	Config m_cfg;
};

} // namespace dungeon::game
