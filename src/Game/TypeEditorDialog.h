// ============================================================================
// Game/TypeEditorDialog.h — the editor's per-TYPE edit modal, for EVERY palette
// category.
//
// The per-INSTANCE half of the editor has had proper dialogs for a while
// (InstanceInspector + its six subclasses); this is their opposite number:
// right-click a palette ROW and edit the catalog entry behind it. One dialog
// serves all ten categories because it does not know any of them — it renders
// its form from CatalogSchema's FieldSpec table (sections become tabs, kinds
// become widgets), so a new field is one table row and a new category is one
// table.
//
// Editing model, deliberately unlike the instance inspectors: NO live apply.
// A type is referenced by every placement of it (and, for surfaces, by baked
// geometry), so changes commit on SAVE — the owner writes the catalog and
// reloads or re-bakes whatever the change invalidates. Close/Esc simply
// discards. Only fields the user actually TOUCHED are written, so an entry's
// other fields — hand-authored, or owned by another dialog like
// MonsterConfigDialog's animation rows — round-trip untouched.
//
// The owner supplies the dropdown contents (installed texture sets and models,
// another catalog's ids) through optionsFor, since the pool and the project are
// Game's to know, not the dialog's.
// ============================================================================
#pragma once

#include "Game/CatalogSchema.h"
#include "Game/Serialize.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h"

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace dungeon::ui {
class TabControl;
class TextField;
} // namespace dungeon::ui

namespace dungeon::game {

class TypeEditorDialog {
public:
	// The entry under edit. `fields` is the working copy: it starts as the
	// catalog entry's own fields and Save writes the touched values into it, so
	// the owner can hand it straight to the catalog writer.
	struct Config {
		std::string catalogKey;    // "walls", "monsters", ... (routes the write)
		std::string categoryLabel; // localized category name, for the title
		std::string id;            // catalog id being edited
		std::vector<serialize::Field> fields;
		// Set on Save when a touched field carries FieldSpec::rebakes — the
		// owner must re-run AssetBaker before the change is visible.
		bool rebake = false;
	};

	TypeEditorDialog(gfx::GraphicsDevice& device, ui::FontLibrary& fonts);

	bool IsOpen() const { return m_open; }
	// Opens on a copy of the entry's fields, with the schema for its category.
	void Open(Config cfg, std::span<const FieldSpec> schema);
	void Close() {
		m_open = false;
		m_busy = false;
		m_helpOpen = false;
	}
	// The owner sets this while an async re-bake runs: the form freezes behind a
	// notice until the owner closes the dialog (the AssetDialog pattern).
	void SetBusy(bool busy) { m_busy = busy; }

	const std::string& CatalogKey() const { return m_cfg.catalogKey; }
	const std::string& Id() const { return m_cfg.id; }

	void Update(const Input& input, float width, float height);
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

	// Fills a dropdown for the reference field kinds (Enum's options come from
	// the schema; CatalogRef's from the project). The empty string is prepended
	// by the dialog itself as "(none)", so a provider only returns real values.
	std::function<std::vector<std::string>(const FieldSpec&)> optionsFor;
	// A POOL asset field (TextureSet / Model) is picked in the asset picker, not
	// a dropdown — there are hundreds and a name tells you nothing. The owner
	// opens it (textures vs models, on `current`) and calls `apply` with the
	// chosen name; the dialog writes it into the working copy like any edit.
	std::function<void(bool textures, const std::string& current,
					   std::function<void(const std::string&)> apply)>
		onPickAsset;
	// Save: commit the working copy (the owner writes the catalog, then reloads
	// or re-bakes per Config::rebake).
	std::function<void(const Config&)> onSave;
	// Optional extra footer button — the per-category escape hatch to a
	// specialised dialog (Monsters: animations + behaviour). No label = no button.
	std::function<void(const Config&)> onExtra;
	std::string extraLabel;
	// Duplicate: clone this entry as a new type — "the same wall with a different
	// texture" is a copy plus one field, not a form filled from scratch. The owner
	// opens the CREATE dialog preset to Duplicate-of-this-id, so a clone still
	// lands through the one create path (id validation, schema defaults, palette
	// enrolment) instead of a second writer. Empty label = no button, which is how
	// a category that cannot author a type (Effects, which need a class) opts out.
	std::function<void(const Config&)> onDuplicate;
	std::string duplicateLabel;
	// Renaming: clicking the id in the title opens an inline edit; Enter commits
	// through this (the LevelSettingsDialog affordance). The owner does the real
	// work — catalog entry, every level's records, the cross-catalog references
	// — and returns false with a reason when it refuses. The dialog adopts the
	// new id on true.
	std::function<bool(const std::string& id, const std::string& newId,
					   std::string& problem)>
		onRename;
	// Delete: the owner refuses (false + reason) while anything still references
	// the type. Two clicks — the first arms the button, so a stray click on a
	// destructive action can't land.
	std::function<bool(const std::string& id, std::string& problem)> onDelete;

	// A refusal (or any note) to show under the form until the next edit.
	void SetNotice(std::string text) { m_notice = std::move(text); }

private:
	void BuildUI();
	// The field's current value: the entry's, else the schema default.
	std::string ValueOf(const FieldSpec& spec) const;
	// Records an edit (and marks the dialog dirty for Save).
	void SetValue(const FieldSpec& spec, std::string value);
	bool Touched(std::string_view key) const;

	gfx::GraphicsDevice& m_device;
	ui::UIContext m_ui; // the tabbed form
	const gfx::Texture* m_closeIcon = nullptr; // shared, owned by AssetUtil

	bool m_open = false;
	bool m_busy = false;     // a re-bake is running; freeze the form
	bool m_helpOpen = false; // the "?" field-explanation overlay
	bool m_uiRebuild = false; // deferred BuildUI (never Clear inside a callback)
	Config m_cfg;
	std::span<const FieldSpec> m_schema;
	// Keys the user changed — the ONLY ones Save writes (see the header note).
	std::vector<std::string> m_touched;
	ui::TabControl* m_tabs = nullptr; // owned by m_ui; the help overlay reads its tab
	std::vector<const char*> m_sections; // tab order, resolved from the schema

	// --- rename / delete ------------------------------------------------------
	std::string m_notice;      // refusal or note, drawn under the form
	bool m_editName = false;   // the title's id is an edit field right now
	bool m_nameHover = false;  // hover on the id (Update tracks, Render styles)
	bool m_deleteArmed = false; // the Delete button is one click from firing
	ui::TextField* m_nameField = nullptr; // valid until the next Clear
	// The id's pixel rect within the title line — the rename click target.
	gfx::Rect IdRect(float w, float h);
};

} // namespace dungeon::game
