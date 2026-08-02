// ============================================================================
// Game/PartyBar.cpp — see PartyBar.h.
// ============================================================================
#include "Game/PartyBar.h"

#include "UI/UIContext.h"

namespace dungeon::game {

void PartyBar::LayoutSelf(ui::UIContext&) {
	const float slot = (1.0f - gap * static_cast<float>(kSlots - 1)) /
					   static_cast<float>(kSlots);
	const auto& slots = Children();
	for (size_t i = 0; i < slots.size(); ++i)
		slots[i]->bounds = {(slot + gap) * static_cast<float>(i), 0.0f, slot, 1.0f};
}

} // namespace dungeon::game
