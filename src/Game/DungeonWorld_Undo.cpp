// ============================================================================
// Game/DungeonWorld_Undo.cpp - split out of DungeonWorld_Editing.cpp to keep files
// small. Editor undo/redo: snapshot capture/restore, the deferred geometry
// flush and the undo step bracketing.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Assets/File.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"

#include <algorithm>
#include <cstdlib> // atof — the .ent `seconds=` override
#include <filesystem>
#include <format>
#include <optional>

using namespace DirectX;

namespace dungeon::game {
// ============================================================================
// Editor undo/redo. Snapshot-based — see the header for the design rationale.
// ============================================================================
DungeonWorld::EditorSnapshot DungeonWorld::CaptureEditorState() const {
	// Still AGGREGATE-INITIALIZED, and it has to be: DungeonMap's default
	// constructor is private (it exists for FromText alone), so the snapshot
	// cannot be default-built and filled in field by field.
	std::optional<WorldMap> worldCopy;
	if (m_worldForUndo) worldCopy = *m_worldForUndo;
	EditorSnapshot s{std::move(worldCopy), m_currentLevel,   m_map, m_entities,
					 m_entsDirty,          SnapshotActive(), {},    {}};
	// The map copy carries the live decoration placements as records, so the
	// restore's LoadDecorations rebuilds them (AddDecoration only appends a
	// live instance — same sync a level-swap stash does).
	s.map.SetDecorationRecords(LiveDecorationRecords());
	if (m_openX && m_openZ) {
		s.openX = *m_openX;
		s.openZ = *m_openZ;
	}
	for (const auto& [stem, map] : m_levelMaps)
		s.stashMaps.emplace(stem, std::make_unique<DungeonMap>(*map));
	for (const auto& [stem, ents] : m_levelEnts)
		s.stashEnts.emplace(stem, std::make_unique<DungeonEntities>(*ents));
	return s;
}

void DungeonWorld::RestoreEditorState(EditorSnapshot snap) {
	// THE WORLD, restored alongside the levels because an editor step may have
	// spanned both (adding a level to a dungeon touches the manifest, the
	// catalog and a doorway). One history means one restore.
	if (m_worldForUndo && snap.world) *m_worldForUndo = std::move(snap.world);
	// The opening, likewise (a stair move can carry it). Flagged as moved so
	// the next save writes project.ini back to whatever the undo left.
	if (m_openX && m_openZ && snap.openX != -2 &&
		(*m_openX != snap.openX || *m_openZ != snap.openZ)) {
		*m_openX = snap.openX;
		*m_openZ = snap.openZ;
		m_openingMoved = true;
	}

	// Cheap in editor mode (only sprite work is queued — the scene passes are
	// skipped while the full-screen editor is up), and still required: the
	// turbidity texture below is replaced, and an in-flight frame must not
	// read a freed resource.
	m_device.WaitIdle();

	// A palette add/remove rides the snapshot (the whole map does), but the
	// LOADED texture sets and worn meshes don't — they'd stay at the old count
	// and the variant indices would disagree. Compare before the map moves and
	// mark the surfaces for a full reload if the membership changed.
	const bool paletteChanged = m_map.WallPalette() != snap.map.WallPalette() ||
								m_map.FloorPalette() != snap.map.FloorPalette() ||
								m_map.CeilingPalette() != snap.map.CeilingPalette();

	// Static layer + records (move-assign like BeginLevelLoad — Party holds a
	// reference to m_map, so the object must persist, only its data changes).
	m_map = std::move(snap.map);
	m_entities = std::move(snap.ents);
	m_entsDirty = snap.entsDirty;
	// Remote-edited stashes are replaced wholesale: a level with no stash in
	// the snapshot reverts to file authority (its entry is simply gone).
	m_levelMaps.clear();
	for (auto&& [stem, map] : snap.stashMaps)
		m_levelMaps.insert_or_assign(stem, std::move(map));
	m_levelEnts.clear();
	for (auto&& [stem, ents] : snap.stashEnts)
		m_levelEnts.insert_or_assign(stem, std::move(ents));

	// The fog mask is parallel to the CELLS, and until the generator existed a
	// restore could never change their number — every snapshot was of this same
	// level. A regenerate can hand back a level of a different size, and undoing
	// one then indexed the old mask out of range. RESIZED rather than cleared, so
	// an ordinary same-size undo still leaves the fog exactly as it was.
	if (const size_t cells = static_cast<size_t>(m_map.Width()) * m_map.Height();
		m_seen.size() != cells)
		m_seen.assign(cells, 0);
	if (m_tracks.size() != m_seen.size()) FitTracksToMap(); // the tracks too (6g)

	// Dynamic layer: respawn from the records, then apply the captured live
	// diffs — the same flow a level re-entry uses (editor-placed monsters ride
	// the snapshot's whole-spawn rows).
	RespawnFromRecords();
	m_levelStates[m_currentLevel] = std::move(snap.state);
	ApplyActiveSnapshot();

	// Fires/turbidity from the restored fixtures. The SURFACE rebake is
	// deferred (m_geometryDirty → FlushGeometry): any cell may differ, so it
	// would be the full quality-swap rebuild — but the full-screen editor
	// hides the scene, so the stale chunks are never drawn, undo stays fast,
	// and a whole editing session pays for one rebake on the way out.
	// Same reason as the fog mask: the party is standing where the level it just
	// left put them, which the restored one may not have made floor at all. Only
	// moved when that is actually true, so an ordinary undo never teleports you.
	// A step that renumbered the squares (a resize) recorded where the party
	// stood; anything else leaves it where it is.
	if (snap.partyX >= 0 && snap.stem == m_currentLevel &&
		m_map.IsWalkable(snap.partyX, snap.partyZ)) {
		m_party.SetGridPosition(snap.partyX, snap.partyZ);
		MarkSeen(snap.partyX, snap.partyZ);
	} else if (!m_map.IsWalkable(m_party.GridX(), m_party.GridZ())) {
		m_party.SetGridPosition(m_map.StartX(), m_map.StartZ());
		MarkSeen(m_map.StartX(), m_map.StartZ());
	}

	RebuildFiresAndDust();
	m_geometryDirty = true;
	if (paletteChanged) m_surfacesDirty = true;
}

// The dynamic layer, rebuilt from whatever the records currently say. Shared by
// the undo restore (which replaces the records wholesale) and the type rename
// (which retypes them in place) — in both cases every live object has to be
// re-resolved through its kind cache, since the type it names may have moved.
void DungeonWorld::RespawnFromRecords(bool geometryToo) {
	m_monsters.clear(); // fresh runtimeIds; stale async AI plans find no match
	m_items.clear();
	m_buttons.clear();
	m_doors.clear();
	m_decorations.clear();
	m_walkableCache.reset();
	LoadDecorations();
	LoadStairs();
	LoadMonsters();
	LoadItems();
	LoadButtons();
	LoadDoors();
	// Fires are NOT rebuilt here: the undo path has to do it after applying its
	// dynamic diffs (which carry each fixture's lit state), so both callers own
	// that step themselves.
	// Wall features are stamped INTO the surface chunks, so retyping one only
	// shows after a re-stamp; deferred like the undo restore's.
	if (geometryToo) m_geometryDirty = true;
	// Every monster, door and prop in the world is a different object now — the
	// one-pipeline check's baselines point at freed storage (Game/DamageLedger.h).
	RebaseDamageLedger();
}

void DungeonWorld::FlushGeometry() {
	// A restored palette needs the heavier path: re-resolve, then reload the
	// worn meshes AND texture sets so the variant arrays match the palette
	// again (ReloadDungeonBlocks rebuilds the chunks itself).
	if (m_surfacesDirty) {
		m_surfacesDirty = false;
		ReloadDungeonBlocks(/*textureResChanged*/ true);
		return;
	}
	if (!m_geometryDirty) return;
	m_device.WaitIdle(); // in-flight frames may still read the old chunk meshes
	m_walls.chunks.clear();
	m_floors.chunks.clear();
	m_ceilings.chunks.clear();
	BuildDungeonMeshes(); // clears m_geometryDirty itself
	m_shadows.InvalidateCubes();
}

void DungeonWorld::BeginUndoStep() {
	m_pendingUndo = CaptureEditorState();
}

void DungeonWorld::CommitUndoStep(bool changed) {
	if (!m_pendingUndo) return;
	if (changed) {
		constexpr size_t kMaxUndoSteps = 64;
		m_undoStack.push_back(std::move(*m_pendingUndo));
		if (m_undoStack.size() > kMaxUndoSteps)
			m_undoStack.erase(m_undoStack.begin());
		m_redoStack.clear(); // a new edit forks history
		NoteEdit();
	}
	m_pendingUndo.reset();
}

void DungeonWorld::Undo() {
	if (m_undoStack.empty()) {
		if (onMessage) onMessage(loc::View("map.undo.none"));
		return;
	}
	EditorSnapshot redo = CaptureEditorState();
	EditorSnapshot snap = std::move(m_undoStack.back());
	m_undoStack.pop_back();
	if (snap.partyX >= 0) { // a resize: the way back has to know where it stood too
		redo.partyX = m_party.GridX();
		redo.partyZ = m_party.GridZ();
	}
	m_redoStack.push_back(std::move(redo));
	RestoreEditorState(std::move(snap));
	NoteEdit();
	if (onMessage) onMessage(loc::View("map.undo.done"));
}

void DungeonWorld::Redo() {
	if (m_redoStack.empty()) {
		if (onMessage) onMessage(loc::View("map.redo.none"));
		return;
	}
	EditorSnapshot undo = CaptureEditorState();
	EditorSnapshot snap = std::move(m_redoStack.back());
	m_redoStack.pop_back();
	if (snap.partyX >= 0) {
		undo.partyX = m_party.GridX();
		undo.partyZ = m_party.GridZ();
	}
	m_undoStack.push_back(std::move(undo));
	RestoreEditorState(std::move(snap));
	NoteEdit();
	if (onMessage) onMessage(loc::View("map.redo.done"));
}

void DungeonWorld::ClearUndoHistory() {
	m_undoStack.clear();
	m_redoStack.clear();
	m_pendingUndo.reset();
	NoteEdit(); // a level transition, rename or delete: the checked set moved
}


} // namespace dungeon::game
