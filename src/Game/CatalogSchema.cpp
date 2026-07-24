// ============================================================================
// Game/CatalogSchema.cpp — see CatalogSchema.h. One table per catalog category.
//
// Rows use DESIGNATED initializers: a row names the members it sets, so the
// table reads as documentation and inserting a member into FieldSpec doesn't
// silently reinterpret every row.
// ============================================================================
#include "Game/CatalogSchema.h"

#include <cctype>

namespace dungeon::game {

namespace {
// --- rows shared by several categories --------------------------------------
// Every prop-like category (a model + a texture set + draw flags) repeats these,
// so they are written once and spliced into the tables below — a table stays a
// flat literal you can read top to bottom, which is the point.
#define IDENTITY_DISPLAY                                                       \
	{.key = "display", .kind = FieldKind::Text, .sectionKey = kSectionIdentity, \
	 .help = "Name shown in the editor palette and inspectors."}
#define IDENTITY_CATEGORY                                                       \
	{.key = "category", .kind = FieldKind::Text, .sectionKey = kSectionIdentity, \
	 .help = "Groups this type under a collapsible sub-accordion in the palette."}
#define PROP_MODEL                                                        \
	{.key = "model", .kind = FieldKind::Model, .sectionKey = kSectionLook, \
	 .help = "The mesh in assets/models (without the extension)."}
#define PROP_TEXTURE                                                             \
	{.key = "texture", .kind = FieldKind::TextureSet, .sectionKey = kSectionLook, \
	 .help = "PBR set bound by name; empty keeps the model's own glTF material."}
#define PROP_SCALE                                                        \
	{.key = "scale", .kind = FieldKind::Float, .sectionKey = kSectionLook, \
	 .help = "Trims the authored size (1 = as authored, in dungeon squares).",  \
	 .lo = 0.1f, .hi = 4.0f, .step = 0.05f, .def = "1"}
#define PROP_AUTHORED                                                        \
	{.key = "authored", .kind = FieldKind::Bool, .sectionKey = kSectionLook,  \
	 .help = "Back-face cull (on for bought/authored meshes, off for hand-built ones).", \
	 .def = "1"}
#define MATERIAL_ROWS                                                              \
	{.key = "metallic", .kind = FieldKind::Float, .sectionKey = kSectionMaterial,   \
	 .help = "Scales the ORM map's metalness; absent leaves the map authoritative.", \
	 .lo = 0.0f, .hi = 1.0f, .step = 0.05f},                                        \
	{.key = "roughness", .kind = FieldKind::Float, .sectionKey = kSectionMaterial,  \
	 .help = "Scales the ORM map's roughness; absent leaves the map authoritative.", \
	 .lo = 0.0f, .hi = 1.0f, .step = 0.05f},                                        \
	{.key = "height_scale", .kind = FieldKind::Float, .sectionKey = kSectionMaterial, \
	 .help = "Parallax depth of the height map, in units. 0 = flat.",               \
	 .lo = 0.0f, .hi = 0.2f, .step = 0.005f}

// --- walls ------------------------------------------------------------------
// The worn block mesh is baked per texture set, so `texture`, `wear` and
// `columns` all invalidate it (rebakes = the owner re-runs AssetBaker).
constexpr FieldSpec kWallFields[] = {
	IDENTITY_DISPLAY,
	IDENTITY_CATEGORY,
	{.key = "texture", .kind = FieldKind::TextureSet, .sectionKey = kSectionLook,
	 .help = "The PBR set this surface paints with; its height map drives the worn relief.",
	 .rebakes = true},
	{.key = "height_scale", .kind = FieldKind::Float, .sectionKey = kSectionLook,
	 .help = "Parallax depth of the painted relief, in units.",
	 .lo = 0.0f, .hi = 0.12f, .step = 0.005f, .def = "0.055"},
	{.key = "wear", .kind = FieldKind::Float, .sectionKey = kSectionLook,
	 .help = "Displacement of the block mesh: 0 = a flat panel, 1 = full worn relief.",
	 .lo = 0.0f, .hi = 1.0f, .step = 0.05f, .def = "1", .rebakes = true},
	{.key = "columns", .kind = FieldKind::Bool, .sectionKey = kSectionLook,
	 .help = "The wall's edge pillars / border strips.", .def = "1", .rebakes = true},
};

// --- floors / ceilings ------------------------------------------------------
// Same as walls minus the pillars (a wall-block feature).
constexpr FieldSpec kFlatSurfaceFields[] = {
	IDENTITY_DISPLAY,
	IDENTITY_CATEGORY,
	{.key = "texture", .kind = FieldKind::TextureSet, .sectionKey = kSectionLook,
	 .help = "The PBR set this surface paints with; its height map drives the worn relief.",
	 .rebakes = true},
	{.key = "height_scale", .kind = FieldKind::Float, .sectionKey = kSectionLook,
	 .help = "Parallax depth of the painted relief, in units.",
	 .lo = 0.0f, .hi = 0.12f, .step = 0.005f, .def = "0.045"},
	{.key = "wear", .kind = FieldKind::Float, .sectionKey = kSectionLook,
	 .help = "Displacement of the block mesh: 0 = a flat panel, 1 = full worn relief.",
	 .lo = 0.0f, .hi = 1.0f, .step = 0.05f, .def = "1", .rebakes = true},
};

// --- decorations ------------------------------------------------------------
constexpr FieldSpec kDecorationFields[] = {
	IDENTITY_DISPLAY,
	IDENTITY_CATEGORY,
	PROP_MODEL,
	PROP_TEXTURE,
	PROP_SCALE,
	PROP_AUTHORED,
	{.key = "solid", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "Blocks the square (the party and monsters can't enter).", .def = "0"},
	{.key = "mount", .kind = FieldKind::Enum, .sectionKey = kSectionRules,
	 .help = "Where the brush hangs it: on a wall face, or standing on the floor.",
	 .options = "floor wall", .def = "floor"},
	{.key = "facing_arrow", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "Draw the editor's green facing arrow for placements of this type.",
	 .def = "1"},
	{.key = "alpha_test", .kind = FieldKind::Float, .sectionKey = kSectionRules,
	 .help = "Cutout threshold for a masked texture (0 = opaque, no clip).",
	 .lo = 0.0f, .hi = 1.0f, .step = 0.05f, .def = "0"},
	MATERIAL_ROWS,
};

// --- fixtures ---------------------------------------------------------------
// flame_height / flame_out are UNITS (points on the mesh); flame_scale is a
// dimensionless particle size.
constexpr FieldSpec kFixtureFields[] = {
	IDENTITY_DISPLAY,
	IDENTITY_CATEGORY,
	PROP_MODEL,
	PROP_TEXTURE,
	PROP_SCALE,
	{.key = "part2_model", .kind = FieldKind::Model, .sectionKey = kSectionLook,
	 .help = "Optional second mesh drawn with it (the brazier's coals)."},
	{.key = "part2_texture", .kind = FieldKind::TextureSet, .sectionKey = kSectionLook,
	 .help = "PBR set for that second mesh."},
	{.key = "mount", .kind = FieldKind::Enum, .sectionKey = kSectionRules,
	 .help = "Wall-mounted (a sconce resolves its wall) or standing on the floor.",
	 .options = "floor wall", .def = "floor"},
	{.key = "flame", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "This kind burns; off places it unlit, with no light or particles.",
	 .def = "1"},
	{.key = "flame_height", .kind = FieldKind::Float, .sectionKey = kSectionRules,
	 .help = "Height of the flame origin on the mesh, in units.",
	 .lo = 0.0f, .hi = 1.5f, .step = 0.002f},
	{.key = "flame_out", .kind = FieldKind::Float, .sectionKey = kSectionRules,
	 .help = "How far the flame origin stands out from the mesh, in units.",
	 .lo = 0.0f, .hi = 0.3f, .step = 0.002f},
	{.key = "flame_scale", .kind = FieldKind::Float, .sectionKey = kSectionRules,
	 .help = "Particle-effect size (brazier 1.0, sconce ~0.55).",
	 .lo = 0.0f, .hi = 3.0f, .step = 0.05f, .def = "1"},
};

// --- monsters ---------------------------------------------------------------
// Behaviour + animation rows are MonsterConfigDialog's (see the header note).
constexpr FieldSpec kMonsterFields[] = {
	IDENTITY_DISPLAY,
	IDENTITY_CATEGORY,
	PROP_MODEL,
	PROP_TEXTURE,
	{.key = "modelscale", .kind = FieldKind::Float, .sectionKey = kSectionLook,
	 .help = "Trims the authored size (1 = as authored, in dungeon squares).",
	 .lo = 0.1f, .hi = 4.0f, .step = 0.05f, .def = "1"},
	{.key = "modelyaw", .kind = FieldKind::Float, .sectionKey = kSectionLook,
	 .help = "Turns the mesh so it faces +Z like the engine expects (degrees).",
	 .lo = -180.0f, .hi = 180.0f, .step = 5.0f, .def = "0"},
	{.key = "hp", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Hit points.", .lo = 1.0f, .hi = 200.0f, .step = 1.0f, .def = "10"},
	{.key = "damage", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Damage per landed blow.", .lo = 0.0f, .hi = 50.0f, .step = 1.0f, .def = "3"},
	{.key = "dmgtype", .kind = FieldKind::Enum, .sectionKey = kSectionStats,
	 .help = "Damage type its melee deals (resists key off this).",
	 .options = "slash pierce bash fire earth air water", .def = "bash"},
	{.key = "accuracy", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Chance to land a blow before the defender's evasion.",
	 .lo = 0.0f, .hi = 1.0f, .step = 0.05f, .def = "0.6"},
	{.key = "defense", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Evasion against incoming attacks.",
	 .lo = 0.0f, .hi = 1.0f, .step = 0.05f, .def = "0"},
	{.key = "armor", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Flat damage soak.", .lo = 0.0f, .hi = 20.0f, .step = 1.0f, .def = "0"},
	{.key = "resists", .kind = FieldKind::Text, .sectionKey = kSectionStats,
	 .help = "Per-type resistance, e.g. 'pierce 0.5, slash 0.25' (1 = immune, negative = weakness)."},
	{.key = "reach", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Melee range in squares; 2 lets it strike from one square back.",
	 .lo = 1.0f, .hi = 3.0f, .step = 1.0f, .def = "1"},
	{.key = "attackcd", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Seconds between its attacks.", .lo = 0.2f, .hi = 5.0f, .step = 0.1f, .def = "1.5"},
	{.key = "movecd", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Seconds between its steps.", .lo = 0.1f, .hi = 3.0f, .step = 0.05f, .def = "0.5"},
	{.key = "aggro", .kind = FieldKind::Float, .sectionKey = kSectionRules,
	 .help = "How many squares away it notices the party.",
	 .lo = 0.0f, .hi = 20.0f, .step = 1.0f, .def = "6"},
	{.key = "iq", .kind = FieldKind::Float, .sectionKey = kSectionRules,
	 .help = "Thinking rate: picks its AI bucket, so a dim monster changes its mind slower.",
	 .lo = 0.0f, .hi = 200.0f, .step = 5.0f, .def = "100"},
	{.key = "faces", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "Has a facing (off hides the editor's facing arrow for it).", .def = "1"},
};

// --- buttons ----------------------------------------------------------------
constexpr FieldSpec kButtonFields[] = {
	IDENTITY_DISPLAY, IDENTITY_CATEGORY, PROP_MODEL, PROP_TEXTURE, PROP_SCALE, PROP_AUTHORED,
};

// --- doors ------------------------------------------------------------------
constexpr FieldSpec kDoorFields[] = {
	IDENTITY_DISPLAY, IDENTITY_CATEGORY, PROP_MODEL, PROP_TEXTURE, PROP_SCALE, PROP_AUTHORED,
	{.key = "hidden", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "Internal type the palette never offers (the shared door frame).",
	 .def = "0"},
};

// --- stairs / pits ----------------------------------------------------------
constexpr FieldSpec kStairFields[] = {
	IDENTITY_DISPLAY,
	IDENTITY_CATEGORY,
	PROP_MODEL,
	PROP_TEXTURE,
	PROP_SCALE,
	PROP_AUTHORED,
	{.key = "solid", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "Blocks the square.", .def = "0"},
	{.key = "up", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "Leads UP (also picks which way the map icon's arrow points).",
	 .def = "0"},
	{.key = "pair", .kind = FieldKind::CatalogRef, .sectionKey = kSectionRules,
	 .help = "The type auto-authored on the destination level (a pit pairs with pit_ceiling).",
	 .options = "stairs"},
	{.key = "hole", .kind = FieldKind::Enum, .sectionKey = kSectionRules,
	 .help = "Which cell block the mesh builder skips so the shaft shows.",
	 .options = "none floor ceiling", .def = "none"},
	{.key = "traverse", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "Stepping on it transitions (off = scenery, e.g. a pit's ceiling hole).",
	 .def = "1"},
	{.key = "fall", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "The transition is a PLUNGE: the camera drops through the shaft first.",
	 .def = "0"},
};

// --- items ------------------------------------------------------------------
// Items name themselves with a LOC KEY (`name = item.<id>`), not a display
// string — they are the one category whose label is translated.
constexpr FieldSpec kItemFields[] = {
	{.key = "name", .kind = FieldKind::Text, .sectionKey = kSectionIdentity,
	 .help = "Loc key of the item's name, by convention item.<id> (add it to every .lang)."},
	IDENTITY_CATEGORY,
	PROP_MODEL,
	PROP_TEXTURE,
	PROP_SCALE,
	{.key = "weight", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Kilograms, against the carry load.",
	 .lo = 0.0f, .hi = 50.0f, .step = 0.1f, .def = "1"},
	{.key = "damage", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Weapon damage before stats and skill.", .lo = 0.0f, .hi = 50.0f, .step = 1.0f},
	{.key = "speed", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Swing speed multiplier.", .lo = 0.1f, .hi = 3.0f, .step = 0.05f},
	{.key = "armor", .kind = FieldKind::Float, .sectionKey = kSectionStats,
	 .help = "Damage soak when worn.", .lo = 0.0f, .hi = 20.0f, .step = 1.0f},
	{.key = "stats", .kind = FieldKind::Text, .sectionKey = kSectionStats,
	 .help = "Which stats scale its damage, e.g. 'str dex'."},
	{.key = "resists", .kind = FieldKind::Text, .sectionKey = kSectionStats,
	 .help = "Per-type resistance granted when worn, e.g. 'fire 0.2'."},
	{.key = "holdable", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "Can be held in a hand slot.", .def = "1"},
	{.key = "symbol", .kind = FieldKind::Enum, .sectionKey = kSectionRules,
	 .help = "Rune symbol this item teaches (runes only).",
	 .options = "fire earth air water project protect sight"},
	{.key = "reach", .kind = FieldKind::Enum, .sectionKey = kSectionRules,
	 .help = "Weapons that strike from the rear rank.",
	 .options = "melee polearm ranged", .def = "melee"},
	{.key = "command", .kind = FieldKind::Text, .sectionKey = kSectionRules,
	 .help = "Hand-menu verbs this item offers, e.g. 'swing thrust'."},
	{.key = "capacity", .kind = FieldKind::Float, .sectionKey = kSectionRules,
	 .help = "Container capacity in kilograms.", .lo = 0.0f, .hi = 50.0f, .step = 0.5f},
	{.key = "accepts", .kind = FieldKind::Text, .sectionKey = kSectionRules,
	 .help = "Item categories a container takes, e.g. 'rune'."},
};

// --- wall features ----------------------------------------------------------
constexpr FieldSpec kWallFeatureFields[] = {
	IDENTITY_DISPLAY,
	IDENTITY_CATEGORY,
	{.key = "model", .kind = FieldKind::Model, .sectionKey = kSectionLook,
	 .help = "The panel mesh stamped in place of the plain wall panel."},
	{.key = "bore", .kind = FieldKind::Bool, .sectionKey = kSectionRules,
	 .help = "A see-through window bored THROUGH the block, not a pocket in its face.",
	 .def = "0"},
};
} // namespace

std::span<const FieldSpec> SchemaFor(std::string_view catalogKey) {
	if (catalogKey == "walls") return kWallFields;
	if (catalogKey == "floors" || catalogKey == "ceilings") return kFlatSurfaceFields;
	if (catalogKey == "decorations") return kDecorationFields;
	if (catalogKey == "fixtures") return kFixtureFields;
	if (catalogKey == "monsters") return kMonsterFields;
	if (catalogKey == "buttons") return kButtonFields;
	if (catalogKey == "doors") return kDoorFields;
	if (catalogKey == "stairs") return kStairFields;
	if (catalogKey == "items") return kItemFields;
	if (catalogKey == "wallfeatures") return kWallFeatureFields;
	return {};
}

std::string PrettyFieldName(std::string_view key) {
	std::string out(key);
	for (char& c : out)
		if (c == '_') c = ' ';
	if (!out.empty())
		out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
	return out;
}

} // namespace dungeon::game
