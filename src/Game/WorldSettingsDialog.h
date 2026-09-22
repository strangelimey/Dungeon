// ============================================================================
// Game/WorldSettingsDialog.h — the editor's world settings modal (W4,
// docs/world-editor-plan.md). LevelSettingsDialog one tier up.
//
// Opened by the world screen's toolbar in Editor mode. Three tabs, which are
// the three console command families the model was built as — `worldprops`,
// `worldarea` and `worldloc` — wearing a face:
//   • World     — the world `start` cell, the game's OPENING (which dungeon
//                 and level a new game begins in, or the world map itself) and
//                 the harness level.
//   • Areas     — the named rectangles that override terrain danger, IN FILE
//                 ORDER with the order editable. Areas may overlap and the
//                 LAST row wins, so a table that sorted them would hide the
//                 only rule the format has, in the one place anyone edits it.
//   • Doorways  — the locations: where each stands on the world, what dungeon
//                 and level it opens onto, and where in that level it lands.
//
// THE DIALOG PROPOSES, THE OWNER DISPOSES — the same split WorldMapView's
// onPaint makes. It holds a borrowed const WorldMap* and READS it every
// rebuild, and every change leaves through a callback that returns whether it
// was allowed. So a refused edit is one the table visibly does not show, and
// there is no working copy that could disagree with the world about what
// happened. The refusals themselves live in WorldMap (SetStart, AddArea,
// AddLocation...), which is what makes this dialog and the console commands
// the same editor rather than two.
//
// Edits are LIVE and each is its own undo step; the footer Save writes them —
// the world to world.map, and the manifest fields to project.ini. Those last
// are NOT undoable and the dialog says so rather than pretending: the editor's
// history snapshots the world and the levels, not the manifest.
//
// A LOCATION'S ID IS NOT EDITABLE HERE. An exit stair's `dest` names it, and
// so does a save's `atLocation` and its discovered list — renaming one needs
// the reference sweep, which is a piece of work rather than a text field.
// ============================================================================
#pragma once

#include "Game/WorldMap.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h"

#include <functional>
#include <string>
#include <vector>

namespace dungeon::ui {
class Label;
class TabControl;
}

namespace dungeon::game {

class WorldSettingsDialog {
public:
	// What the dialog needs that is not in the WorldMap: the manifest's fields,
	// and the catalog's dungeons so a dungeon/level field can be a DROPDOWN of
	// what exists rather than a box you type a typo into. The checker rejects a
	// location naming a dungeon that has no such level; offering only the real
	// ones refuses that by construction instead of reporting it afterwards.
	struct DungeonInfo {
		std::string id;
		std::vector<std::string> levels;
	};
	struct Manifest {
		std::string startDungeon; // empty = a new game opens on the world map
		std::string startLevel;
		int startX = -1, startZ = -1; // -1 = that level's own start cell
		std::string evalLevel;        // the level the eval harness opens in
	};

	WorldSettingsDialog(gfx::GraphicsDevice& device, ui::FontLibrary& fonts);

	bool IsOpen() const { return m_open; }
	// `world` is BORROWED for the dialog's life — Game's, and modal input means
	// nothing can reload it underneath. `selectLocation` opens straight on the
	// Doorways tab with that row selected, which is what a right-click on the
	// world map wants.
	void Open(const WorldMap* world, std::vector<DungeonInfo> dungeons,
			  std::vector<std::string> levels, Manifest manifest,
			  const std::string& selectLocation = {});
	void Close() { m_open = false; }

	void Update(const Input& input, float width, float height);
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme, float width,
				float height);

	// --- the world (undoable, applied live) ---------------------------------
	// Each returns whether the world accepted it; the dialog re-reads either
	// way, so a refusal shows as a row that did not change.
	std::function<bool(int x, int z)> onSetStart;
	std::function<bool(const WorldMap::Area&)> onAddArea;
	std::function<bool(const std::string& id)> onDeleteArea;
	std::function<bool(const std::string& id, int index)> onOrderArea;
	// The whole row, id included: an area's id is a label nothing references,
	// so renaming one is an ordinary field edit (the owner refuses a clash).
	std::function<bool(const std::string& id, const WorldMap::Area&)> onEditArea;
	std::function<bool(WorldMap::Location)> onAddLocation;
	std::function<bool(const std::string& id)> onDeleteLocation;
	std::function<bool(const std::string& id, int x, int z)> onMoveLocation;
	// Everything about a doorway except where it stands (that is onMove, which
	// has the occupancy rule) and its id (see the header).
	std::function<bool(const std::string& id, const WorldMap::Location&)>
		onEditLocation;

	// --- the manifest (NOT undoable) ----------------------------------------
	// Applied to the in-memory project as they are edited, written by Save.
	std::function<void(const Manifest&)> onManifest;
	// The footer Save: world.map and project.ini.
	std::function<void()> onSave;

private:
	void BuildUI();
	void BuildWorldTab(size_t tab);
	void BuildAreasTab(size_t tab);
	void BuildLocationsTab(size_t tab);
	// The "+ Add" rows. Both pick a fresh id that steps past collisions, and
	// both place the new thing somewhere the world will ACCEPT — a doorway on
	// impassable or occupied ground is refused, so "+ Add" that landed it
	// anywhere would be a button that sometimes does nothing.
	void AddArea();
	void AddLocation();
	// The levels of `dungeonId`, or every level when it names nothing known —
	// a location pointing at a dungeon that has since been deleted still has to
	// show what it points at.
	const std::vector<std::string>& LevelsOf(const std::string& dungeonId) const;
	const WorldMap::Location* Selected() const;
	// Sets the caption under whichever control was last edited, by WRITING INTO
	// the label rather than rebuilding — a rebuild destroys the field being
	// typed in, and a two-digit number cannot be typed into a box that stops
	// existing after its first digit.
	void SetNote(std::string text);
	// The World tab's resting caption: what the start cell is standing on.
	std::string StartNote() const;

	gfx::GraphicsDevice& m_device;
	ui::UIContext m_ui;
	const gfx::Texture* m_closeIcon = nullptr; // shared, owned by AssetUtil

	bool m_open = false;
	bool m_helpOpen = false;
	// Deferred rebuild: a row that adds, deletes or reorders is inside the tree
	// walk when it fires, and UIContext::Clear from there dangles the caller
	// (the m_pendingLanguage convention). Field edits do NOT rebuild — that
	// would take the focus out of the box being typed in.
	bool m_uiRebuild = false;

	const WorldMap* m_world = nullptr; // borrowed; read every rebuild
	std::vector<DungeonInfo> m_dungeons;
	std::vector<std::string> m_levels;
	Manifest m_manifest;
	std::string m_selected; // the doorway whose form is shown, by id
	// The status caption: why the last edit was refused, or what the value it
	// changed now means. `m_noteLabel` is the Label showing it, BORROWED from
	// the live tree — valid until the next Clear, which is why BuildUI drops it
	// and each tab re-seeds it.
	std::string m_note;
	ui::Label* m_noteLabel = nullptr;

	ui::TabControl* m_tabs = nullptr; // owned by m_ui; kept to restore the tab
	int m_activeTab = 0;
};

} // namespace dungeon::game
