// ============================================================================
// Game/DungeonWorld_Light.cpp - the party's own light: a LIT TORCH in a hand.
//
// There is no light at the eye any more (spell-updates Phase 4, Michael: "the
// held torch is the light"). Each lit torch a member holds - and one on the
// cursor - is a flickering point light at that member's side of the eye; with
// none, the party sees by the level's ambient alone, and a level authored at
// ambient 0 is pitch black.
//
// A lit torch BURNS while it is held: its CHARGE (ItemSlot::charge) counts its
// seconds down from the kind's `burn_time`, it dims over its last tenth, and
// spent it becomes its `spent_as` stub. Stowed in a pack it goes out (the
// floor's half of that rule is PlaceDrop's), keeping what is left.
//
// Everything here runs every frame, so it allocates nothing: the ids it renames
// are assigned into the slots' own buffers (always to a shorter or equal-length
// id in the shipped catalog, and a std::string keeps its capacity anyway), and
// the kinds are all built at load (PreloadItemKinds).
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Core/Loc.h"

#include <algorithm>
#include <cmath>

namespace dungeon::game {

namespace {
// The share of a torch's burn over which it dims, and how dim it gets.
constexpr float kDimShare = 0.1f;
constexpr float kDimFloor = 0.35f;
// A torch's light, as the old carried light was.
constexpr float kTorchRadius = 9.0f;
constexpr float kTorchIntensity = 2.6f;
} // namespace

float DungeonWorld::TorchBrightness(const ItemKind& kind, float charge) {
	if (!kind.Lit()) return 0.0f;
	const float left = charge < 0.0f ? kind.burnTime : charge;
	const float share = left / kind.burnTime;
	return share >= kDimShare ? 1.0f
							  : kDimFloor + (1.0f - kDimFloor) * std::max(share, 0.0f) / kDimShare;
}

bool DungeonWorld::BurnTorch(ItemSlot& slot, float dt, const Character* holder) {
	const ItemKind& kind = ItemKindFor(slot.typeId);
	if (!kind.Lit()) return false;
	if (slot.charge < 0.0f) slot.charge = kind.burnTime; // a fresh one starts full
	slot.charge -= dt;
	if (slot.charge > 0.0f) return false;
	// Burnt out: the stub (or nothing, for a kind that leaves none). The line
	// names the torch by its UNLIT name - "Sera's torch burns out", not "Sera's
	// Lit torch".
	if (holder) {
		const ItemKind& named = kind.unlitAs.empty() ? kind : ItemKindFor(kind.unlitAs);
		MemberMessage(*holder, loc::FormatLine("log.torch_spent", holder->name,
											   loc::View(named.nameKey)));
	}
	if (kind.spentAs.empty()) {
		slot.Clear();
	} else {
		slot.typeId.assign(kind.spentAs);
		slot.charge = kNoCharge;
	}
	return true;
}

void DungeonWorld::TickCarriedLight(float dt) {
	if (!m_roster) return;
	for (Character& c : *m_roster) {
		// In a HAND it burns (a downed member's torch burns on in their grip).
		for (int h = 0; h < 2; ++h) {
			ItemSlot& slot = c.inventory.Hand(h);
			if (!slot.Empty()) BurnTorch(slot, dt, &c);
		}
		// In a PACK it goes out, keeping what is left of it.
		for (Pack& pack : c.inventory.packs)
			for (ItemSlot& s : pack.contents) {
				if (s.Empty()) continue;
				const ItemKind& kind = ItemKindFor(s.typeId);
				if (kind.Lit() && !kind.unlitAs.empty()) s.typeId.assign(kind.unlitAs);
			}
	}
	// On the CURSOR it is still in the leader's hand: it burns.
	// (Through a member scratch slot, swapped in and out: a local ItemSlot would
	// construct a string every frame, which the debug CRT allocates for.)
	if (m_cursorItem && m_cursorItem->has_value()) {
		ItemSlot& held = m_cursorScratch;
		held.typeId.swap(m_cursorItem->Id());
		held.charge = m_cursorItem->Charge();
		BurnTorch(held, dt, LeaderMember());
		m_cursorItem->Id().swap(held.typeId);
		m_cursorItem->SetCharge(held.charge);
		held.Clear();
	}
}

void DungeonWorld::AppendCarriedLights(float time) {
	const Vec3 eye = PartyEye(); // a carried light falls with the camera
	const float flicker =
		0.92f + 0.08f * std::sin(time * 9.0f) * std::sin(time * 13.7f + 1.3f);
	const auto add = [&](const Vec3& at, float brightness) {
		gfx::PointLight torch;
		torch.position = at;
		torch.radius = kTorchRadius * (0.6f + 0.4f * brightness);
		torch.color = kTorchColor;
		torch.intensity = kTorchIntensity * flicker * brightness;
		m_lights.points.push_back(torch);
	};
	if (m_roster) {
		const Direction faced = static_cast<Direction>(m_party.Facing());
		for (size_t m = 0; m < m_roster->size(); ++m) {
			const Character& c = (*m_roster)[m];
			if (!c.IsAlive()) continue;
			// At the member's own side of the eye, as their casts and throws leave
			// from (the quadrant lane), a little lower than the old eye light.
			const Direction lateral = static_cast<Direction>(
				(static_cast<int>(faced) + (m % 2 == 0 ? 3 : 1)) % 4);
			const Vec3 at{eye.x + static_cast<float>(DirDX(lateral)) * kCellSize * 0.15f,
						  eye.y + 0.2f,
						  eye.z + static_cast<float>(DirDZ(lateral)) * kCellSize * 0.15f};
			for (int h = 0; h < 2; ++h) {
				const ItemSlot& slot = c.inventory.Hand(h);
				if (slot.Empty()) continue;
				const ItemKind& kind = ItemKindFor(slot.typeId);
				if (kind.Lit()) add(at, TorchBrightness(kind, slot.charge));
			}
		}
	}
	if (m_cursorItem && m_cursorItem->has_value()) {
		const ItemKind& kind = ItemKindFor(**m_cursorItem);
		if (kind.Lit())
			add({eye.x, eye.y + 0.2f, eye.z}, TorchBrightness(kind, m_cursorItem->Charge()));
	}
}

} // namespace dungeon::game
