// ============================================================================
// Game/CatalogSchema.h — what fields each catalog category understands.
//
// A .cat entry is free-form key = value (Catalog.h), with the fields a category
// reads documented in its file header. This turns that documentation into a
// TABLE: one FieldSpec row per field, giving its type, range, options and a
// one-line explanation. TypeEditorDialog renders a form straight from it, so
// exposing a new field in the editor is ONE ROW here — the kBalanceFields /
// kCategoryInfo / kThemeFields idiom the project uses everywhere else.
//
// The schema is deliberately NOT exhaustive: a catalog may carry fields no row
// mentions (hand-authored, or written by another dialog), and those round-trip
// untouched — the editor's writer starts from the existing entry and only sets
// what the user changed. Two fields sets are deliberately left OUT of the
// monster schema (archetype/keeprange/fleebelow/spell/threat_*/states/anim_*):
// MonsterConfigDialog owns them and REWRITES them authoritatively, so a second
// writer would fight it.
//
// TEXT POLICY: labels come from the field key itself, prettified
// ("height_scale" -> "Height scale"), and `help` is English in the table — a
// catalog field name is project vocabulary like an ini key or an asset name,
// which CLAUDE.md keeps out of Loc. The dialog's own chrome (title, section
// names, buttons) goes through loc:: as usual.
// ============================================================================
#pragma once

#include <span>
#include <string>
#include <string_view>

namespace dungeon::game {

// How a field is edited, and therefore which widget the dialog builds.
enum class FieldKind {
	Text,       // free string (display name, a token list)
	Bool,       // checkbox; written as 1 / 0
	Float,      // slider between lo and hi
	Enum,       // dropdown over `options` (space-separated tokens)
	TextureSet, // dropdown over the installed texture sets (assets/textures)
	Model,      // dropdown over the installed models (assets/models)
	CatalogRef, // dropdown over the ids of the catalog named by `options`
	// ONE id from the catalog named by `options`, like CatalogRef, but picked
	// from a list of rows that shows each candidate's face (a theme's floor /
	// wall / ceiling, with the palette's swatches) instead of a dropdown.
	CatalogRefPick,
	// Dropdown over the DAMAGE TYPES the project defines (damagetypes.cat).
	// Its own kind rather than a CatalogRef because the types are loaded into a
	// registry that resolves and validates them (DamageTypeBook), and the
	// dropdown should offer exactly what the game will accept. It replaced a
	// hardcoded "slash pierce bash fire earth air water" that went stale the
	// moment types became data — a project could author a type the editor
	// could not name.
	DamageType,
	// A quest's ORDERED stages, each with the line the log shows on reaching it
	// (`stages` = the ids, space-separated; `text_<id>` = each one's line). One
	// row per stage - an id, its text, a remove box - and an add button, so a
	// stage and what it says are authored together rather than as a list here
	// and a hand-typed field elsewhere.
	QuestStages,
	// A WEIGHTED LIST of ids from the catalog named by `options`, written
	// `<id> [weight], ...` (a style's `monsters`, Game/Style.h). One row per
	// entry - the id as a dropdown, its weight, a remove box - and an add
	// button; the dropdown names each candidate through faceFor, so a monster
	// shows its power.
	WeightedRefs,
};

// One editable field of one category.
struct FieldSpec {
	const char* key;         // the .cat field name
	FieldKind kind;
	const char* sectionKey;  // loc key of the tab it lives under
	const char* help;        // one-line explanation (English, see TEXT POLICY)
	float lo = 0.0f;         // Float: slider range
	float hi = 1.0f;
	// Float: value granularity. A slider is continuous, but a catalog number
	// wants to read like an authored one — `hp = 17`, not `hp = 16.9157` — so
	// the dialog snaps to this (0 = the 0.001 fallback).
	float step = 0.0f;
	const char* options = ""; // Enum: tokens; CatalogRef: the catalog key
	const char* def = "";     // value shown when the entry doesn't carry the field
	// Float only, and only for a field whose ABSENCE is meaningful (no `def`):
	// the value to seed when the user takes manual control of it. An absent
	// metallic/roughness means "the texture's ORM map decides", which no slider
	// position can represent — so the form offers the choice as a checkbox and
	// starts the slider here (1 = "scale the map by 1", i.e. what you already
	// see) rather than at the range's bottom.
	const char* neutral = "";
	// Changing this field invalidates BAKED geometry (the worn block meshes),
	// so saving it has to re-run AssetBaker before the change is visible.
	bool rebakes = false;
	// Text only: the longest value the field accepts (0 = the dialog's 64). A
	// generator settings line runs to a couple of hundred characters.
	int maxLen = 0;
	// What the form calls the field, when that is not its key prettified (the
	// `destructible` key reads "Breakable"). Null = PrettyFieldName(key).
	const char* label = nullptr;
};

// The rows for a project catalog key ("walls", "monsters", ...); empty for a
// category with no schema yet (the dialog then shows only the identity rows).
std::span<const FieldSpec> SchemaFor(std::string_view catalogKey);

// "height_scale" -> "Height scale": the label shown for a field key.
std::string PrettyFieldName(std::string_view key);
// The name a form row shows: the spec's `label`, else its key prettified.
std::string FieldLabel(const FieldSpec& spec);

// Section loc keys, in tab order (the dialog walks the schema in this order).
inline constexpr const char* kSectionIdentity = "map.type.sec.identity";
inline constexpr const char* kSectionLook = "map.type.sec.look";
inline constexpr const char* kSectionMaterial = "map.type.sec.material";
inline constexpr const char* kSectionStats = "map.type.sec.stats";
inline constexpr const char* kSectionRules = "map.type.sec.rules";
inline constexpr const char* kSectionQuest = "map.type.sec.quest";
inline constexpr const char* kSectionStages = "map.type.sec.stages";
inline constexpr const char* kSectionShape = "map.type.sec.shape";
inline constexpr const char* kSectionMonsters = "map.type.sec.monsters";

// CatalogRef `options` that name no catalog but a list Game builds: every
// "<quest>:<stage>" pair, every world-map location id, and every UI material
// (assets/ui/stones).
inline constexpr const char* kOptQuestStages = "@queststages";
inline constexpr const char* kOptLocations = "@locations";
inline constexpr const char* kOptUiStones = "@uistones";

} // namespace dungeon::game
