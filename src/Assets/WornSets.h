// ============================================================================
// Assets/WornSets.h - the one record of how a surface texture set's worn block
// meshes are baked: the surface KIND they are baked as, the RELIEF (how far the
// stones stand proud, in metres) and the SEED of their noise terms.
//
// A set's worn meshes (worn_<set>_<tier>*.gltf) are named by the SET alone and
// shared by every world and every catalog type that paints with it, so what
// shapes them belongs to the set, not to a type. Three things read this record:
//   * `AssetBaker models` bakes every shipped set at its record;
//   * `AssetBaker wornblock` (what the editor runs after an import or a type
//     save) bakes one set at its record - and REFUSES a shipped set asked for
//     as a kind it is not, since the file name carries no kind and a floor bake
//     of a wall set would turn every wall of it into floor;
//   * the editor's type dialog shows the record's relief as what an unset
//     `relief` field bakes at.
// They used to be two sources that disagreed (code-review C406): the full bake
// had its own table, while `wornblock` fell back to a flat per-kind relief and a
// std::hash seed - so saving ANY surface field in the type editor re-baked a
// shipped set at another depth with other noise, and its slider started at the
// wrong value. It lives in Assets, beside WornPanel.h, because the baker and the
// game are two binaries and a private copy on each side is two things to keep
// in step.
//
// A set the table does not list (an editor import) gets a DERIVED record: the
// kind it is baked as, that kind's default relief, and a seed hashed from its
// name with a fixed hash (std::hash is the implementation's to change). Derived
// rather than stored, so nothing on disk can drift from it.
//
// A type's `relief` and `wear` catalog fields are OVERRIDES on this: the baker
// scales the WHOLE displacement by relief x wear / the record's relief, so a
// type that sets neither bakes byte-for-byte what `models` does.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <optional>
#include <span>
#include <string_view>

namespace dungeon::assets {

enum class WornKind : u8 { Wall, Floor, Ceiling };

struct WornSet {
	// The set's base name (no _1k/_2k/_4k). A derived record views the name it
	// was asked about, so it lives as long as that string does.
	std::string_view texture;
	WornKind kind = WornKind::Wall;
	float relief = 0.0f; // displacement amplitude, metres
	u32 seed = 1;        // seeds the noise terms (bowed masonry, unevenness)
};

// Every set the shipped pool bakes, in `AssetBaker models` order.
std::span<const WornSet> ShippedWornSets();

// The shipped record for `texture`, or null when the table does not list it.
const WornSet* FindShippedWornSet(std::string_view texture);

// What a set of `kind` that the table does not list bakes at.
float DefaultWornRelief(WornKind kind);

// The record a bake of `texture` as `kind` uses: the shipped one when listed
// (whatever kind was asked - check `kind` against it before baking), else the
// derived one.
WornSet WornSetFor(std::string_view texture, WornKind kind);

// "wall" / "floor" / "ceiling" - the `wornblock` argument and imports.cat's
// `surface`. Anything else is nullopt, never a silent wall.
std::optional<WornKind> ParseWornKind(std::string_view name);
std::string_view WornKindName(WornKind kind);

} // namespace dungeon::assets
