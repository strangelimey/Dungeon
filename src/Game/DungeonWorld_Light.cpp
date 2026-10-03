// ============================================================================
// Game/DungeonWorld_Light.cpp - light profiles, and the party's own light: a
// LIT TORCH in a hand.
//
// Every light is pushed through PushLight from a lights.cat PROFILE
// (Game/LightProfile.h, lighting-updates Phase 2): its colour, brightness,
// reach, pulse and shadow come from the source's TYPE, not from code.
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
#include "Core/Log.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace dungeon::game {

namespace {
// The share of a torch's burn over which it dims, and how dim it gets.
constexpr float kDimShare = 0.1f;
constexpr float kDimFloor = 0.35f;
} // namespace

// --- light profiles (lighting-updates Phase 2) --------------------------------

void DungeonWorld::ReloadLightProfiles() {
	m_lightProfiles.clear();
	std::vector<std::string> problems;
	for (const CatalogEntry& e : m_project.lights.Entries())
		m_lightProfiles.push_back(light::Parse(
			e.id, [&e](std::string_view key) { return e.Get(key); }, &problems));
	for (const std::string& p : problems) log::Warn("{}", p);

	// A source naming a profile the project lacks still lights (the fallback),
	// but say so once here rather than leave it to be noticed as "too orange".
	const auto known = [this](std::string_view id) {
		for (const light::Profile& p : m_lightProfiles)
			if (p.id == id) return true;
		return false;
	};
	const auto check = [&](const Catalog& cat, const char* file) {
		for (const CatalogEntry& e : cat.Entries())
			if (const std::string id = e.Get("light"); !id.empty() && !known(id))
				log::Warn("{} [{}]: light '{}' is not in lights.cat; it draws as the "
						  "fallback warm light", file, e.id, id);
	};
	check(m_project.fixtures, "fixtures.cat");
	check(m_project.items, "items.cat");
	check(m_project.weapons, "weapons.cat");
	check(m_project.armor, "armor.cat");
	check(m_project.effects, "effects.cat");
	// The ids the code names when a source names none.
	for (const char* id : {"fire_sconce", "fire_brazier", "torch", "burning", "floor_glow",
						   "ember_sight"})
		if (!known(id))
			log::Warn("lights.cat has no [{}]; those lights use the fallback", id);

	// Item kinds are built once (PreloadItemKinds) and no other reload reaches
	// their `light`, so re-read it here: a torch's light can be re-pointed live.
	for (auto&& [id, kind] : m_itemKinds) {
		const CatalogEntry* def = m_project.FindItem(id);
		kind->light = CatalogGet(def, "light", kind->Lit() ? "torch" : "");
	}
	log::Info("Light profiles: {} (lights.cat)", m_lightProfiles.size());
}

const light::Profile& DungeonWorld::LightProfileFor(std::string_view id) const {
	for (const light::Profile& p : m_lightProfiles)
		if (p.id == id) return p;
	return light::Fallback();
}

Vec3 DungeonWorld::FixtureLightColor(const std::string& type) {
	return LightProfileFor(FixtureKindFor(type).light).color;
}

gfx::PointLight* DungeonWorld::PushLight(const light::Profile& profile, const char* source,
										 const Vec3& pos, float time, float phase,
										 const Vec3& color, float brightness,
										 float radiusMetres) {
	if (brightness <= 0.0f) return nullptr;
	const light::Sample s = light::Evaluate(profile, time, phase);
	gfx::PointLight l;
	l.position = {pos.x + s.offset.x * kCellSize, pos.y + s.offset.y * kCellSize,
				  pos.z + s.offset.z * kCellSize};
	l.radius = radiusMetres > 0.0f ? radiusMetres : profile.radius * kCellSize;
	l.color = profile.sourceColor ? color : profile.color;
	l.intensity = s.intensity * brightness;
	l.castsShadow = profile.shadow;
	l.longShadowFade = profile.longFade;
	// A wandering origin re-renders its shadow cube on the flicker cadence
	// rather than on every sub-pixel move (ShadowScheduler).
	l.flickerShadow = profile.wander > 0.0f;
	m_lights.points.push_back(l);
	const auto index = &profile - m_lightProfiles.data();
	m_lightOrigins.push_back(
		{source, index >= 0 && index < static_cast<std::ptrdiff_t>(m_lightProfiles.size())
					 ? static_cast<int>(index)
					 : -1});
	return &m_lights.points.back();
}

std::vector<std::string> DungeonWorld::DescribeLights() const {
	std::vector<std::string> out;
	const Vec3 eye = PartyEye();
	out.push_back(std::format("lights: {} this frame ({} before the budget of {}), {} profiles",
							  m_lights.points.size(), m_lightsBeforeCut,
							  m_settings.maxPointLights, m_lightProfiles.size()));
	for (size_t i = 0; i < m_lights.points.size(); ++i) {
		const gfx::PointLight& l = m_lights.points[i];
		const LightOrigin origin = i < m_lightOrigins.size() ? m_lightOrigins[i] : LightOrigin{};
		const int p = origin.profile;
		const char* profile = p >= 0 && p < static_cast<int>(m_lightProfiles.size())
								  ? m_lightProfiles[static_cast<size_t>(p)].id.c_str()
								  : "(fallback)";
		const Vec3 d = Sub(l.position, eye);
		out.push_back(std::format(
			"  [{:2}] {:<8} {:<13} rgb {:.2f} {:.2f} {:.2f}  i {:.2f}  r {:.1f} m  at {:.1f} m  {}",
			i, origin.source, profile, l.color.x, l.color.y, l.color.z, l.intensity, l.radius,
			std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z),
			!l.castsShadow ? "no shadow"
			: l.shadowSlot >= 0 ? std::format("shadow slot {}", l.shadowSlot)
								: std::string("shadow (no slot)")));
	}
	return out;
}

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
	// Its kind's profile (items.cat `light`), dimmed by its charge: the reach
	// shrinks to 60% and the brightness to the dim floor over the last tenth.
	const auto add = [&](const ItemKind& kind, const Vec3& at, float brightness, float phase) {
		const light::Profile& profile =
			LightProfileFor(kind.light.empty() ? std::string_view("torch") : kind.light);
		PushLight(profile, "torch", at, time, phase, {1, 1, 1}, brightness,
				  profile.radius * kCellSize * (0.6f + 0.4f * brightness));
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
				if (kind.Lit())
					add(kind, at, TorchBrightness(kind, slot.charge),
						static_cast<float>(m * 2 + h) * 1.7f);
			}
		}
	}
	if (m_cursorItem && m_cursorItem->has_value()) {
		const ItemKind& kind = ItemKindFor(**m_cursorItem);
		if (kind.Lit())
			add(kind, {eye.x, eye.y + 0.2f, eye.z}, TorchBrightness(kind, m_cursorItem->Charge()),
				0.0f);
	}
}

} // namespace dungeon::game
