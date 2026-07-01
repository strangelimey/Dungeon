// ============================================================================
// Game/PropInspector.h — the editor's per-INSTANCE editor for placed items and
// decorations.
//
// A concrete InstanceInspector (see that header). Items and decorations differ
// only in how an edit is applied/persisted, and today both expose just the
// common Facing property, so ONE dialog serves both — the Config carries a Kind
// and an opaque handle (decoration index, or item entity id) that the owner maps
// back to the live object in the onApply/onSave callbacks. Extra type-specific
// controls slot into BuildContent when they arrive.
// ============================================================================
#pragma once

#include "Game/InstanceInspector.h"

#include <functional>
#include <string>

namespace dungeon::game {

class PropInspector : public InstanceInspector {
public:
	struct Config {
		enum class Kind { Item, Decoration } kind = Kind::Item;
		int handle = 0;       // decoration: list index; item: stable entity id
		std::string type;     // catalog display name (for the title)
		Direction facing = Direction::South;
	};

	explicit PropInspector(gfx::GraphicsDevice& device) : InstanceInspector(device) {}

	void Open(const Config& cfg);

	std::function<void(const Config&)> onApply; // set facing on the live object
	std::function<void()> onSave;               // persist the level

protected:
	std::string Title() const override;
	gfx::Rect Panel() const override { return {0.35f, 0.33f, 0.30f, 0.28f}; }
	void ApplyLive() override;
	void Persist() override;
	void Revert() override;

private:
	Config m_cfg;
	Config m_original;
};

} // namespace dungeon::game
