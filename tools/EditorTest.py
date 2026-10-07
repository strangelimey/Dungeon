# tools/EditorTest.py - the editor-updates branch's checks, checked
# (docs/editor-updates-plan.md).
#
# Run:  python tools\EditorTest.py      (needs a debug build)
#
# The eval harness REPORTS and never judges (docs/eval-harness.md); this is the
# judge. Each phase runs a script from tools\EvalScripts headless (phase 19 with
# its window: it measures what a frame DRAWS, the half -headless skips) and reads
# dungeon.log. Surfaces are read by GEOMETRY - `geomhash` fingerprints the
# level's walls/floors/ceilings - so "put back" means the hash matches the one
# taken before, not that a message said so.
#
#   1. A PAINT DRAG IS ONE UNDO STEP, even when its first square already had
#      the texture (the step used to be decided on the press and dropped, so
#      Ctrl+Z skipped the drag and undid the edit before it).
#   2. CHECKING DOES NOT CHANGE WHAT A SAVE WRITES: after a validate and two
#      `typerefs`, savemap writes the active level alone (the checker and the
#      type-usage count both used to stash every level, and a stashed level is
#      one savemap rewrites).
#   3. THE EDIT COUNTER live validation keys on moves on a change and STAYS PUT
#      on a no-op (a repaint of the same texture), and moves on undo.
#   4. BATCHED FILLS LEAVE NO CHUNK STALE: after a rectangle recolour, a
#      rectangle that raises walls across chunk edges, and a flood, the chunks
#      actually uploaded match a fresh bake (`geomlayout`); undo restores the
#      surfaces exactly.
#   5. THE AREA FILL paints the room or corridor, stopping where narrow meets
#      open: on a carved layout with known answers (a 154-square room, a
#      13-square corridor with a bend and a doorway, a 60-square room, the
#      corridor's 25 wall blocks, 227 floor squares in all) every count must
#      come out exact and every fill must leave the chunks current.
#   6. LIVE VALIDATION'S BOXES: two items walled into a pocket are one finding
#      but two boxes; a stair whose partner was deleted is boxed on its own
#      level AND at the far end on the other; a finding with no square (a
#      dungeon with no levels) is counted on the Check badge instead.
#   7. PAINTING WITH A THEME: an area fill with "marble hall" makes the
#      room's floors and ceilings and the walls around it REFERENCE it, every
#      one resolving to its one member per surface (the floor one the palette
#      had to enrol); the eyedropper picks the theme up; it survives a save and
#      a reload with the geometry unchanged; and "+ New..." seeds a theme
#      from the selected square's look.
#   8. EDITING A THEME reaches every square painted with it: a new floor
#      shows at once on the level in hand and on a level NOT loaded, and the
#      save that follows writes that level but not one that does not use it;
#      a rename reaches the squares, a delete is refused while any use it, and
#      renaming a member floor type keeps it resolving.
#   9. MAKING A WORLD three ways: blank from the template, this world whole
#      (its UNSAVED edit written into the copy, and this world's own file left
#      alone), and one level (its stairs replaced by one exit). Each refusal
#      says its own reason; a half-built `.building-*` folder is never listed;
#      each new world opens by name and passes the checker as it stands; and
#      (code-review C323) a new world's flags, quests and dungeons catalogs
#      keep their documentation without the source's entries, while a
#      one-level world keeps none of its source's manifest comments.
#  10. THE NEW WORLD DIALOG: its "Copy one level" makes a one-level world of
#      the level picked; a name in use is refused in its own words; and a world
#      made over the Worlds dialog lands in that list ARMED.
#  11. THE WIZARD: the template's content and one generated floor. The same
#      knobs and seed make an identical floor, another seed a different one;
#      the tag holds the monsters to it; the size is the map's; there
#      is a way out; the template's flags.cat documentation comes across; and
#      each world opens by name and passes the checker.
#  12. THE PALETTE'S CATEGORY BAR (tool-refinement Phase 1): both groupings
#      are the designed tables, each group lists exactly its sections, effects
#      are in none; the world sections list their entries; a filter from inside
#      one group finds matches in the others and drops sections with none; a
#      filter is capped in characters, never cut inside one (C383); and the
#      grouping survives a restart through settings.ini.
#  13. MONSTER POWER (Phase 2): unset, a kind's power is its derived threat;
#      the bands are the fifths of the project's range (recomputed here, not
#      read back) and the palette's rows wear them; an override moves that
#      kind, re-cuts every band, and moves the generator's pick - a control
#      level has the swarm, the same level after the override has none, and a
#      boss is exactly one swarm; removing the override restores everything.
#  14. THE DOCKS AND THE OVERVIEW (Phase 3): a dock takes the width it is
#      dragged to and everything beside it follows (strip, grid, palette
#      body); both clamps hold; the widths survive a restart; the overview's
#      level / dungeon / world counts equal the PROJECT FILES' (read here, not
#      from the game); and a monster placed on the viewed level counts at once
#      and uncounts on undo.
#  15. FLAGS (Phase 4), on a fixture written into eval_arena: a door waiting on
#      a flag stays sealed and a lever waiting on it stays put until a lever
#      sets it; an item's hook sets one; the save carries them; a stair waiting
#      on one bars the way until it is on, then goes down; the checker names a
#      wait nothing satisfies, a flag flags.cat lacks, a dungeon's flag used in
#      another and a flag nothing touches - and nothing about the sound ones;
#      the inspectors' setters rewrite the records; the overview counts flags
#      per scope; and the palette's Quest items & flags section lists this
#      dungeon's and the world's (not another dungeon's), says where a quest
#      item lies, arms it, goes there and opens a flag's editor.
#  16. STYLES (Phase 5): the world's and the library's are listed apart; the
#      armed style ranks the Monsters section by its list and disarms again;
#      adding a library style copies it, its themes and the one floor the
#      world lacked - and nothing the world had - and a second add is a no-op;
#      a world style saves to the library with what it names; the checker
#      names a monster a style lists and the world lacks; and renaming a theme
#      or a flag rewrites the styles, levers, stairs and items that name it.
#  17. THE SHAPE BRUSHES (Phase 6), on eval_arena turned to rock round one
#      room: a corridor opens the rock it crosses, reaches its far end, and
#      wears the style's corridor theme on its floor and walls - and undo puts
#      the geometry back exactly; a room opens its rectangle in the room theme;
#      a stamp raises its pillars, never on the party; a region generates
#      inside its box joined to the room it touches; no style = plain carving;
#      a region under 6x6 makes nothing.
#  18. THE WORKFLOW, WIRED THROUGH (Phase 7): a blank world made in a library
#      style receives it (and its themes), its dungeon names it and its first
#      room wears it; a wizard world in a style lays both themes and draws the
#      style's monsters; both pass the checker. Then the walk, inside the new
#      world: [+] opens on the dungeon's style; Create and Empty each land in
#      Build with the style armed, the level tagged and themed with no Level
#      settings visit; a room, a corridor and a stamp build; Populate fills the
#      hand-built floor from the style's list; and the overview's "what next"
#      reads build -> populate -> check/ready, counting what the files hold.
#  19. THE SPRITE ARENA HOLDS THE LARGEST MAP (code-review C163): a 128x128
#      floor - the generator's largest - generated with the editor open and
#      viewed at fit zoom, where every square is one quad on screen, draws with
#      NOTHING dropped, and the frame really carried the whole cell layer (its
#      bytes are read, so a frame that drew nothing cannot pass). Then the floor
#      is resized to 256x256, past the arena: the cell layer is dropped and
#      COUNTED - in all, and per frame (two readings over the same view: the
#      frame's count holds while the total grows) - and the game carries on to
#      its verdict - it used to abort.
#  20. THE TYPE EDITOR (code-review C101, C100, C235): every category opened
#      the way the palette opens it builds a CONTROL for every schema row -
#      read off the widget tree, so a kind the dialog's switch does not know
#      (a damage type, once) shows as a row with nothing; a quest stage renamed
#      stage2 -> stage10 a keystroke at a time is refused at "stage1" in the
#      notice and keeps stage1's line, in the working copy, the save and the
#      entry read back; and, in a WINDOWED run (the fault was in the drawing),
#      a theme open on its Floors tab through two quality changes leaves no
#      error in the log - its swatches used to be freed textures.
#  21. CATALOG SWEEPS AND WRITES (code-review C305, C306, C323): a floor and a
#      ceiling feature type renamed take every record with them - on the level
#      in hand and on one not loaded, in memory and on disk - with the geometry
#      unchanged, and a delete of either is refused while they are placed; a
#      rename or delete of an effect, attack, spell or the balance sheet is
#      refused and leaves their files as they were, and the type editor on an
#      effect answers Delete with why; and catround's three cases - a catalog
#      with no entry yet, the first entry deleted, the monster dialog's Save -
#      keep every comment.
#  22. THE MAP'S MONSTER ICONS STAND IN THEIR IDLES (code-review C183 / C189):
#      every catalog kind loaded and baked in one pass. Read from what the BAKE
#      RECORDED drawing (MonsterKind::iconDrawn), never the inputs it was handed:
#      each icon is drawn in its kind's idle (monsters.cat `anim_idle`, read
#      here) with that pose's own palette (fingerprinted against the idle's first
#      frame on a fresh animator), and its head shot is framed on that pose's
#      box - the bought kit's four rigs come out narrower than their T-pose. And
#      each rig is drawn with its OWN skinning palette:
#      the pass uploads one per skinned kind, among them several the same size
#      (the renderer knows a palette by its address, and a throwaway animator
#      per bake handed eight of sixteen kinds another kind's upload).
#  23. THE MONSTER INSPECTOR AND PATROL ROUTES (code-review C80, C232, C233,
#      C104, C99): a route finished after another monster was inspected
#      reopens on ITS monster with its waypoints; the player map's keys are not
#      the route's; Clear route keeps the Patrol tab; a Caster picked in the
#      inspector and in the monster dialog shows the spell it saves, and a new
#      process loads both with no warning, each dialog OPENING on that spell;
#      with the spells taken out of the files the load warns for both, each
#      dialog opens on none yet holds and shows the first spell offered, and
#      Esc puts the spell-less original back; a route laid before a world switch
#      is gone after it, with the next world's level files untouched though its
#      monster holds the same id; and, WINDOWED, saving the route's monster
#      type ends the route (Enter reopens nothing) and closes an open
#      inspector and the monster dialog, with no fault in the log.
#  24. ESC BACKS OUT ONE LAYER (code-review C81), every Esc a real key through
#      the frame's input (`presskey esc`): with a drop-down open in a monster
#      inspector, in Level settings, the Generate and the New world dialogs and
#      the create dialog, the first Esc closes only the list - each dialog
#      stays, the inspector's Caster edit and the previewed dust kept - and the
#      second closes the dialog (the inspector's and Level settings' putting
#      their edits back; the create dialog had no Esc at all); on the editor
#      map an Esc closes the level list before it puts the brush down, and
#      only then the editor; on the travel screen it puts the terrain brush
#      down before it pauses.
#  25. A SURFACE'S RELIEF IS ITS SET'S OWN (code-review C406): unset, a wall,
#      floor and ceiling type's relief row names the relief the baker's record
#      gives its texture set (`AssetBaker wornsets`), not the old per-kind
#      default; a set no record lists names its kind's default; a type that
#      sets one gets a slider and still names the set's.
#  26. THE INSPECTORS' TEXT IN THE PLAYER'S LANGUAGE (code-review C107): a
#      right-click's "what is on this square" line, under `lang de`, is word for
#      word the line de.lang builds - the base, the monster and prop counts with
#      their plural and singular keys (two skeletons authored on one square give
#      the plural monster key; the editor places one a square), on the active
#      level and on a browsed one (which says the base alone) - where it used to
#      splice English words into the German pattern; a thrown torch's projectile
#      card words its units and its burn line as de.lang does (they were English
#      in every language), and as ru.lang does, since de.lang's "m/s" and "m"
#      are English's own and only Russian tells those rows apart; English
#      likewise for both; no raw key on either; and `editor cell`, the dev
#      readout every other phase parses, stays English under German.
#  27. "USE INSTALLED" NEVER RE-BAKES A SHARED SET (code-review C407): a set's
#      worn meshes are one file per set, shared by every type in every world, so
#      the create dialog's FORM refuses a set painted as another surface kind -
#      by its shipped record, by a type in this world (one whose set has worn
#      meshes and no record among them), by a type in ANOTHER world, by the
#      import that baked it - before Create, with onCreate never asked; a pick
#      that goes stale (a wall type renamed onto it after the pick) is refused
#      by onCreate's own check, and the form stays open saying why; the longest
#      refusal wraps inside the footer (uioverlap, WINDOWED); a same-kind create
#      of a set whose meshes exist starts no bake, and a set nobody paints with
#      no meshes is baked, as the new kind - the only AssetBaker any create
#      starts. Every worn mesh in the pool is fingerprinted before and after
#      (the bake waited out): none changes and none appears but the fresh set's.
#  28. `editor place` TAKES THE VIEWED LEVEL'S FREE FACE (code-review C449): a
#      wall kind placed with no face named hangs on the first FREE solid face
#      of the level being VIEWED - on the active level past a face its own
#      sconce holds, on a browsed level past the face its authored sconce holds
#      and where the active level's square has no wall at all, a niche on one
#      face then the other - and a square with no free face is refused. An
#      `editor erase` there frees its face for the next default, since the
#      command rebuilds the browsed snapshot after an erase as after a place.
#      Read off the console's answers AND the saved records, which gain exactly
#      the placements and nothing else; the old pick (the ACTIVE map's first
#      solid face, taken or not) refused every one of them, and an erase that
#      left the snapshot stale refused both placements after it.
#  30. A WORLD SWITCH CARRIES NO QUESTION AND RUNS UNDER NO BAKE (code-review
#      C115, C234; 28 and 29 are left for other work): an exit's question asked
#      before `worlds load` is gone in the next world, taken down by the unload
#      itself (its log line names that ending, not the new game's), and a Yes
#      is refused; a switch asked while a bake runs is refused - a "Use
#      installed" bake, and an IMPORT's two runs (its maps, then its worn
#      meshes; Browse handed a folder of one small albedo map this phase
#      writes), refused in each - and every bake lands in the world that
#      started it: on disk the other world's manifest and catalogs, imports.cat
#      among them, are byte for byte as they were, while this one's floors.cat
#      gains the two types and its imports.cat the import's one record (the
#      installed set's type records none), and nothing else moves.
#  31. THE STASH RULES (code-review C308, C298, C307): a level stashed is a
#      level savemap rewrites, so only an EDITED one may be. A walk eval_arena
#      -> crypt1 -> crypt2 -> crypt1 stashes no map and no .ent and its save
#      writes crypt1 alone; so do a dungeon delete asked and one cancelled, and
#      a level rename (which writes crypt1, the one level whose stair it
#      repoints) - crypt2's and eval_arena's files byte for byte as made. A
#      wall painted on crypt1 and then a save LOADED on another level: the
#      paint is stashed, saved and found again on the way back (the measure
#      reads edited, clean after an undo, edited again). On a browsed level an
#      erase of nothing, on a level with no stash yet, stashes neither layer, a
#      wall paint stashes the map and not the .ent, an item erased stashes the
#      .ent and not the map - and the files say so: crypt2.ent and crypt1.map
#      untouched. A wall painted on crypt1, the level PARKED by a walk out to
#      the world and a save made there loaded back IN PLACE: nothing of crypt1
#      is left stashed, so with the square put back as filed a savemap writes it
#      once, the stairs stash nothing and the wall does not come back; then a
#      wall painted and a NEW GAME begun elsewhere: the paint is stashed, saved
#      and found again.
#  32. THE RECORDS TELL THE TRUTH (code-review C326, C327, C311, C355). After a
#      fight - a monster killed, others woken and off their squares - savemap
#      writes eval_arena's monster lines byte for byte as filed (it wrote where
#      each STOOD). A monster placed on a browsed level, after that level's last
#      record (a monster the party killed) was erased, arrives alive where it was
#      placed (it was handed the erased one's id, and its death). An undo after
#      an erase brings the erased monster back no more, and a type Save and a
#      Rename keep everything the editor and play did to the level - a placed
#      monster and prop, an opened door, a dropped potion, a dead monster, an
#      inspector's archetype - and the save after writes them; a delete of a type
#      only an editor-placed monster uses is refused (it counted records). And under
#      `pipelineguard strict` every way the editor removes a monster, a prop or
#      a door (the erase ladder, an inspector's Delete, a wall painted over one)
#      reports no violation, each staged so the object that slides up has other
#      hit points than the one it replaces.
#  33. PLACEMENT ON A BROWSED LEVEL (code-review C310, C344, C351), crypt1
#      browsed from eval_arena: a window bored from a face of crypt1 bores
#      crypt1's block, into its stash (the brush bored the ACTIVE level's), and
#      the middle-click erase takes it (the browsed ladder had no bore rung).
#      Two banners hung on crypt1, then the walls behind them opened: one
#      re-hangs on its cell's other wall, the other has none and goes - in the
#      saved .map, where no `wall=` record faces open floor - and after a reset
#      the Check, a browse and the live check parse that file (a record left
#      facing floor asserted there, every build). And an ITEM INTO A NICHE, by
#      the brush: the pointer over a niche's block picks its face (`editor
#      ghost`, MapView's own hover - the item brush tracked no face) and the
#      ghost and the click put it in the niche, on eval_arena and on browsed
#      crypt1 alike, each record carrying `niche=`; a face with no niche is
#      refused, a floor square takes a floor item, and `editor place` naming
#      a wall with open floor behind it is refused (it laid a floor item there
#      and said it went into the niche).
#  34. THE BRUSH BY ID, AN ERASE OF NOTHING, THE START SQUARE (code-review C352,
#      C353, C348): crypt1 and eval_arena given different fifth floors, the
#      brush armed on eval_arena's (slate) and crypt1 browsed - no row of
#      crypt1's lights, and a click paints SLATE there, enrolled (the brush was a
#      row number, lit crypt1's fifth row and painted that). A paint undone, then
#      a middle-click erase of an empty square - refused, said as nothing to
#      erase - and the redo still brings the paint back, live and on a browsed
#      level. A wall on the start square is refused, live and browsed, and a
#      wall rectangle over it keeps it open, saying so once; a crop that leaves
#      out the start is refused naming it.
#  35. THE TYPES THE GAME AUTHORS ARE FOUND BY FLAG (code-review C333, C334):
#      wooden_door renamed oak_door, and a floor generated with locks:1 and
#      played has an oak_door for its lock (the generator wrote the literal
#      `wooden_door`, and entering the floor aborted on its model); deleted,
#      the lock is the next offered door with an opener (stone_door); a door
#      marked `lock = 1` wins (the portcullis); with none of either, no lock and
#      no door. stairs_exit renamed gate_stair, and an ambush's way out is a
#      gate_stair the party arrives on, facing open floor (the literal aborted
#      the ambush); and the last exit type's delete is refused as the last,
#      where a second one's is allowed.
#  40. ONE WORLD TICK (code-review C78, C125), read off the world's own update
#      count (`worldclock`): a paused editor stays paused through a bare
#      `editor`, `editor pick` and `editor issues` - each asks for Editor mode,
#      with the pause pressed again before each, so each fails on its own (a
#      real flip still clears it), and the run leaves settings.ini as it found
#      it; the open console holds the world over the paused editor and over an
#      editor dialog, and runs it over the character sheet (it used to run the
#      first two and freeze the third); and a stair stepped onto under the
#      console is followed there - an exit's question goes up and holds the
#      world, a stair down lands the party on crypt2 with the console still up
#      (the console used to leave either latched).
#  41. THE EDITOR MAP'S WHEEL AND TOOLBAR (code-review C373, C374), by the mouse
#      (`pressmouse`, aimed from mapaim.eval's readings): a wheel zoom near the
#      map's far corner keeps the map point under the pointer at every notch in
#      and out (it slid ~5% of its distance from the centre each notch - the
#      inline zoom left out the edge-handle margin), and a notch at zoom 1 or 10
#      leaves the pan exactly as it was (it kept sliding the map); a click on the
#      Level button opens Level settings with nothing left hovered under it (the
#      button stayed lit, its tooltip under the dialog's dim), and under the
#      same dialog opened by the CONSOLE a drawn frame leaves no square hovered
#      (the Render-path ClearHover; run WITH the window, which a headless run
#      never draws).
#  50. A MODEL LOADS AS .gltf OR .glb (code-review C301): a weapon made from a
#      model installed only as .gltf (an item's loader opened .glb) and a
#      decoration from one only as .glb (a decoration's opened .gltf; no other
#      type of the scratch world names it, so its file is first opened by the
#      place) resolve, the decoration placed and saved; the weapon wears its
#      `texture` (an item's was never read - an imported one drew white); the
#      create dialog's form, `typeset` and the type editor's own Save refuse a
#      model installed as neither - and taking a decoration's model away, which
#      falls back to its id - writing nothing. Then a RELOAD in a second
#      process, WINDOWED: the world load (every item kind) and the level load
#      (the decoration) both open their files, levelcheck passes, and with the
#      weapon on the floor, at the tier in force and through two quality swaps,
#      the albedo its part's DRAWS were handed (DrawPart's stamp, not a material
#      worked out for the readout) and the details preview's is its set's own.
#  51. THE KIND CACHES (code-review C302, C330, C347): a dagger's damage saved
#      through the type editor shows in its details dialog at once (an item's
#      kind is rebuilt in place; a save used to clear a DECORATION's cache entry
#      of that id instead) - the save closes the details it was made under
#      (their preview held the model the rebuild frees), and a dagger in the air
#      across it still reads as a dagger through the kind its flight holds, then
#      lands as one; a rune tablet under an id that is not rune_<symbol>
#      draws as its rune - the carved tablet in a socket, the glyph where a
#      control asks - and memorizes, once; rune_light renamed, a new game's
#      casters carry the renamed tablet and `rune light` gives it, and the
#      Sowilo light's effect icon keeps its glyph though its `icon` named
#      rune_light (a ward wears its Protect by its class, an item override
#      still wins, a burn is the tinted square); a door and a
#      decoration of ONE id, `portcullis`, placed side by side, are two kinds,
#      each with its own catalog's model and set; and create, duplicate and
#      rename refuse an id a related catalog holds (an item's in another item
#      catalog, a prop's in another prop catalog), each in its own words, beside
#      a free id that is accepted, writing nothing refused.
#  52. A SURFACE'S RE-BAKING SAVE WAITS FOR ITS BAKE (code-review C346), and an
#      IMPORT ALWAYS SAYS ITS FLIP (C393). `typeset walls <id> texture <set>`
#      with a set nobody paints bakes its worn meshes (it wrote the field with no
#      bake at all); while the bake runs - HELD, `bake hold`, so the reads cannot
#      race it - the catalog still names the old set and a second such save is
#      refused, and landed clean it names the new one. A bake that FAILS (its low
#      tier planted read-only, so the baker exits 1) writes nothing, through
#      `typeset` and through the dialog's Save, whose form stays open saying why;
#      the dialog's Save of a set nobody paints stays up, frozen, while its bake
#      runs and CLOSES once it lands, the field written (the `typeset` landing
#      has no dialog to close, so it cannot judge that branch); a shipped floor set as a wall's texture is refused before any bake; a
#      field that bakes nothing is written at once. On disk the failed type's
#      block is exactly as written. Then the create dialog's import of a normal
#      map with "gl" inside a word is sent --no-flip-green and one ending in the
#      GL token --flip-green - each imports.cat record saying so, each packed
#      _n.png's green unflipped / flipped.
#  55. A LEVER'S REVEAL SWAPS IN PRE-BUILT WALLS (code-review C211; phases 28-54
#      are other lanes'): eval_arena's lever wired to nothing moves no wall; the
#      one naming the hidden niche opens it, shuts it, and shuts it again after
#      the niche was opened by hand; and reveals it after the wall it is cut into
#      was repainted - each time with the walls on show matching a fresh bake
#      (`geomhash`'s layout line) and the look on show flipped in BOTH chunks the
#      niche reaches (`niche looks`); a real new game (`newgame`, not `reset`,
#      which re-reads the map) shuts it and starts the looks over, and a load of
#      a save made with it open restores it - the batched re-stamps of
#      ResetForNewGame and ApplyActiveSnapshot. A pillar written into the scratch
#      eval_arena puts a niche beside a chunk with NO walls: that chunk holds no
#      look, and no press in the run rebuilds walls in play. AllocTest -Lever
#      measures the corner's presses' frames.
#  60. THE DOOR INSPECTOR'S OPEN (code-review C356): it edits the AUTHORED
#      state, and the leaf follows only when it can - a smashed door's Open
#      ticked then unticked leaves the wreck standing open (it used to shut it,
#      and then it would not open again) while the record takes shut, and the
#      same untick on a door with a skeleton in its doorway, picked from the
#      chooser, is REFUSED, record and all: the box ticks itself again, the leaf
#      stays open on the skeleton, Esc's revert is refused too, and the level
#      saved holds open=1 beside the skeleton's record (one that took the close
#      shut the door on it at the next load); each says why. The control: with
#      the doorway empty the untick does shut the leaf.
#  61. A MONSTER MADE IN THE EDITOR IS LEASHED FROM ITS OWN SQUARE (code-review
#      batch 81's find): one placed with the brush and one made by `spawn` are
#      each anchored on the square they were made on (`leash`), the placed one
#      keeps its anchor when the move tool moves it, and both keep theirs
#      through a level re-entry and a save loaded - each used to read 0,0, the
#      struct's default, since only the .ent loader set the anchor. Given a
#      leash of 1 in its inspector and shoved three squares west by a gust, the
#      placed one walks back EAST to its square (it went on toward the corner);
#      and the level saved (the inspector's Save, then savemap) writes no
#      `leashfrom=` on any monster line, where it wrote `leashfrom=0,0` for both.
#      The checker flags a `leashfrom=` the judge plants on rock (0,0) and off
#      the map (99,99) in crypt2.ent, and not one on open floor - and on crypt2
#      that open-floor plant is anchored where its record says, not on its spawn.
#
# NOTHING HERE EDITS THE REAL WORLD (code-review C431). Each phase starts on a
# FRESH SCRATCH COPY of dungeon-demo, et_demo, and every run opens it with
# -project; the phases used to edit dungeon-demo itself and put it back with a
# delete then a copy, so a run killed mid-phase left it changed - and the next
# run began by deleting the backup that was its only clean copy. Now a killed
# run leaves a scratch folder, which the next run clears. The style library has
# one fixed home (assets/library), so phase 16 still changes it - behind a
# backup that only the restore that used it deletes (harness_game.back_up). The
# saves phases 15 and 55 make are this worktree's (harness_game.save_name). The
# run ends by checking the real worlds and the library are byte for byte as it
# found them - BEFORE it cleared up after a killed run, so the clean-up is judged
# too - and that git status shows nothing new and nothing of this judge's worlds.
import hashlib
import io
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import time
import zlib

# A detail line may quote Russian (phase 26's card): on a console whose code
# page cannot show it, a \u escape - not a UnicodeEncodeError in place of the
# verdict.
sys.stdout.reconfigure(errors="backslashreplace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECTS = os.path.join(ROOT, r"assets\projects")
SCRATCH = "et_demo"
PROJ = os.path.join(PROJECTS, SCRATCH)
LIBRARY = os.path.join(ROOT, r"assets\library")
LIBBAK = os.path.join(ROOT, r"build\editortest-library")
EXE = os.path.join(ROOT, r"build\debug\bin\Dungeon.exe")
LOG = os.path.join(ROOT, r"build\debug\bin\dungeon.log")
SCRIPTS = os.path.join(ROOT, r"tools\EvalScripts")
COPIES = os.path.join(ROOT, r"build\harness-scripts\editortest")
# Every world a phase makes (and the half-built one phase 9 plants), with the
# scratch copy: cleared at the start (what a killed run left) and at the end,
# each with the .building-<name> a create killed half-way leaves.
WORLDS = (SCRATCH, "nw_blank", "nw_copy", "nw_level", "nw_bad", ".building-nw_ghost",
          "nwd_level", "nwd_blank", "wz_a", "wz_b", "wz_c", "wz_undead", "wz_dlg",
          "p7_world", "p7_wiz", "et_next", "et_bind", "et_other")
# The worn meshes phase 27 puts in the pool (its fixture copy and the one bake it
# starts) and a bake it must never start would write: its sets are all et_-named,
# so no real set's file can match.
MODELS = os.path.join(ROOT, r"assets\models")


def own_worn():
    return [f for f in os.listdir(MODELS) if f.startswith("worn_et_")] if os.path.isdir(MODELS) else []


# ...and phase 30's import: the set it installs in the texture pool (et_-named
# too) and the folder of source maps it hands the create dialog's Browse.
TEXTURES = os.path.join(ROOT, r"assets\textures")
IMPORT_SRC = os.path.join(ROOT, r"build\editortest-import")


def own_textures():
    return [f for f in os.listdir(TEXTURES) if f.startswith("et_")] if os.path.isdir(TEXTURES) else []

# Never a stale exe, and never beside this worktree's own game, which shares
# the log every phase reads (tools/harness_game.py).
import harness_game
harness_game.refuse_if_stale(EXE)
harness_game.refuse_if_running(EXE)

# flags.eval's, stashload.eval's, stashpark.eval's, nichelooks.eval's and
# leashanchor.eval's save slots, renamed to this worktree's: the saves folder is
# shared with every other session and with Michael's own play.
SAVES = {"flagtest": harness_game.save_name(ROOT, "flagtest"),
         "stashtest": harness_game.save_name(ROOT, "stashtest"),
         "stashpark": harness_game.save_name(ROOT, "stashpark"),
         "nichelooks": harness_game.save_name(ROOT, "nichelooks"),
         "leashtest": harness_game.save_name(ROOT, "leashtest")}


def cleanup():
    """Every scratch world and save this judge makes, and the library put back
    if a backup of it is standing. Safe to run twice."""
    harness_game.recover(LIBRARY, LIBBAK)
    for w in WORLDS:
        harness_game.remove_world(ROOT, w)
    harness_game.remove_saves(SAVES.values())
    for f in own_worn():
        # Phase 52 plants one READ-ONLY (a bake that cannot write it), which a
        # killed run leaves so: writable first, or it cannot go.
        os.chmod(os.path.join(MODELS, f), stat.S_IREAD | stat.S_IWRITE)
        os.remove(os.path.join(MODELS, f))
    for f in own_textures():
        os.remove(os.path.join(TEXTURES, f))
    shutil.rmtree(IMPORT_SRC, ignore_errors=True)


# The guard is taken BEFORE clearing up what a killed run left, so a clean-up
# that does damage fails the run instead of becoming its baseline: the library
# must end as a standing backup holds it, and nothing of this judge's worlds may
# be left in git status, a killed run's included.
real = harness_game.RealTree(ROOT, own=WORLDS, backups={LIBRARY: LIBBAK})
cleanup()

# Muted for the whole run (tools/harness_audio.py), and cleaned up however it
# ends. The phases are flat, not one try block, so both ride atexit - which also
# runs after sys.exit, an uncaught exception and Ctrl+C (not after a kill: the
# next run's cleanup() above is for that).
import atexit
import harness_audio
atexit.register(harness_audio.restore, harness_audio.mute(os.path.dirname(EXE)))
atexit.register(cleanup)

failures = 0


def fresh():
    """Each phase starts on a fresh copy of dungeon-demo."""
    harness_game.scratch_world(ROOT, SCRATCH)


def drop():
    """...and its scratch world goes with it."""
    harness_game.remove_world(ROOT, SCRATCH)


def run(script, project=SCRATCH, headless=True, timeout=600, words=None):
    # -project opens the scratch world, never the real one (nor whatever world
    # the developer last switched to). A run that died before its verdict
    # counts as a failure on its own, not as a log to be read as if it were
    # whole. `headless=False` for a phase that needs the window to draw (the
    # sprite arena is only filled by a frame that renders). `words`: a script's
    # placeholder words and what this run puts there (harness_game.eval_script).
    global failures
    path = harness_game.eval_script(os.path.join(SCRIPTS, script), COPIES, SAVES, words)
    code, log = harness_game.run_eval(EXE, ROOT, LOG, [path], ["-project", project],
                                      timeout=timeout, headless=headless)
    if harness_game.report_unfinished(code, log, script):
        failures += 1
    return log


def check(ok, label, detail=""):
    global failures
    print(f"  {'[ok  ]' if ok else '[FAIL]'} {label}")
    if not ok:
        failures += 1
        if detail:
            print(f"         {detail}")


GEOM = re.compile(r"console: geomhash (\S+) walls=(\w+) floors=(\w+) ceilings=(\w+)")


def hashes(log):
    """Every geomhash line, in order, as (level, walls, floors, ceilings)."""
    return [m.groups() for m in GEOM.finditer(log)]


def passed(log):
    return "eval BATCH RESULT=PASS" in log


def console_sections(log):
    """Every console line (not only `editor` ones), split on '--- name ---'."""
    out, name = {}, None
    for line in log.splitlines():
        if "console: " not in line:
            continue
        text = line.split("console: ", 1)[1]
        m = re.match(r"--- (.*) ---$", text)
        if m:
            name = m.group(1)
            out[name] = []
        elif name is not None:
            out[name].append(text)
    return out


# The editor's own state (palette grouping, dock widths) lives in settings.ini
# beside the exe; the phases that change it put the developer's copy back.
SETTINGS = os.path.join(os.path.dirname(EXE), "settings.ini")


# --- phase 1: a drag is one undo step ----------------------------------------
print("1 - a paint drag is one undo step")
fresh()
log = run("strokeundo.eval")
check(passed(log), "the script ran clean")
h = hashes(log)
if len(h) != 5:
    check(False, "five geometry readings", f"got {len(h)}")
else:
    h0, h1, h2, u1, u2 = h
    # Non-vacuous: each edit must actually MOVE the geometry, or "undo put it
    # back" would be satisfied by edits that did nothing.
    check(h1 != h0, "the place changed the floor")
    check(h2 != h1, "the drag changed the floor again")
    check(u1 == h1, "undo takes off the drag ALONE", f"{u1} vs {h1}")
    check(u2 == h0, "a second undo takes off the place", f"{u2} vs {h0}")

# --- phase 2: a check leaves the save alone -----------------------------------
print("2 - checking does not change what a save writes")
fresh()
try:
    log = run("validatesave.eval")
    check(passed(log), "the script ran clean")
    ran = "console: > validate" in log and "console: > savemap" in log
    check(ran, "both the check and the save ran")
    m = re.search(r"console: saved levels: (.*)", log)
    saved = [s.strip() for s in m.group(1).split(",")] if m else []
    # The level the harness opens in (project.ini eval_level) is the active one.
    check(saved == ["eval_arena"], "savemap wrote the active level alone",
          f"saved: {saved}")
finally:
    drop()

# --- phase 3: the edit counter ------------------------------------------------
print("3 - the edit counter moves on a change and only on a change")
fresh()
log = run("editrev.eval")
check(passed(log), "the script ran clean")
revs = [int(r) for r in re.findall(r"console: editor rev (\d+)", log)]
if len(revs) != 4:
    check(False, "four counter readings", f"got {revs}")
else:
    r0, r1, r2, r3 = revs
    check(r1 > r0, "a paint moves it", f"{r0} -> {r1}")
    check(r2 == r1, "repainting the same texture does not", f"{r1} -> {r2}")
    check(r3 > r2, "an undo moves it", f"{r2} -> {r3}")

# --- phase 4: batched fills leave no chunk stale ------------------------------
print("4 - a batched fill rebuilds every chunk it touched")
fresh()
log = run("chunkbatch.eval")
check(passed(log), "the script ran clean")
h = hashes(log)
layouts = re.findall(r"console: geomlayout \S+ fresh=\w+ live=\w+ (\w+)", log)
if len(h) != 5 or len(layouts) != 5:
    check(False, "five readings", f"got {len(h)} hashes, {len(layouts)} layouts")
else:
    # Non-vacuous: each fill must have changed the geometry, or "no chunk is
    # stale" would hold for fills that touched nothing.
    for i, label in ((1, "the recolour"), (2, "the wall raise"), (3, "the flood")):
        check(h[i] != h[i - 1], f"{label} changed the level")
        check(layouts[i] == "match", f"after {label} the uploaded chunks match a fresh bake",
              layouts[i])
    check(h[4] == h[0], "undoing all of it restores the surfaces exactly")
    fills = re.findall(r"console: editor fill: (\w+) \S+ in ([\d.]+) ms", log)
    print("         fill times (ms): " + ", ".join(f"{k} {t}" for k, t in fills))

# --- phase 5: the area fill ---------------------------------------------------
print("5 - the area fill paints the room or corridor, and stops there")
fresh()
log = run("areafill.eval")
check(passed(log), "the script ran clean")
areas = [int(n) for n in re.findall(r"editor: Filled the room or corridor \((\d+) cells\)", log)]
# The corridor is the one that matters most: 13 only if the fill turned the
# bend AND stopped at both of its ends (room A directly, room B at a doorway).
expected = [("room A", 154), ("the bent corridor", 13), ("room B", 60),
            ("the corridor's wall blocks", 25)]
if len(areas) != len(expected):
    check(False, "four area fills", f"got {areas}")
else:
    for (label, want), got in zip(expected, areas):
        check(got == want, f"{label}: {want} squares", f"got {got}")
check("editor: Click inside a room or corridor to fill it" in log,
      "a solid square has no area, and says so")
m = re.search(r"editor: Filled the whole level \((\d+) cells\)", log)
check(m is not None and int(m.group(1)) == 227, "fill level: all 227 floor squares",
      m.group(1) if m else "no report")
layouts = re.findall(r"console: geomlayout \S+ fresh=\w+ live=\w+ (\w+)", log)
check(len(layouts) == 6 and all(l == "match" for l in layouts),
      "every fill left the uploaded chunks current", str(layouts))
h = hashes(log)
check(len(h) == 6 and all(h[i] != h[i - 1] for i in range(1, 6)),
      "every fill changed the level")

# --- phase 6: live validation's boxes -----------------------------------------
print("6 - live validation boxes what it finds, where it is")


def sections(log):
    """The console output split on the script's '--- name ---' echoes."""
    out, name = {}, None
    for line in log.splitlines():
        m = re.search(r"console: --- (.*) ---$", line)
        if m:
            name = m.group(1)
            out[name] = []
        elif name is not None and "console: editor " in line:
            out[name].append(line.split("console: ", 1)[1])
    return out


fresh()
try:
    # The two breakages the eval script's PART B and the badge rely on.
    crypt2 = os.path.join(PROJ, r"levels\crypt2.map")
    text = io.open(crypt2, encoding="utf-8", newline="").read()
    lines = [l for l in text.splitlines(True) if not l.startswith("stairs stairs_up 1 1")]
    check(len(lines) == len(text.splitlines(True)) - 1, "removed crypt2's stair back up")
    io.open(crypt2, "w", encoding="utf-8", newline="").write("".join(lines))
    dungeons = os.path.join(PROJ, r"catalog\dungeons.cat")
    d = io.open(dungeons, encoding="utf-8", newline="").read()
    io.open(dungeons, "w", encoding="utf-8", newline="").write(
        d + "\r\n[hollow]\r\nlevels =\r\n")

    log = run("liveissues.eval")
    check(passed(log), "the script ran clean")
    s = sections(log)
    base = s.get("A: baseline", [])
    check(not any(l.startswith("editor box eval_arena") for l in base),
          "eval_arena starts with nothing boxed", str(base))
    # It is two world findings, not one (no levels, and so no way in either);
    # without it the project's badge reads 0, so any count here is its.
    badge = [int(m.group(1)) for l in base for m in [re.search(r"badge (\d+)", l)] if m]
    check(bool(badge) and badge[0] >= 1,
          "the dungeon with no levels is counted on the Check badge", str(base))
    walled = s.get("A: two items walled in", [])
    check("editor box eval_arena 3,3 warning map.check.itemslost" in walled,
          "the first walled-in item is boxed amber", str(walled))
    check("editor box eval_arena 4,3 warning map.check.itemslost (from eval_arena 3,3)" in walled,
          "so is the second, from the same one finding", str(walled))
    own = s.get("B: the stair's own end", [])
    check("editor box crypt1 1,1 error map.check.stairunpaired" in own,
          "the unpaired stair is boxed red where it stands", str(own))
    far = s.get("B: the far end", [])
    check("editor box crypt2 1,1 error map.check.stairunpaired (from crypt1 1,1)" in far,
          "and at its far end, on the level it leads to", str(far))
finally:
    drop()

# --- phase 7: painting with a theme -------------------------------------------
print("7 - a theme paints a whole look, by reference")
CELL = re.compile(r"console: editor cell (\S+) (\d+),(\d+) (\w+) wall=(\S+)/(\S+) "
                  r"floor=(\S+)/(\S+) ceiling=(\S+)/(\S+)")
fresh()
try:
    # floor_rubble is not in eval_arena's palette: painting must enrol it.
    io.open(os.path.join(PROJ, r"catalog\themes.cat"), "w", encoding="utf-8", newline="").write(
        "[marble_hall]\r\ndisplay = Marble Hall\r\nfloor = floor_rubble\r\n"
        "wall = wall_marble\r\nceiling = ceiling_stone\r\n")
    log = run("themes.eval")
    check(passed(log), "the script ran clean")
    m = re.search(r"editor: Filled the room or corridor \((\d+) cells\)", log)
    check(m is not None and int(m.group(1)) == 212,
          "the area fill took the room (154) and its walls (58)", m.group(0) if m else "none")
    cells = [c.groups() for c in CELL.finditer(log)]
    before = cells[:6]
    opens = [c for c in before if c[3] == "open"]
    solids = [c for c in before if c[3] == "solid"]
    check(len(opens) == 4 and len(solids) == 2, "four room squares and two walls read back",
          str(len(before)))
    check(all(c[6] == "theme:marble_hall" and c[7] == "floor_rubble" and
              c[8] == "theme:marble_hall" and c[9] == "ceiling_stone" for c in opens),
          "every room square references it and shows its members", str(opens))
    check(len(opens) > 0 and all(c[7] == "floor_rubble" for c in opens),
          "including the floor the palette had to enrol", str([c[7] for c in opens]))
    check(all(c[4] == "theme:marble_hall" and c[5] == "wall_marble" for c in solids),
          "the walls around the room reference it too", str(solids))
    check("console: editor pick: themes marble_hall" in log,
          "the eyedropper picks up the theme, not its member")
    h = hashes(log)
    check(len(h) == 3 and h[1] != h[0] and h[2] == h[1],
          "painting changed the level, and a save and reload kept it exactly",
          str([x[2] for x in h]))
    after = cells[6:8]
    check(len(after) == 2 and after[0][6] == "theme:marble_hall" and
          after[1][4] == "theme:marble_hall", "the references came back from the file", str(after))
    # "+ New..." seeds a theme from the SELECTED square: 1,5's floor and
    # ceiling and its west wall, one member each - so what it paints must show
    # exactly those.
    seed = cells[8:]
    if len(seed) != 3:
        check(False, "three squares read for the new theme", str(len(seed)))
    else:
        src, room, wall = seed
        check(room[6] == "theme:theme1" and room[7] == src[7] and room[9] == src[9],
              "a new theme takes the selected square's floor and ceiling",
              f"{src[7]}/{src[9]} -> {room[7]}/{room[9]}")
        check(wall[4] == "theme:theme1" and wall[5] == "wall_marble",
              "and the wall beside it", str(wall))
finally:
    drop()

# --- phase 8: editing a theme -------------------------------------------------
print("8 - editing a theme repaints every square that uses it")
fresh()
try:
    io.open(os.path.join(PROJ, r"catalog\themes.cat"), "w", encoding="utf-8", newline="").write(
        "[marble_hall]\r\ndisplay = Marble Hall\r\nfloor = floor_rubble\r\n"
        "wall = wall_marble\r\nceiling = ceiling_stone\r\n")
    log = run("themeedit.eval")
    check(passed(log), "the script ran clean")
    s = {}
    name = None
    for line in log.splitlines():
        m = re.search(r"console: --- (\d+):", line)
        if m:
            name = m.group(1)
            s[name] = []
        elif name and "console: " in line:
            s[name].append(line.split("console: ", 1)[1])
    two, three, four = s.get("2", []), s.get("3", []), s.get("4", [])
    floors2 = [l for l in two if l.startswith("editor cell")]
    check(len(floors2) == 3 and all("floor=theme:marble_hall/floor_temple" in l for l in floors2),
          "the new floor shows on the level in hand AND on crypt2, not loaded",
          str(floors2))
    saved = [l for l in two if l.startswith("saved levels:")]
    check(saved and "crypt2" in saved[0] and "crypt1" not in saved[0],
          "the save wrote crypt2, which uses it, and not crypt1, which does not",
          str(saved))
    check(any("floor=theme:grand_hall/" in l for l in three), "a rename reaches the squares",
          str(three))
    check(any(l.startswith("typeset delete themes 'grand_hall': refused") for l in three),
          "a delete is refused while squares use it", str(three))
    check(any("floor=theme:grand_hall/floor_temple_b" in l for l in four),
          "a member floor type renamed keeps the theme resolving", str(four))
finally:
    drop()

# --- phase 9: making a world ----------------------------------------------------
print("9 - a new world three ways: blank, this world whole, one level")
MADE = ("nw_blank", "nw_copy", "nw_level", "nw_bad")
LEFTOVER = os.path.join(PROJECTS, ".building-nw_ghost")


def template_text(rel):
    """A file of the template a blank or wizard world starts from."""
    return io.open(os.path.join(ROOT, r"assets\templates\default", rel), encoding="utf-8").read()


def catalog_header(text):
    """The comment lines above a catalog's first [id] - its documentation."""
    out = []
    for line in text.splitlines():
        if line.startswith("["):
            break
        out.append(line)
    return "\n".join(out).rstrip()


fresh()
arena = os.path.join(PROJ, r"levels\eval_arena.map")
arena_before = io.open(arena, "rb").read()
# A note UNDER the manifest's last field (its trailer) is about this world: a
# whole copy keeps it, a one-level world must not (code-review C323).
NOTE = "; et_demo's trailer: a note about the world being copied."
try:
    # An interrupted create leaves one of these; no world list may offer it.
    os.makedirs(LEFTOVER, exist_ok=True)
    io.open(os.path.join(LEFTOVER, "project.ini"), "w").write("name = ghost\n")
    with io.open(os.path.join(PROJ, "project.ini"), "a", encoding="utf-8", newline="") as f:
        f.write("\r\n" + NOTE + "\r\n")
    log = run("newworlds.eval")
    check(passed(log), "the script ran clean")
    for w in ("nw_blank", "nw_copy", "nw_level"):
        check(f"console: created world '{w}'" in log, f"{w} was made")
    refusals = log.split("--- refusals ---", 1)[-1]
    check("could not create: A world named 'nw_blank' already exists." in refusals,
          "a name in use is refused, in its own words")
    check("could not create: Type a name first" in refusals,
          "a name that filters to nothing is refused, in its own words")
    check("could not create: 'nowhere' is not a level of this world." in refusals,
          "a level the world does not have is refused, in its own words")
    check("nw_ghost" not in refusals and ".building" not in refusals,
          "a half-built .building folder is not listed as a world")
    check(not os.path.isdir(os.path.join(PROJECTS, "nw_bad")),
          "a refused create leaves nothing behind")

    def read(world, rel):
        p = os.path.join(PROJECTS, world, rel)
        return io.open(p, encoding="utf-8").read() if os.path.isfile(p) else ""

    # THE COPY CARRIES THE UNSAVED EDIT, AND THIS WORLD KEEPS ITS FILE. The 3x3
    # at 3..5 was painted and never saved: the copy's eval_arena must hold it
    # (variant records on those squares) and the world it was copied from (the
    # scratch copy of dungeon-demo) must not have moved.
    copied = read("nw_copy", r"levels\eval_arena.map")
    painted = sum(1 for x in range(3, 6) for z in range(3, 6)
                  if re.search(rf"^variant floor {x} {z} \d+", copied, re.M))
    check(painted == 9, "the copy carries the unsaved edit (9 painted squares)", str(painted))
    check(io.open(arena, "rb").read() == arena_before,
          "and the world it was copied from was NOT saved behind your back")
    one = read("nw_level", r"levels\crypt1.map")
    stairs = re.findall(r"^stairs (\S+) .*dest=(\S+)", one, re.M)
    check(stairs == [("stairs_exit", "keep_gate")],
          "the one-level world keeps no stair but an exit to its doorway", str(stairs))
    blank = read("nw_blank", r"levels\room1.map")
    check(re.search(r"^stairs stairs_exit 8 7 south dest=keep_gate", blank, re.M) is not None,
          "the blank world's first room has a way out")
    check("[marble_hall]" in read("nw_blank", r"catalog\themes.cat") and
          "[crypt]" not in read("nw_blank", r"catalog\dungeons.cat"),
          "and the template's content, without dungeon-demo's places")

    # THE CATALOGS' DOCUMENTATION COMES ACROSS WITHOUT THE PLACES (code-review
    # C323). The template's flags.cat and quests.cat are comments alone, and a
    # new world used to get one generated line in their place; its dungeons.cat
    # is the same header with the starter dungeon under it.
    for cat in ("flags", "quests"):
        rel = rf"catalog\{cat}.cat"
        check(read("nw_blank", rel) == template_text(rel),
              f"the blank world's {cat}.cat is the template's, its documentation whole",
              read("nw_blank", rel)[:120])
    dungeons = read("nw_blank", r"catalog\dungeons.cat")
    check(dungeons.startswith(template_text(r"catalog\dungeons.cat")) and "[keep]" in dungeons,
          "its dungeons.cat keeps the template's header above the starter dungeon",
          dungeons[:120])
    # A one-level world drops the source's entries and keeps the HEADER, the
    # comments above its first [id] (non-vacuous: each source has a header and
    # the entry).
    for cat, entry in (("flags", "[relic_lifted]"), ("quests", "[sunken_relic]"),
                       ("dungeons", "[crypt]")):
        rel = rf"catalog\{cat}.cat"
        source = io.open(os.path.join(PROJ, rel), encoding="utf-8").read()
        head = catalog_header(source)
        made = read("nw_level", rel)
        check(head != "" and entry in source and made.startswith(head) and entry not in made,
              f"the one-level world's {cat}.cat keeps the header and drops {entry}", made[:120])
    # ...and none of the manifest's comments: the note under its last field
    # went with the rest, and the file opens on its own generated line.
    check(NOTE in read("nw_copy", "project.ini"),
          "a whole copy keeps the note under the manifest's last field")
    manifest = read("nw_level", "project.ini")
    check(NOTE not in manifest and manifest.startswith("; nw_level "),
          "a one-level world does not, and opens on its own header line", manifest[:120])
    # Each opens BY NAME (-project) and is clean as it stands.
    for w in ("nw_blank", "nw_copy", "nw_level"):
        wlog = run("worldcheck.eval", project=w)
        check("validate: clean" in wlog, f"{w} opens and passes the checker",
              next((l for l in wlog.splitlines() if "validate" in l), "no validate line"))
finally:
    for w in MADE + (os.path.basename(LEFTOVER),):
        harness_game.remove_world(ROOT, w)
    drop()

# --- phase 10: the New world dialog ---------------------------------------------
print("10 - the New world dialog makes a world, and hands it to the Worlds list")
fresh()
try:
    log = run("newworlddialog.eval")
    check(passed(log), "the script ran clean")
    lines = [l.split("console: ", 1)[1] for l in log.splitlines()
             if "console: new world dialog" in l or "console: worlds dialog" in l]
    check(any(l.startswith("new world dialog open: source level made 'nwd_level'") for l in lines),
          "Copy one level makes the world, and says so", " | ".join(lines))
    check(any("already exists" in l for l in lines if l.startswith("new world dialog")),
          "a name in use is refused, in its own words")
    manifest = os.path.join(PROJECTS, "nwd_level", "project.ini")
    text = io.open(manifest, encoding="utf-8").read() if os.path.isfile(manifest) else ""
    check(re.search(r"^levels = crypt2\s*$", text, re.M) is not None,
          "and it holds the level picked, alone", text[:200])
    after = [l for l in lines if l.startswith("worlds dialog open")]
    check(bool(after) and "nwd_blank" in after[-1] and "armed 'nwd_blank'" in after[-1],
          "a world made over the Worlds dialog is listed there, armed",
          after[-1] if after else "no worlds dialog line")
finally:
    for w in ("nwd_level", "nwd_blank"):
        harness_game.remove_world(ROOT, w)
    drop()

# --- phase 11: the wizard -------------------------------------------------------
print("11 - the wizard generates a first floor, tagged and reproducible")
WIZ = ("wz_a", "wz_b", "wz_c", "wz_undead", "wz_dlg")


def tagged(tag):
    """Template monsters carrying `tag` (monsters.cat `tags`)."""
    text = io.open(os.path.join(ROOT, r"assets\templates\default\catalog\monsters.cat"),
                   encoding="utf-8").read()
    out, cur = set(), None
    for line in text.splitlines():
        m = re.match(r"\[(\S+)\]", line)
        if m:
            cur = m.group(1)
        elif cur and re.match(r"tags\s*=", line) and tag in line.split("=", 1)[1].split():
            out.add(cur)
    return out


fresh()
try:
    log = run("wizard.eval")
    check(passed(log), "the script ran clean")
    for w in WIZ[:-1]:
        check(f"console: created world '{w}'" in log, f"{w} was made")
    check("source wizard made 'wz_dlg'" in log, "wz_dlg was made, through the dialog")
    themes = next((l.split("wizard tags: ", 1)[1] for l in log.splitlines()
                   if "wizard tags: " in l), "")
    check("vermin" in themes.split() and "undead" in themes.split(),
          "the tag choices are the template's tags", themes)

    def floor(w, ext):
        p = os.path.join(PROJECTS, w, "levels", "floor1." + ext)
        return io.open(p, encoding="utf-8").read() if os.path.isfile(p) else ""

    a_map, a_ent = floor("wz_a", "map"), floor("wz_a", "ent")
    check(a_map != "" and a_map == floor("wz_b", "map") and a_ent == floor("wz_b", "ent"),
          "the same knobs and seed make the SAME floor")
    check(floor("wz_c", "map") not in ("", a_map), "another seed makes a different one")
    check(floor("wz_dlg", "map") == a_map and floor("wz_dlg", "ent") == a_ent,
          "the dialog's wizard rows make the same floor as the console, from the same knobs")
    grid = [l for l in a_map.splitlines() if l and l[0] in "#.P"]
    check(bool(grid) and len(grid) == 24 and all(len(l) == 24 for l in grid),
          "the size asked for is the map's (24 x 24)", f"{len(grid)} rows")
    for w, tag in (("wz_a", "vermin"), ("wz_undead", "undead")):
        monsters = set(re.findall(r"^monster (\S+)", floor(w, "ent"), re.M))
        check(bool(monsters) and monsters <= tagged(tag),
              f"{w}'s monsters all carry '{tag}'", str(sorted(monsters)))
        check(re.search(r"^stairs stairs_exit \d+ \d+ \w+ dest=keep_gate", floor(w, "map"), re.M)
              is not None, f"{w}'s floor has a way out")
    # The template's catalog documentation comes across, as for a blank world
    # (phase 9; code-review C323).
    flags = os.path.join(PROJECTS, "wz_a", r"catalog\flags.cat")
    check(os.path.isfile(flags) and
          io.open(flags, encoding="utf-8").read() == template_text(r"catalog\flags.cat"),
          "a wizard world's flags.cat is the template's, its documentation whole")
    for w in WIZ:
        wlog = run("worldcheck.eval", project=w)
        check("validate: clean" in wlog, f"{w} opens and passes the checker",
              next((l for l in wlog.splitlines() if "validate" in l), "no validate line"))
finally:
    for w in WIZ:
        harness_game.remove_world(ROOT, w)
    drop()

# --- phase 12: the palette's category bar -------------------------------------
print("12 - the category bar shows one group, both ways, and the filter sees past it")
# The groupings AS DESIGNED (docs/tool-refinement-plan.md Phase 1). Stated here
# rather than read back from the game, so a section moved to the wrong group is
# a failure and not a new truth.
STAGE = {"world": ["styles", "dungeons", "quests", "flags", "terrain"],
         "build": ["shapes", "themes", "walls", "floors", "ceilings", "wallfeatures",
                   "surfacefeatures", "doors", "stairs"],
         # Lights (lighting-updates Phase 2) sit beside the fixtures that give them.
         "furnishings": ["decorations", "fixtures", "lights", "trails", "buttons"],
         "populate": ["monsters", "items", "weapons", "armor"]}
KIND = {"surfaces": ["themes", "walls", "floors", "ceilings", "wallfeatures",
                     "surfacefeatures"],
        "structure": ["shapes", "doors", "stairs"],
        "furnishings": ["decorations", "fixtures", "lights", "trails", "buttons"],
        "creatures": ["monsters"],
        "items": ["items", "weapons", "armor"],
        "world": ["styles", "dungeons", "quests", "flags", "terrain"]}
SHOWS = re.compile(r"editor palette: (\w+) (\w+) filter='([^']*)' shows:(.*)")


def shown(line):
    """(grouping, group, filter, [(section, rows)]) from one palette line."""
    m = SHOWS.search(line)
    if not m:
        return None
    secs = [(s, int(n)) for s, n in re.findall(r"(\w+)\((\d+)\)", m.group(4))]
    return m.group(1), m.group(2), m.group(3), secs


# The bar's state lives in settings.ini beside the exe (SETTINGS, above), not
# in the world, so this phase puts the developer's copy back afterwards.
fresh()
saved_settings = io.open(SETTINGS, "rb").read() if os.path.isfile(SETTINGS) else None
try:
    log = run("palette.eval")
    check(passed(log), "the script ran clean")
    sec = sections(log)
    tables = {}
    for line in sec.get("tables", []):
        m = re.match(r"editor palette group (\w+) (\w+):(.*)", line)
        if m:
            tables.setdefault(m.group(1), {})[m.group(2)] = m.group(3).split()
    check(tables.get("stage") == STAGE, "the stage groups are World / Build / Furnishings / Populate as designed",
          str(tables.get("stage")))
    check(tables.get("kind") == KIND, "the kind groups are as designed", str(tables.get("kind")))
    for mode, groups in (("stage", STAGE), ("kind", KIND)):
        listed = [c for cats in groups.values() for c in cats]
        check(len(listed) == len(set(listed)) and "effects" not in listed,
              f"by {mode}, every section is in one group and effects in none")
    for mode, groups in (("stage", STAGE), ("kind", KIND)):
        lines = [shown(l) for l in sec.get(mode, []) if shown(l)]
        got = {g: [s for s, _ in secs] for _, g, _, secs in lines}
        check(got == groups, f"each {mode} group lists exactly its own sections", str(got))
    world = next((secs for _, g, _, secs in
                  (shown(l) for l in sec.get("stage", []) if shown(l)) if g == "world"), [])
    counts = dict(world)
    # Quest items & flags: the world's one flag and the two quest items (both
    # world-scoped; the harness views eval_arena, whose dungeon has none).
    check(counts.get("dungeons", 0) == 2 and counts.get("quests", 0) == 1
          and counts.get("terrain", 0) == 7 and counts.get("flags", 0) == 3
          and counts.get("styles", 0) == 4,
          "the world sections list their entries (4 styles, 2 dungeons, 1 quest, 3 quest rows, "
          "7 terrains)", str(world))
    flt = [shown(l) for l in sec.get("filter", []) if shown(l)]
    if len(flt) != 3:
        check(False, "three readings in the filter section", str(flt))
    else:
        (_, _, _, before), (_, g, f, during), (_, _, f2, after) = flt
        names = [s for s, _ in during]
        check(before == [s for s in before if s[0] == "monsters"] and len(before) == 1,
              "Creatures alone lists monsters", str(before))
        check(f == "marble" and "themes" in names and "decorations" in names,
              "a filter from inside Creatures finds the marble theme and props", str(during))
        check("monsters" not in names and all(n > 0 for _, n in during),
              "while filtering, a section with no match drops out", str(during))
        check(f2 == "" and [s for s, _ in after] == ["monsters"],
              "clearing the filter hands the list back to the bar", str(after))
    check(any("no group 'build' when grouped by kind" in l for l in sec.get("refuse", [])),
          "a group the grouping lacks is refused by name")
    # C383: "a" and 12 Cyrillic letters is 13 characters but 25 bytes. A cap in
    # bytes keeps half the last letter (a lone lead byte, read back as U+FFFD).
    wide = "a" + "".join(chr(0x430 + i) for i in range(12))
    uf = [shown(l) for l in sec.get("utf8", []) if shown(l)]
    check(len(uf) == 2 and uf[0][2] == wide and uf[1][2] == "",
          "a 13-character filter of 25 bytes is kept whole, not cut at a byte",
          ascii([u[2] for u in uf]))
    ini = io.open(SETTINGS, encoding="utf-8").read() if os.path.isfile(SETTINGS) else ""
    check("map_palette_group=1" in ini and "map_palette_kind=4" in ini,
          "the grouping and its group are saved to settings.ini")
    log2 = run("palette-persist.eval")
    back = [shown(l) for l in sections(log2).get("reopened", []) if shown(l)]
    check(len(back) == 1 and back[0][:2] == ("kind", "items"),
          "a fresh start opens the palette where it was left", str(back))
finally:
    drop()
    if saved_settings is None:
        if os.path.isfile(SETTINGS):
            os.remove(SETTINGS)
    else:
        io.open(SETTINGS, "wb").write(saved_settings)

# --- phase 13: monster power ---------------------------------------------------
print("13 - a monster's power is its threat until overridden, and everything ranks by it")


THREAT = re.compile(r"threat (\S+) ([\d.]+) offence=.* power=([\d.]+)(\(set\))? band=(\d)")


def table(lines):
    """{id: (threat, power, overridden, band)} from `threat` lines."""
    out = {}
    for l in lines:
        m = THREAT.match(l)
        if m:
            out[m.group(1)] = (float(m.group(2)), float(m.group(3)), bool(m.group(4)),
                               int(m.group(5)))
    return out


def expected_band(p, lo, hi):
    # Game/Power.h, written out again HERE so the check is not the code judging
    # itself: which fifth of the range, the top edge in band 5, no width = 3.
    if hi - lo <= 1e-9:
        return 3
    t = min(1.0, max(0.0, (p - lo) / (hi - lo)))
    return min(5, 1 + int(t * 5))


def bands_agree(t):
    powers = [v[1] for v in t.values()]
    lo, hi = min(powers), max(powers)
    return {k: v[3] for k, v in t.items()} == {k: expected_band(v[1], lo, hi) for k, v in t.items()}


def palette_bands(lines):
    return {m.group(1): int(m.group(2)) for m in
            (re.match(r"editor palette item monsters (\S+) band=(\d)", l) for l in lines) if m}


def swarms(stem):
    p = os.path.join(PROJ, "levels", stem + ".ent")
    text = io.open(p, encoding="utf-8").read() if os.path.isfile(p) else ""
    return len(re.findall(r"^monster skel_swarm ", text, re.M)), bool(text)


fresh()
try:
    log = run("power.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    before, after, cleared = (table(sec.get(s, [])) for s in ("before", "after", "cleared"))
    check(len(before) >= 10 and all(v[0] == v[1] and not v[2] for v in before.values()),
          "unset, every kind's power IS its derived threat", str(len(before)))
    check(bands_agree(before) and len({v[3] for v in before.values()}) >= 3,
          "the bands are the fifths of the project's power range (3+ bands in use)",
          str({k: v[3] for k, v in before.items()}))
    check(palette_bands(sec.get("before", [])) == {k: v[3] for k, v in before.items()},
          "the palette's rows wear the same bands")
    sw = after.get("skel_swarm")
    check(sw is not None and sw[1] == 40.0 and sw[2] and sw[0] == before["skel_swarm"][0],
          "an override sets the power and leaves the derived threat as it was", str(sw))
    check(sw is not None and sw[3] == 5 and bands_agree(after)
          and after["skel_lurker"][3] < before["skel_lurker"][3],
          "the bands re-cut against the new top (the old strongest drops a band or more)",
          str({k: v[3] for k, v in after.items()}))
    check(palette_bands(sec.get("after", [])) == {k: v[3] for k, v in after.items()},
          "the palette's pips follow the override")
    stems = [re.match(r"generate: wrote (\S+) ", l).group(1)
             for s in ("control", "picks") for l in sec.get(s, []) if l.startswith("generate: wrote")]
    if len(stems) != 3:
        check(False, "three generated levels", str(stems))
    else:
        (c_n, c_ok), (p_n, p_ok), (b_n, b_ok) = (swarms(s) for s in stems)
        check(c_ok and c_n > 0, "before the override the swarm is among the weak, and turns up",
              f"{c_n} swarms")
        check(p_ok and p_n == 0, "overridden to the top, the same low-difficulty level has none",
              f"{p_n} swarms")
        check(b_ok and b_n == 1, "with a boss, the boss is the overridden swarm (exactly one)",
              f"{b_n} swarms")
    cw = cleared.get("skel_swarm")
    check(cw is not None and not cw[2] and cw[1] == cw[0] and cleared == before,
          "removing the override puts every power and band back", str(cw))
finally:
    drop()

# --- phase 14: the docks and the overview --------------------------------------
print("14 - the docks resize and remember, and the overview counts what the files hold")
DOCK = re.compile(r"editor dock panel=(\d+) left=(\d+) right=(\d+) grid=(-?\d+),(\d+),(\d+) "
                  r"strip=(-?\d+) palette=(-?\d+),(\d+)")


def docks(lines):
    out = []
    for l in lines:
        m = DOCK.match(l)
        if m:
            v = [int(x) for x in m.groups()]
            out.append(dict(zip(("panel", "left", "right", "gx", "gw", "gright", "strip", "px", "pw"), v)))
    return out


def overview(lines, scope):
    return {m.group(1): m.group(2) for m in
            (re.match(rf"editor overview {scope} (\S+) (.*)", l) for l in lines) if m}


def files_census():
    """Each level's counts, read from the PROJECT FILES - not from the game."""
    ini = io.open(os.path.join(PROJ, "project.ini"), encoding="utf-8").read()
    levels = re.search(r"^levels\s*=\s*(.*)$", ini, re.M).group(1).split()
    quest, cur = set(), None
    for cat in ("items", "weapons", "armor"):
        for l in io.open(os.path.join(PROJ, "catalog", cat + ".cat"), encoding="utf-8"):
            h = re.match(r"\[(\S+)\]", l)
            if h:
                cur = h.group(1)
            elif cur and re.match(r"(quest|flag|reveals)\s*=", l):
                quest.add(cur)
    scenery, cur = set(), None
    for l in io.open(os.path.join(PROJ, r"catalog\stairs.cat"), encoding="utf-8"):
        h = re.match(r"\[(\S+)\]", l)
        if h:
            cur = h.group(1)
        elif cur and re.match(r"traverse\s*=\s*0", l):
            scenery.add(cur)
    out = {}
    for s in levels:
        ent = io.open(os.path.join(PROJ, "levels", s + ".ent"), encoding="utf-8").read()
        mp = io.open(os.path.join(PROJ, "levels", s + ".map"), encoding="utf-8").read()
        items = re.findall(r"^item (\S+)", ent, re.M)
        out[s] = {"monsters": len(re.findall(r"^monster ", ent, re.M)), "items": len(items),
                  "quest": sum(1 for i in items if i in quest),
                  "doors": len(re.findall(r"^door ", ent, re.M)),
                  "stairs": sum(1 for t in re.findall(r"^stairs (\S+)", mp, re.M) if t not in scenery)}
    return out


def dungeon_levels():
    out, cur = {}, None
    for l in io.open(os.path.join(PROJ, r"catalog\dungeons.cat"), encoding="utf-8"):
        h = re.match(r"\[(\S+)\]", l)
        if h:
            cur = h.group(1)
        m = re.match(r"levels\s*=\s*(.*)", l)
        if cur and m:
            out[cur] = m.group(1).split()
    return out


saved_settings = io.open(SETTINGS, "rb").read() if os.path.isfile(SETTINGS) else None
fresh()
try:
    log = run("docks.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    w = docks(sec.get("widths", []))
    if len(w) != 5:
        check(False, "five layout readings", str(len(w)))
    else:
        d0, d1, d2, d3, d4 = w
        grow = 400 - d0["left"]
        check(d1["left"] == 400 and grow != 0, "the palette dock takes the width it is dragged to",
              f"{d0['left']} -> {d1['left']}")
        check(d1["strip"] - d0["strip"] == grow and d1["gx"] - d0["gx"] == grow
              and d1["pw"] - d0["pw"] == grow,
              "the tool strip, the grid and the palette's body all move with its edge",
              f"strip {d0['strip']}->{d1['strip']} grid {d0['gx']}->{d1['gx']} palette {d0['pw']}->{d1['pw']}")
        check(d2["right"] == 350 and d2["gright"] == d2["panel"] - 350,
              "the key dock likewise, and the grid ends at its edge",
              f"right {d2['right']}, grid ends {d2['gright']} of {d2['panel']}")
        check(d3["left"] == 120, "too narrow clamps to the floor (120)", str(d3["left"]))
        check(d4["left"] == round(d4["panel"] * 0.30),
              "too wide clamps to 30% of the panel, so the grid stays the larger part",
              f"{d4['left']} of {d4['panel']}")
    truth = files_census()
    viewed = "eval_arena"  # project.ini's eval_level, where the harness opens
    dl = dungeon_levels()
    home = next((d for d, lv in dl.items() if viewed in lv), None)
    ov = {s: overview(sec.get("overview", []), s) for s in ("level", "dungeon", "world")}
    lvl, dun, wld = ov["level"], ov["dungeon"], ov["world"]

    def total(key, stems):
        return sum(truth[s][key] for s in stems)

    def bands_sum(o):
        return sum(int(x) for x in o.get("bands", "").split(",") if x)

    for scope, o, stems in (("level", lvl, [viewed]), ("dungeon", dun, dl.get(home, [])),
                            ("world", wld, list(truth))):
        want = {k: str(total(k, stems)) for k in ("monsters", "items", "quest", "doors", "stairs")}
        got = {k: o.get(k) for k in want}
        check(got == want and bands_sum(o) == total("monsters", stems),
              f"the {scope} counts are the files' (and every monster has a band)",
              f"got {got}, files {want}, bands {o.get('bands')}")
    check(wld.get("levels") == str(len(truth)) and dun.get("levels") == str(len(dl.get(home, []))),
          "the level counts are the manifest's and the dungeon's",
          f"world {wld.get('levels')}, dungeon {dun.get('levels')}")
    links = {k[len("dungeon:"):]: v for k, v in wld.items() if k.startswith("dungeon:")}
    check(set(links) == set(dl) and all(v.startswith(str(len(dl[d]))) for d, v in links.items()),
          "the world lists every dungeon with its level count", str(links))
    placed = overview(sec.get("placed", []), "level")
    placed_w = overview(sec.get("placed", []), "world")
    undone = overview(sec.get("undone", []), "level")
    check(placed.get("monsters") == str(int(lvl.get("monsters", -1)) + 1)
          and placed_w.get("monsters") == str(int(wld.get("monsters", -1)) + 1)
          and bands_sum(placed) == bands_sum(lvl) + 1,
          "a monster placed on the viewed level counts at once, there and in the world",
          f"{lvl.get('monsters')} -> {placed.get('monsters')}")
    check(undone == lvl, "undo takes it off the count again", str(undone))
    back = docks(console_sections(run("docks-persist.eval")).get("reopened", []))
    check(len(back) == 1 and back[0]["left"] == 300 and back[0]["right"] == 350,
          "a fresh start opens both docks at the widths they were left",
          str(back[0] if back else "no reading"))
finally:
    drop()
    if saved_settings is None:
        if os.path.isfile(SETTINGS):
            os.remove(SETTINGS)
    else:
        io.open(SETTINGS, "wb").write(saved_settings)

# --- phase 15: flags --------------------------------------------------------------
print("15 - flags: what waits on them, what sets them, the checker and the palette")

ARENA = os.path.join(PROJ, r"levels\eval_arena")
FIXTURE_FLAGS = """
[arena_gate]
display = Arena gate
dungeon = eval

[beacon_lit]
display = Beacon lit

[crypt_seal]
display = Crypt seal
dungeon = crypt

[orphan_flag]
display = Orphan
"""
FIXTURE_ENTS = [
    "door wooden_door 12 3 north flag=arena_gate",
    "button lever 5 4 north sets=arena_gate",
    "button lever 8 4 north flag=arena_gate",
    "button lever 10 4 north toggles=crypt_seal",
    "button lever 14 4 north flag=ghost_flag",
]
FIXTURE_STAIR = "stairs stairs_down 20 10 south dest=crypt2 destx=1 destz=1 flag=beacon_lit"


def write_fixture():
    def edit(path, fn):
        raw = io.open(path, "rb").read().decode("utf-8")
        eol = "\r\n" if "\r\n" in raw else "\n"
        io.open(path, "wb").write(eol.join(fn(raw.split(eol))).encode("utf-8"))

    def walled(lines):
        grid = [i for i, l in enumerate(lines) if l.startswith("#")]
        row = grid[3]  # z = 3: a wall across, its doorway at x = 12
        lines[row] = "#" * 12 + "." + "#" * (len(lines[row]) - 13)
        lines.insert(grid[0], FIXTURE_STAIR)
        return lines

    edit(ARENA + ".map", walled)
    edit(ARENA + ".ent", lambda lines: [l for l in lines if l] + FIXTURE_ENTS + [""])
    edit(os.path.join(PROJ, r"catalog\flags.cat"),
         lambda lines: lines + FIXTURE_FLAGS.strip("\n").split("\n") + [""])


FLAGLINE = re.compile(r"flag (\S+) (on|off) (\S+)")


def flag_states(lines):
    return {m.group(1): (m.group(2), m.group(3)) for m in (FLAGLINE.match(l) for l in lines) if m}


def palette_rows(lines):
    out = []
    for l in lines:
        m = re.match(r"editor palette item flags (\S+) band=\d group='([^']*)' ref=(\S+) goto=(\S+) "
                     r"label='([^']*)'", l)
        if m:
            out.append(m.groups())
    return out


fresh()
try:
    write_fixture()
    log = run("flags.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    start = flag_states(sec.get("start", []))
    check(start == {"arena_gate": ("off", "dungeon:eval"), "beacon_lit": ("off", "world"),
                    "crypt_seal": ("off", "dungeon:crypt"), "orphan_flag": ("off", "world"),
                    "relic_lifted": ("off", "world")},
          "every flag starts off, each with its scope", str(start))
    local = [l for l in sec.get("start", []) if l.startswith("flag ")][len(start):]
    check([l.split()[1] for l in local] == ["arena_gate"],
          "`flags dungeon` lists the party's dungeon's own flags alone", str(local))
    door = sec.get("door", [])
    want = ["door 12,3 -> shut", "button 8,4 -> off", "button 5,4 -> on",
            "flag arena_gate on dungeon:eval", "door 12,3 -> open", "button 8,4 -> on"]
    check([l for l in door if l.startswith(("door ", "button ", "flag "))] == want,
          "sealed and stuck until the lever sets the flag, then both give", str(door))
    item = flag_states(sec.get("item", []))
    lines = [l for l in sec.get("item", []) if l.startswith("flag ")]
    check(len(lines) == 2 and lines[0].startswith("flag relic_lifted off")
          and lines[1].startswith("flag relic_lifted on"),
          "lifting the relic turns its flag on", str(lines))
    saved = [l for l in sec.get("save", []) if l.startswith("flag ")]
    check(saved[-2:] == ["flag arena_gate on dungeon:eval", "flag relic_lifted on world"],
          "a load puts back the flags the save held", str(saved))
    issues = [l.strip() for l in sec.get("validate", [])]

    def found(sev, where, key, arg):
        return any(re.match(rf"{sev}\s+{re.escape(where)}\s*{key} {arg}$", l) for l in issues)

    check(found("ERR", "eval_arena @20,10", "map.check.flagwaits", "beacon_lit")
          and found("ERR", "eval_arena @14,4", "map.check.flagwaits", "ghost_flag"),
          "a stair and a lever waiting on flags nothing sets are errors where they stand")
    check(found("warn", "eval_arena @14,4", "map.check.flagunknown", "ghost_flag"),
          "a flag flags.cat lacks is named")
    check(found("warn", "eval_arena @10,4", "map.check.flagscope", "crypt_seal"),
          "the crypt's flag toggled from the Proving Ground is named")
    check(found("warn", "", "map.check.flagunused", "orphan_flag"),
          "a flag nothing sets or reads is named")
    check(not any(re.search(r"flag\w+ (arena_gate|relic_lifted)$", l) for l in issues),
          "the sound flags raise nothing (the gate's lever, the relic in the crypt)",
          str([l for l in issues if "flag" in l]))
    wire = [l for l in sec.get("wire", []) if l.startswith("flagwire ")]
    check(wire == ["flagwire door 12,3 flag=beacon_lit", "flagwire door 12,3 flag=arena_gate",
                   "flagwire lever 8,4 flag= clears=arena_gate",
                   "flagwire lever 8,4 flag=arena_gate op="],
          "the inspectors' setters rewire a door and a lever", str(wire))
    pal = sec.get("palette", [])
    wld, dun = overview(pal, "world"), overview(pal, "dungeon")
    check(wld.get("flags") == "3 (1 on)" and dun.get("flags") == "1 (1 on)",
          "the overview counts the world's flags and the dungeon's own, and how many are on",
          f"world {wld.get('flags')}, dungeon {dun.get('flags')}")
    # Listed twice: before `newtype flags` (N rows) and after it (N + 1).
    item_lines = [l for l in pal if l.startswith("editor palette item flags")]
    first = palette_rows(item_lines[:(len(item_lines) - 1) // 2])
    rows = {r[0]: r for r in palette_rows(item_lines)}
    groups = {r[0]: r[1] for r in first}
    check(groups.get("arena_gate", "").endswith("(this dungeon)")
          and all(groups.get(f) == "World" for f in ("beacon_lit", "orphan_flag", "relic_lifted"))
          and "crypt_seal" not in groups,
          "the section lists this dungeon's flags and the world's, not the crypt's", str(groups))
    relic = rows.get("sunken_relic")
    check(relic is not None and relic[2] == "items" and relic[3] == "crypt2@5,4"
          and "crypt2 5,4" in relic[4] and relic[1] == "World",
          "a quest item lists with where it lies, and a link there", str(relic))
    check("flag1" in rows and rows["flag1"][1].endswith("(this dungeon)") and "flag1" not in groups,
          "a new flag joins the viewed dungeon's own")
    used = [l for l in pal if l.startswith("editor palette used")]
    exp = ["armed=items:sunken_relic", "armed=-:", "typeeditor=arena_gate", "view=crypt2 sel=5,4"]
    check(len(used) == 4 and all(e in u for e, u in zip(exp, used)),
          "a quest item row arms its brush and again puts it down; a flag row opens its "
          "editor; the link goes to where the item lies", "\n         ".join(used))
    stair = sec.get("stair", [])
    maps = [l for l in stair if " map, start " in l]
    check(any(l.startswith("20,10 ") for l in stair) and len(maps) == 2
          and maps[0].startswith("28x24") and not maps[1].startswith("28x24"),
          "a stair waiting on a flag bars the way until it is on, then goes down",
          str([l for l in stair if " map, " in l or l.startswith("20,")]))
finally:
    drop()
    harness_game.remove_saves(SAVES.values())  # its save, gone as soon as it is done with

# --- phase 16: styles and the library ---------------------------------------------
print("16 - styles: the library, adding and saving, the monster lens, the rename sweeps")

STYLELINE = re.compile(r"style (\S+) (world|library)( current)? room=(\S+) corridor=(\S+) "
                       r"width=(\S+) monsters=(.*)")


def style_lines(lines):
    return {m.group(1): m.groups()[1:] for m in (STYLELINE.match(l) for l in lines) if m}


def palette_lens(lines, section):
    return {m.group(1): m.group(2) for m in
            (re.match(rf"editor palette item {section} (\S+) .* lens=(on|off)(?: |$)", l) for l in lines)
            if m}


def palette_listings(lines, section):
    """Each consecutive run of `editor palette item <section>` lines, as one listing."""
    runs, cur = [], []
    for l in lines:
        if l.startswith(f"editor palette item {section} "):
            cur.append(l)
        elif cur:
            runs.append(cur)
            cur = []
    if cur:
        runs.append(cur)
    return runs


def drop_block(path, block_id):
    raw = io.open(path, "rb").read().decode("utf-8")
    eol = "\r\n" if "\r\n" in raw else "\n"
    out, skip = [], False
    for l in raw.split(eol):
        if l.strip().startswith("["):
            skip = l.strip() == f"[{block_id}]"
        if not skip:
            out.append(l)
    io.open(path, "wb").write(eol.join(out).encode("utf-8"))


def read(rel):
    return io.open(os.path.join(PROJ, rel), encoding="utf-8").read()


fresh()
# THE LIBRARY IS REAL: it has one home, so this phase changes it (an add reads
# it, a save writes it) behind a backup. back_up() never deletes a backup it
# finds - that one is a killed run's and the clean copy - and restore() writes
# the library back in place and only then discards it (renamed out of its name
# first, so a kill during the delete never leaves a partial "clean copy").
harness_game.back_up(LIBRARY, LIBBAK)
try:
    drop_block(os.path.join(PROJ, r"catalog\floors.cat"), "ground_soil_rocky")
    walls_before = io.open(os.path.join(PROJ, r"catalog\walls.cat"), "rb").read()

    def arena(ext, fn):
        p = ARENA + ext
        raw = io.open(p, "rb").read().decode("utf-8")
        eol = "\r\n" if "\r\n" in raw else "\n"
        io.open(p, "wb").write(eol.join(fn(raw.split(eol))).encode("utf-8"))

    arena(".ent", lambda ls: [l for l in ls if l] +
          ["button lever 5 1 north flag=relic_lifted sets=relic_lifted", ""])
    arena(".map", lambda ls: ls[:next(i for i, l in enumerate(ls) if l.startswith("#"))] +
          ["stairs stairs_down 20 10 south dest=crypt2 destx=1 destz=1 flag=relic_lifted"] +
          ls[next(i for i, l in enumerate(ls) if l.startswith("#")):])

    log = run("styles.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    listed = style_lines(sec.get("list", []))
    check({k: v[0] for k, v in listed.items()} ==
          {"small_crypt": "world", "dirt_tunnels": "library", "guard_barracks": "library",
           "marble_halls": "library"},
          "the world's style and the library's three are listed", str({k: v[0] for k, v in listed.items()}))
    groups = {m.group(1): (m.group(2), m.group(3)) for m in
              (re.match(r"editor palette item styles (\S+) .*group='([^']*)' ref=(\S+)", l)
               for l in sec.get("list", [])) if m}
    check(groups.get("small_crypt") == ("This world", "-")
          and all(groups.get(s, ("", ""))[1] == "library"
                  for s in ("dirt_tunnels", "guard_barracks", "marble_halls")),
          "the palette lists This world, then the library's", str(groups))
    lens = sec.get("lens", [])
    runs = [palette_lens(r, "monsters") for r in palette_listings(lens, "monsters")]
    crypt = {"skeleton", "skel_archer", "skel_coward", "mummy"}
    if len(runs) != 3:
        check(False, "three Monsters listings", str(len(runs)))
    else:
        check(all(v == "on" for v in runs[0].values()) and len(runs[0]) >= 10,
              "with no style armed every monster is on the lens")
        check({k for k, v in runs[1].items() if v == "on"} == crypt,
              "Small Crypt armed: its four monsters lead, the rest fall below the divider",
              str({k for k, v in runs[1].items() if v == "on"}))
        check(runs[2] == runs[0], "disarmed, the lens is the level's again")
    rows = [l for l in lens if l.startswith("style current")]
    check(rows[-2:] == ["style current small_crypt", "style current -"],
          "clicking the world style's row arms it, and again puts it down", str(rows))
    add = [l for l in sec.get("add", []) if l.startswith("style add")]
    check(len(add) == 2 and add[0] == "style add dirt_tunnels: added copied=floors:ground_soil_rocky,"
          "themes:dirt_cave,themes:dirt_tunnel,styles:dirt_tunnels missing=-",
          "adding Dirt Tunnels copies its two themes and the one floor the world lacked",
          add[0] if add else "(none)")
    check(len(add) == 2 and add[1].startswith("style add dirt_tunnels: already copied=-"),
          "a second add is a no-op", add[1] if len(add) > 1 else "(none)")
    check(style_lines(sec.get("add", [])).get("dirt_tunnels", ("",))[0] == "world",
          "and it lists as the world's own now")
    check("[ground_soil_rocky]" in read(r"catalog\floors.cat")
          and io.open(os.path.join(PROJ, r"catalog\walls.cat"), "rb").read() == walls_before,
          "the floor reached floors.cat, and walls.cat (which had wall_rock) is untouched")
    saved = [l for l in sec.get("save", []) if l.startswith("style save")]
    lib = io.open(os.path.join(LIBRARY, "styles.cat"), encoding="utf-8").read()
    check(saved == ["style save style1: saved copied=walls:wall_stone_30,floors:floor_ancient_stone,"
                    "ceilings:ceiling_stone,themes:crypt_chamber,styles:style1"]
          and "[style1]" in lib and "monsters = skeleton 2, ghoul" in lib,
          "a world style saves to the library with its theme and the theme's surfaces",
          str(saved))
    issues = [l.strip() for l in sec.get("save", [])]
    check(any(re.match(r"warn\s+map\.check\.stylenomonster style1$", l) for l in issues),
          "the checker names the monster the style lists and the world lacks")
    renamed = style_lines(sec.get("rename", []))
    check(renamed.get("small_crypt", ("",) * 3)[2] == "crypt_room"
          and renamed.get("style1", ("",) * 3)[2] == "crypt_room",
          "renaming a theme rewrites the styles that name it",
          str({k: v[2] for k, v in renamed.items()}))
    ent, mp = read(r"levels\eval_arena.ent"), read(r"levels\eval_arena.map")
    items = read(r"catalog\items.cat")
    check("flag=relic_taken sets=relic_taken" in ent and "flag=relic_taken" in mp
          and "flag = relic_taken=1" in items and "[relic_taken]" in read(r"catalog\flags.cat"),
          "renaming a flag rewrites the lever, the stair and the item that name it")
finally:
    drop()
    harness_game.restore(LIBBAK, LIBRARY)

# --- phase 17: the shape brushes ---------------------------------------------------
print("17 - shape brushes: corridor, room, stamp and region, in the current style")

SHAPELINE = re.compile(r"editor shape (\w+): squares=(\d+) solid=(\d+) opened=(\d+) "
                       r"raised=(\d+) painted=(\d+)")
WALKABLE = re.compile(r"(\d+)x(\d+) map, start (\d+),(\d+), (\d+) walkable")


def shape_lines(lines):
    return [tuple(int(x) if x.isdigit() else x for x in m.groups())
            for m in (SHAPELINE.match(l) for l in lines) if m]


def walkables(lines):
    return [int(m.group(5)) for m in (WALKABLE.match(l) for l in lines) if m]


def read_level(stem):
    """(grid rows, {(surface, x, z): theme}) of a saved level."""
    text = io.open(os.path.join(PROJ, "levels", stem + ".map"), encoding="utf-8").read()
    rows = [l for l in text.splitlines() if l and l[0] in "#.PDTF"]
    themes = {}
    for m in re.finditer(r"^theme (\w+) (\d+) (\d+) (\S+)", text, re.M):
        themes[(m.group(1), int(m.group(2)), int(m.group(3)))] = m.group(4)
    return rows, themes


fresh()
try:
    # The fixture: rock everywhere but a 5x5 room round the start (14,12).
    def rocky(lines):
        grid = [i for i, l in enumerate(lines) if l and l[0] in "#.P"]
        for i, z in zip(grid, range(len(grid))):
            row = ["#"] * len(lines[i])
            if 10 <= z <= 14:
                for x in range(12, 17):
                    row[x] = "."
            if z == 12:
                row[14] = "P"
            lines[i] = "".join(row)
        return lines

    for ext, fn in ((".map", rocky), (".ent", lambda ls: ["; eval_arena - emptied by EditorTest 17", ""])):
        p = ARENA + ext
        raw = io.open(p, "rb").read().decode("utf-8")
        eol = "\r\n" if "\r\n" in raw else "\n"
        io.open(p, "wb").write(eol.join(fn(raw.split(eol))).encode("utf-8"))

    log = run("shapes.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    base = walkables(sec.get("base", []))
    cor = walkables(sec.get("corridor", []))
    shapes = shape_lines([l.split("console: ", 1)[1] for l in log.splitlines()
                          if "console: editor shape" in l])
    kinds = [s[0] for s in shapes]
    check(base == [25], "the fixture is the 5x5 room and rock", str(base))
    c = shapes[0] if shapes else None
    check(c is not None and c[0] == "corridor" and len(cor) == 3 and cor[0] == 25 + c[3]
          and c[3] > 0 and c[5] > c[3],
          "a corridor opens the rock it crosses and paints it and its walls",
          f"{c}, walkable {cor}")
    hashes_seen = [h for h in hashes(log)]
    check(len(cor) == 3 and cor[1] == 25 and cor[2] == cor[0]
          and len(hashes_seen) >= 2 and hashes_seen[0] == hashes_seen[1],
          "undo puts the rock back exactly (geometry hash), redo lays it again",
          f"walkable {cor}")
    rows, themes = read_level("eval_arena")
    open_ = {(x, z) for z, r in enumerate(rows) for x, ch in enumerate(r) if ch != "#"}

    def reach(start):
        seen, todo = {start}, [start]
        while todo:
            x, z = todo.pop()
            for n in ((x + 1, z), (x - 1, z), (x, z + 1), (x, z - 1)):
                if n in open_ and n not in seen:
                    seen.add(n)
                    todo.append(n)
        return seen

    from_start = reach((14, 12))
    check((3, 3) in from_start, "the corridor joins the room to its far end")
    check(themes.get(("floor", 3, 3)) == "crypt_passage"
          and any(themes.get(("wall", x, z)) == "crypt_passage"
                  for x, z in ((2, 3), (3, 2), (2, 2), (4, 2))),
          "its floor and the walls along it wear the style's corridor theme",
          str({k: v for k, v in themes.items() if k[1] <= 4 and k[2] <= 4}))
    r = shapes[1] if len(shapes) > 1 else None
    room = {(x, z) for x in range(18, 25) for z in range(3, 9)}
    check(r is not None and r[0] == "room" and r[1] == 42 and room <= open_
          and themes.get(("floor", 18, 3)) == "crypt_chamber",
          "a room opens its rectangle in the style's room theme", str(r))
    s = shapes[2] if len(shapes) > 2 else None
    check(s is not None and s[0] == "stamp" and s[2] == 6 and s[4] == 2
          and (14, 10) not in open_ and (14, 13) not in open_ and (14, 12) in open_,
          "a stamp over the start room raises its two pillars there - never on the party",
          str(s))
    g = shapes[3] if len(shapes) > 3 else None
    region = {(x, z) for x in range(12, 23) for z in range(15, 23)} & open_
    check(g is not None and g[0] == "region" and g[3] > 20 and region and region <= from_start,
          "a region generates rooms in its box, joined to the room touching it",
          f"{g}, {len(region)} open, {len(region - from_start)} unreached")
    pl = shapes[4] if len(shapes) > 4 else None
    check(pl is not None and pl[0] == "room" and pl[3] == 9 and pl[5] == 0
          and ("floor", 25, 13) not in themes,
          "with no style a room is plain carving: opened, nothing painted", str(pl))
    sm = shapes[5] if len(shapes) > 5 else None
    check(sm is not None and sm[0] == "region" and sm[1] == 0 and sm[3] == 0,
          "a region under 6x6 generates nothing", str(sm))
finally:
    drop()

# --- phase 18: the workflow, wired through ------------------------------------------
print("18 - the four stages as one path: a styled world, add, build, populate, overview")
P7 = ("p7_world", "p7_wiz")
DIRT = {"centipede", "giant_spider", "blob"}
MARBLE = {"skel_mage", "mummy", "skel_warrior", "skel_berserker"}


def world_file(world, rel):
    p = os.path.join(PROJECTS, world, rel)
    return io.open(p, encoding="utf-8").read() if os.path.isfile(p) else ""


def theme_ids(text):
    return {m.group(1) for m in re.finditer(r"^theme \w+ \d+ \d+ (\S+)", text, re.M)}


def monsters_in(text):
    return [m.group(1) for m in re.finditer(r"^monster (\S+)", text, re.M)]


# The dialog's Create and Populate PERSIST their knobs (settings.ini gen_knobs),
# and other suites' scripts inherit the knobs they leave unset - so the style's
# recipe this phase uses must not outlive it.
settings_before = io.open(SETTINGS, "rb").read() if os.path.isfile(SETTINGS) else None
fresh()
try:
    for w in P7:
        harness_game.remove_world(ROOT, w)
    log = run("workflow_worlds.eval")
    check(passed(log), "the worlds script ran clean")
    # A BLANK world in a library style: the style and what it names arrive, the
    # starter dungeon names it, and the first room wears it - tags included.
    check("[dirt_tunnels]" in world_file("p7_world", r"catalog\styles.cat")
          and "[dirt_cave]" in world_file("p7_world", r"catalog\themes.cat")
          and "[dirt_tunnel]" in world_file("p7_world", r"catalog\themes.cat"),
          "a blank world made in Dirt Tunnels receives the style and its two themes")
    check(re.search(r"^style = dirt_tunnels\s*$", world_file("p7_world", r"catalog\dungeons.cat"), re.M)
          is not None, "its starter dungeon names the style as its default")
    room1 = world_file("p7_world", r"levels\room1.map")
    check(re.search(r"^tags cave vermin ooze\s*$", room1, re.M) is not None
          and re.search(r"^theme floor 8 8 dirt_cave\s*$", room1, re.M) is not None
          and re.search(r"^theme wall 6 6 dirt_cave\s*$", room1, re.M) is not None
          and re.search(r"^palette floor .*ground_soil_rocky", room1, re.M) is not None,
          "the first room wears the room theme, floor and walls, with the style's tags")
    # The WIZARD in a style: its recipe, tags, monsters and both themes.
    wiz_map, wiz_ent = world_file("p7_wiz", r"levels\floor1.map"), world_file("p7_wiz", r"levels\floor1.ent")
    wiz_monsters = set(monsters_in(wiz_ent))
    check(theme_ids(wiz_map) == {"marble_hall", "marble_gallery"}
          and re.search(r"^tags undead stone\s*$", wiz_map, re.M) is not None,
          "a wizard floor in Marble Halls lays its room and corridor themes, with its tags",
          str(theme_ids(wiz_map)))
    check(bool(wiz_monsters) and wiz_monsters <= MARBLE,
          "...and draws its monsters from the style's list", str(sorted(wiz_monsters)))
    for w in P7:
        wlog = run("worldcheck.eval", project=w)
        check("validate: clean" in wlog, f"{w} opens and passes the checker",
              next((l for l in wlog.splitlines() if "validate" in l), "no validate line"))

    # THE WALK, inside p7_world.
    log = run("workflow_walk.eval", project="p7_world")
    check(passed(log), "the walk ran clean")
    sec = console_sections(log)
    add = sec.get("add", [])
    opened = next((l for l in add if l.startswith("generate dialog: create")), "")
    check("style=dirt_tunnels" in opened and "winding:0.85" in opened,
          "[+] opens on the dungeon's style, its shape knobs loaded", opened[:120])
    check("generate dialog: made keep1" in add, "Create makes the floor", " | ".join(add[:4]))
    check(any(l.startswith("editor palette: stage build ") for l in add)
          and any(re.match(r"style dirt_tunnels world current ", l) for l in add),
          "...and lands in the Build stage with the style armed (it was on Creatures before)")
    keep1_map, keep1_ent = world_file("p7_world", r"levels\keep1.map"), world_file("p7_world", r"levels\keep1.ent")
    check(theme_ids(keep1_map) == {"dirt_cave", "dirt_tunnel"}
          and re.search(r"^tags cave vermin ooze\s*$", keep1_map, re.M) is not None,
          "the generated floor wears both themes and carries the style's tags - no Level settings visit",
          str(theme_ids(keep1_map)))
    check(set(monsters_in(keep1_ent)) <= DIRT and monsters_in(keep1_ent),
          "...and its monsters are the style's", str(sorted(set(monsters_in(keep1_ent)))))
    empty = sec.get("empty", [])
    check("generate dialog: made keep2" in empty
          and any(l.startswith("editor palette: stage build ") for l in empty),
          "Empty makes the box in the style, landing in Build")

    def ov(lines, key):
        return next((l.split(f"editor overview level {key} ", 1)[1] for l in lines
                     if l.startswith(f"editor overview level {key} ")), None)

    def followed(lines):
        return next((l.split(" -> ", 1)[1].split(" ", 1)[0] for l in lines
                     if l.startswith("editor overview follow next")), None)

    check(ov(empty, "squares") == "9" and followed(empty) == "stage:build",
          "an empty floor's next step is Build (and its link opens that stage)",
          f"squares {ov(empty, 'squares')}, next -> {followed(empty)}")
    build = sec.get("build", [])
    shapes = shape_lines(build)
    check([s[0] for s in shapes] == ["room", "corridor", "stamp"] and all(s[3] > 0 and s[5] > 0 for s in shapes),
          "room, corridor and stamp each open rock and paint it", str(shapes))
    check(int(ov(build, "squares") or 0) > 9 and followed(build) == "populate",
          "built, the next step is Populate", f"squares {ov(build, 'squares')}, next -> {followed(build)}")
    pop = sec.get("populate", [])
    keep2_ent = world_file("p7_world", r"levels\keep2.ent")
    placed = monsters_in(keep2_ent)
    runs = [tuple(int(x) for x in m.groups()) for m in
            re.finditer(r"populate keep2: (\d+) monsters, (\d+) loot, (\d+) replaced", log)]
    check(len(runs) == 2, "the dialog's Populate and the console's both populate the viewed floor",
          str(runs))
    check(len(runs) == 2 and runs[0][0] > 0 and runs[1][2] == runs[0][0] + runs[0][1]
          and len(placed) == runs[1][0],
          "populating again replaces what the first populate placed: the file holds the second's alone",
          f"{runs}, file {len(placed)}")
    check(bool(placed) and set(placed) <= DIRT,
          "the hand-built floor is populated from the style's list", str(placed))
    check(ov(pop, "monsters") == str(len(placed)),
          "the overview counts what the file holds", f"{ov(pop, 'monsters')} vs {len(placed)}")
    check(followed(pop) in ("-", "check"),
          "populated, the next step is the check or play", str(followed(pop)))
    keep2_map = world_file("p7_world", r"levels\keep2.map")
    check(re.search(r"^tags cave vermin ooze\s*$", keep2_map, re.M) is not None
          and "dirt_cave" in theme_ids(keep2_map),
          "the empty floor carries the style's tags and room theme too")
finally:
    for w in P7:
        harness_game.remove_world(ROOT, w)
    drop()
    if settings_before is not None:
        io.open(SETTINGS, "wb").write(settings_before)
    elif os.path.isfile(SETTINGS):
        os.remove(SETTINGS)

# --- phase 19: the sprite arena holds the largest map ----------------------------
print("19 - the editor map of a 128x128 floor fits the sprite arena; past it, drops are counted")
# One quad a square, SpriteBatch::kQuadBytes (six 56-byte vertices).
QUAD_BYTES = 6 * 56
SPRITES = re.compile(r"^sprites last=(\d+) peak=(\d+) capacity=(\d+) .* lastdrops=(\d+) "
                     r"drops=(\d+) dropvertices=(\d+)")
VIEW = re.compile(r"^editor view: (\S+) (\d+)x(\d+) ")


def arena(lines):
    m = next((SPRITES.match(l) for l in lines if SPRITES.match(l)), None)
    return tuple(int(g) for g in m.groups()) if m else None


def viewed(lines):
    m = next((VIEW.match(l) for l in lines if VIEW.match(l)), None)
    return (m.group(1), int(m.group(2)), int(m.group(3))) if m else None


settings_before = io.open(SETTINGS, "rb").read() if os.path.isfile(SETTINGS) else None
fresh()
try:
    # WITH the window, and a short timeout: an arena that still aborts parks a
    # debug build on its CRT dialog, which is a failed run, not a ten-minute wait.
    log = run("spritearena.eval", headless=False, timeout=240)
    check(passed(log), "the script ran clean, to its verdict (no abort)")
    sec = console_sections(log)
    big, grown = sec.get("largest", []), sec.get("grown", [])
    a, v = arena(big), viewed(big)
    check(v is not None and v[1:] == (128, 128),
          "the editor views the generated floor, 128x128", str(v))
    cells = 128 * 128 * QUAD_BYTES
    # Non-vacuous: the frame must have DRAWN the cell layer - a render half that
    # never ran, or a view of some smaller level, reads far below it.
    check(a is not None and a[0] >= cells,
          f"that frame carried the whole cell layer ({cells} bytes of quads)",
          str(a))
    check(a is not None and a[4] == 0,
          "...and the arena dropped nothing (drops=0)", str(a))
    g, gv = arena(grown), viewed(grown)
    check(gv is not None and gv[1:] == (256, 256),
          "resized past the arena, the editor views it at 256x256", str(gv))
    check(g is not None and g[4] > 0 and g[5] >= 256 * 256 * 6,
          "...where the cell layer is dropped and counted, not aborted on", str(g))
    # lastdrops is ONE frame's count, closed by NewFrame. Read again a few frames
    # on, over the same view: every frame drops the same cell layer, so it is
    # not 0 (a count never copied reads 0) and it has not moved while the
    # lifetime total has (a count never reset grows with every frame). Comparing
    # it to `drops` in one reading cannot tell: a reading lands after a render
    # that dropped again and before NewFrame closes it, so even a running count
    # reads one below the total (measured: 5 against 6).
    later = arena(sec.get("later", []))
    check(g is not None and later is not None and 0 < g[3] == later[3]
          and later[4] > g[4],
          "...counted per frame too: the last frame's drops, not the run's",
          f"grown {g}, later {later}")
    warned = log.count("sprite arena full:")
    check(warned == 1, "...and the log says so, once",
          f"{warned} 'sprite arena full' warning(s)")
finally:
    drop()
    if settings_before is not None:
        io.open(SETTINGS, "wb").write(settings_before)
    elif os.path.isfile(SETTINGS):
        os.remove(SETTINGS)

# --- phase 20: the type editor ---------------------------------------------------
print("20 - the type editor: a control per row, stage ids typed a key at a time, swatches")
# What counts as a CONTROL, as the tree names the classes. A Label alone is not:
# a row that shows a field's name and nothing to edit it with is the defect.
CONTROLS = {"Checkbox", "Slider", "TextField", "Button", "DropDown"}
# Every category the palette can open a type editor on, AS DESIGNED (the
# kCategoryInfo table): the sweep must reach each, so a category that stopped
# opening fails here instead of shrinking the sweep.
TYPE_CATEGORIES = ("walls", "floors", "ceilings", "themes", "decorations", "fixtures",
                   "monsters", "buttons", "doors", "stairs", "items", "weapons", "armor",
                   "wallfeatures", "surfacefeatures", "effects", "dungeons", "terrain",
                   "quests", "flags", "styles", "shapes", "lights", "trails")
ROW = re.compile(r"console: typeset row (\S+) (\S+) (\S+) (\S+)\s*$")
ROWS = re.compile(r"console: typeset rows (\S+) '[^']*': (\d+) schema row\(s\), (\d+) built")
ET_STAGES = ("\r\n[et_stages]\r\ndisplay = Stage test\r\nstages = stage1 stage2\r\n"
             "text_stage1 = The first line\r\ntext_stage2 = The second line\r\n")


def type_rows(log):
    """{catalog: [(key, kind, widgets)]} and {catalog: (schema rows, rows built)}."""
    rows, counts = {}, {}
    for line in log.splitlines():
        m = ROW.search(line)
        if m:
            rows.setdefault(m.group(1), []).append(m.group(2, 3, 4))
            continue
        m = ROWS.search(line)
        if m:
            counts[m.group(1)] = (int(m.group(2)), int(m.group(3)))
    return rows, counts


def has_control(widgets):
    return any(w in CONTROLS for w in re.split(r"[+,]", widgets))


def fields(lines):
    """`typeset field k = v` lines as a dict."""
    out = {}
    for l in lines:
        m = re.match(r"typeset field (\S+) = ?(.*)$", l)
        if m:
            out[m.group(1)] = m.group(2).strip()
    return out


def cat_block(text, block_id):
    """One [id] block of a .cat file as a dict (None = absent)."""
    m = re.search(r"^\[" + re.escape(block_id) + r"\]\s*$(.*?)(?=^\[|\Z)", text, re.M | re.S)
    if not m:
        return None
    return {k.strip(): v.strip() for k, v in
            (l.split("=", 1) for l in m.group(1).splitlines() if "=" in l and not l.startswith(";"))}


# `quality` persists to settings.ini beside the exe: the developer's copy (or its
# absence) is put back afterwards, like phase 18's knobs.
settings_before = io.open(SETTINGS, "rb").read() if os.path.isfile(SETTINGS) else None
fresh()
try:
    quests = os.path.join(PROJ, r"catalog\quests.cat")
    io.open(quests, "a", encoding="utf-8", newline="").write(ET_STAGES)
    log = run("typeeditor.eval")
    check(passed(log), "the script ran clean")
    rows, counts = type_rows(log)
    missing = [c for c in TYPE_CATEGORIES if c not in counts]
    check(not missing, f"every category's type editor opened ({len(counts)} of {len(TYPE_CATEGORIES)})",
          f"never opened: {missing}")
    short = {c: counts[c] for c in counts if counts[c][0] != counts[c][1] or len(rows.get(c, [])) != counts[c][0]}
    check(not short, "each reported a row for every schema row", str(short))
    bare = [f"{c}.{k} ({kind}: {w})" for c, rs in rows.items() for k, kind, w in rs if not has_control(w)]
    total = sum(len(rs) for rs in rows.values())
    check(total > 200 and not bare, f"every one of the {total} schema rows built a control",
          "; ".join(bare[:8]))
    # The two rows the dialog used to skip (C101), named so a pass cannot be a
    # sweep that quietly left them out.
    dmg = [(c, k, w) for c, rs in rows.items() for k, kind, w in rs if kind == "damagetype"]
    check({(c, k) for c, k, _ in dmg} >= {("monsters", "dmgtype"), ("effects", "damage_type")}
          and all("DropDown" in w for _, _, w in dmg),
          "a damage-type row is a dropdown (monsters' dmgtype, effects' damage_type)", str(dmg))

    sec = console_sections(log)
    typing, saved = sec.get("stages", []), sec.get("saved", [])
    check(any("Another stage is called stage1" in l for l in typing),
          "typing \"stage1\" over stage2 is refused in the notice",
          " | ".join(l for l in typing if l.startswith("typeset dialog:")))
    want = {"stages": "stage1 stage10", "text_stage1": "The first line",
            "text_stage10": "The second line"}
    now = fields(typing)
    check(all(now.get(k) == v for k, v in want.items()),
          "stage2 -> stage10 a key at a time keeps stage1's line (the working copy)", str(now))
    back = fields(saved)
    check(all(back.get(k) == v for k, v in want.items()) and "text_stage2" not in back,
          "...and the Save wrote exactly that (the entry read back)", str(back))
    block = cat_block(io.open(quests, encoding="utf-8").read(), "et_stages") or {}
    check(all(block.get(k) == v for k, v in want.items()) and "text_stage2" not in block
          and "text_stage" not in block,
          "...and so does quests.cat on disk", str(block))

    # The "taken" notice follows what the fields SHOW. Each section raises it
    # first (checked, so a pass cannot be a notice that never went up), then
    # does the thing a latched flag got wrong; the status line is read just
    # before the section closes the dialog, where it does.
    TAKEN = "Another stage is called stage1"
    ARMED = "Click Delete again"

    def notices(name):
        """The notice after each step of section `name`: text, '' for none,
        None where the dialog was closed."""
        out = []
        for l in sec.get(name, []):
            m = re.match(r"typeset dialog: (?:open \S+ '[^']*' -(.*)|closed)$", l)
            if m:
                out.append(None if m.group(1) is None else m.group(1).strip())
        return out

    own, rebuild = notices("notice own"), notices("notice rebuild")
    other, rows_n = notices("notice other"), notices("notice rows")
    check(len(own) == 2 and TAKEN in (own[0] or "") and own[1] == "",
          "an id typed back to its own stage's id takes the \"taken\" notice down", str(own))
    check(len(rebuild) == 2 and TAKEN in (rebuild[0] or "") and rebuild[1] == "",
          "...and so does a rebuild (+ Add a stage resets every id field)", str(rebuild))
    check(len(other) == 4 and TAKEN in (other[0] or "") and ARMED in (other[1] or "")
          and ARMED in (other[2] or ""),
          "a unique id typed after another notice went up leaves that notice (Delete's) standing",
          str(other))
    check(len(rows_n) == 6 and TAKEN in (rows_n[2] or "") and TAKEN in (rows_n[3] or "")
          and rows_n[4] == "",
          "a row still showing a taken id keeps the notice up while another row renames; "
          "its own unique id takes it down", str(rows_n))

    # WINDOWED: a headless run never draws, and the freed swatches were a drawing
    # fault - a d3d12 error on a 0xdd... descriptor, then an access violation.
    # Still UNATTENDED (run_eval passes -unattended): a fault that ends in an
    # assert must end the run, not wait out the timeout on the CRT's abort box.
    log = run("typeswatch.eval", headless=False)
    check("crash: unattended" in log,
          "the windowed run is unattended (a fatal error exits; no dialog waits)")
    check(passed(log), "the windowed script ran to its verdict (the dialog drew through both changes)")
    check("Quality switched to Low" in log, "the textures really were reloaded under the open dialog")
    errors = [l for l in log.splitlines() if l.startswith("[ERROR]")]
    check(not errors, "a theme open in the type editor over a quality change leaves no fault in the log",
          " | ".join(e[:140] for e in errors[:3]))
finally:
    drop()
    if settings_before is not None:
        io.open(SETTINGS, "wb").write(settings_before)
    elif os.path.isfile(SETTINGS):
        os.remove(SETTINGS)

# --- phase 21: catalog sweeps and writes -------------------------------------------
print("21 - a feature type's rename follows its records; the code's ids stay; catround's cases")
# The catalogs whose ids are the code's (Project::IdentityInCode), each with an
# entry the script tries to rename and delete.
CODE_IDS = {"effects": "burn", "attacks": "stab", "spells": "flame", "balance": "formula"}
REFUSED = "The game's code defines this one"


def feature_records(text):
    """A .map's `floorfeature` / `ceilingfeature` records as sorted word tuples."""
    return sorted(tuple(l.split()[:4]) for l in text.splitlines()
                  if l.startswith(("floorfeature ", "ceilingfeature ")))


def block_ids(text):
    """A .cat file's [id] headers, in file order."""
    return re.findall(r"^\[([^\]]+)\]", text, re.M)


fresh()
try:
    # A level NOT loaded carries one of each, so the sweep must reach its file.
    crypt1 = os.path.join(PROJ, r"levels\crypt1.map")
    text = io.open(crypt1, encoding="utf-8", newline="").read()
    eol = "\r\n" if "\r\n" in text else "\n"
    io.open(crypt1, "w", encoding="utf-8", newline="").write(
        text + ("" if text.endswith(eol) else eol) +
        f"floorfeature recess 7 7{eol}ceilingfeature vault 7 7{eol}")
    sfc = os.path.join(PROJ, r"catalog\surfacefeatures.cat")
    sfc_before = io.open(sfc, encoding="utf-8", newline="").read()
    log = run("typesweep.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    placed, renamed = sec.get("1: features placed", []), sec.get("2: renamed", [])
    code, cat = sec.get("3: the code's ids", []), sec.get("4: catround", [])

    # THE CONTROL: the features really are in the geometry, or "unchanged after
    # the rename" would be satisfied by tiles that never drew.
    h = hashes(log)
    check(len(h) == 3 and h[1][2] != h[0][2] and h[1][3] != h[0][3],
          "two recesses and a vault move the arena's floor and ceiling", str(h))
    check("surfacefeatures 'recess': 3 level record(s), 0 other reference(s)" in placed and
          "surfacefeatures 'vault': 2 level record(s), 0 other reference(s)" in placed,
          "the sweep counts them on both levels (two recesses + a vault here, one of "
          "each on crypt1)", " | ".join(l for l in placed if l.startswith("surfacefeatures")))
    check("typeset rename surfacefeatures 'recess': done" in renamed and
          "typeset rename surfacefeatures 'vault': done" in renamed,
          "both renames go through", " | ".join(l for l in renamed if l.startswith("typeset rename")))
    check("surfacefeatures 'recess': 0 level record(s), 0 other reference(s)" in renamed and
          "surfacefeatures 'sunk': 3 level record(s), 0 other reference(s)" in renamed and
          "surfacefeatures 'dome': 2 level record(s), 0 other reference(s)" in renamed,
          "every record follows the rename - none left naming the old id",
          " | ".join(l for l in renamed if l.startswith("surfacefeatures")))
    check(len(h) == 3 and h[2] == h[1],
          "the geometry is exactly what it was - the tiles are found under their new names",
          f"{h[1:] if len(h) == 3 else h}")
    for new in ("sunk", "dome"):
        line = next((l for l in renamed if l.startswith(f"typeset delete surfacefeatures '{new}'")), "")
        check(line.startswith(f"typeset delete surfacefeatures '{new}': refused") and
              "crypt1" in line and "eval_arena" in line,
              f"deleting {new} is refused while it is placed, naming both levels", line)

    # ON DISK, after the savemap: both levels, and the entries renamed IN PLACE.
    arena = io.open(os.path.join(PROJ, r"levels\eval_arena.map"), encoding="utf-8").read()
    c1 = io.open(crypt1, encoding="utf-8").read()
    check(feature_records(c1) == [("ceilingfeature", "dome", "7", "7"),
                                  ("floorfeature", "sunk", "7", "7")],
          "on disk: crypt1, never loaded, names the new ids", str(feature_records(c1)))
    check(feature_records(arena) == [("ceilingfeature", "dome", "8", "5"),
                                     ("floorfeature", "sunk", "5", "5"),
                                     ("floorfeature", "sunk", "6", "5")],
          "on disk: the arena names the new ids", str(feature_records(arena)))
    sfc_after = io.open(sfc, encoding="utf-8", newline="").read()
    want_ids = [{"recess": "sunk", "vault": "dome"}.get(i, i) for i in block_ids(sfc_before)]
    check(block_ids(sfc_after) == want_ids and
          sfc_after.splitlines()[0] == sfc_before.splitlines()[0],
          "surfacefeatures.cat: renamed where they stood, its header kept",
          f"{block_ids(sfc_after)} vs {want_ids}")

    # THE CODE'S IDS: every rename and delete refused in its own words, and
    # the files exactly as the real world has them (the renames above saved
    # the whole project, so a write was not missing - it would have shown).
    for key, entry in CODE_IDS.items():
        for verb in ("rename", "delete"):
            line = next((l for l in code if l.startswith(f"typeset {verb} {key} '{entry}'")), "")
            check(line.startswith(f"typeset {verb} {key} '{entry}': refused") and REFUSED in line,
                  f"{verb} of {key} '{entry}' is refused: the code owns the id", line)
        # (Not `real`: that is the RealTree guard the run ends on.)
        theirs = io.open(os.path.join(PROJECTS, "dungeon-demo", "catalog", key + ".cat"), "rb").read()
        mine = io.open(os.path.join(PROJ, "catalog", key + ".cat"), "rb").read()
        check(mine == theirs and f"[{entry}]".encode() in mine,
              f"{key}.cat is as it was, [{entry}] in it")
    dialog = [l for l in code if l.startswith("typeset dialog: open effects 'burn'")]
    # Opened with no notice, then Delete's click: the refusal, never the arming.
    check(len(dialog) == 2 and REFUSED not in dialog[0] and REFUSED in dialog[1]
          and "Click Delete again" not in dialog[1],
          "the type editor on an effect answers Delete with why instead of arming it",
          " | ".join(dialog))

    # catround: the world's files, then the three cases no project carries.
    m = next((re.match(r"catround (\d+) of (\d+) file\(s\) round-trip, (\d+) absent", l)
              for l in cat if l.startswith("catround ") and "file(s)" in l), None)
    check(m is not None and m.group(1) == m.group(2) and m.group(3) == "0",
          "catround: every file of the world round-trips after the saves above",
          m.group(0) if m else " | ".join(cat))
    for name in ("header-only", "first-entry", "monster-config"):
        line = next((l for l in cat if l.startswith(f"catround case {name}:")), "")
        check(line == f"catround case {name}: ok", f"catround case {name} keeps every comment", line)
    check("catround cases 3 of 3 pass" in cat, "...and there are exactly three",
          " | ".join(l for l in cat if l.startswith("catround cases")))
finally:
    drop()

# --- phase 22: the map's monster icons stand in their idles -----------------------
print("22 - each monster's map icon is posed on its idle, framed on that pose, with its own palette")
# pose / frame / palette are what the bake RECORDED drawing (MonsterKind::
# iconDrawn), never the inputs it was handed; bind and posed (the kind's idle at
# its first frame, on a fresh animator) are measured beside them by the readout.
ICON = re.compile(r"^mapicon (\S+) joints=(\d+) pose=(\S+) bind=([\d.]+)x([\d.]+) "
                  r"frame=([\d.]+)x([\d.]+) posed=([\d.]+)x([\d.]+) palette=(\S+) baked=(\d)")
BAKE = re.compile(r"^mapicons bake passes=(\d+) kinds=(\d+) skinned=(\d+) uploads=(\d+) "
                  r"reuses=(\d+)")
# The bought kit: Mixamo rigs bound in a T-pose, so their idle is far narrower.
KIT = ("skel_warrior", "skel_bare", "skel_berserker", "skel_spearman")


def catalog_idles():
    """monsters.cat, read here: id -> its first `anim_idle` clip ("" = none named)."""
    out, cur = {}, None
    for line in read(r"catalog\monsters.cat").splitlines():
        line = line.strip()
        m = re.match(r"^\[(\S+)\]$", line)
        if m:
            cur = m.group(1)
            out[cur] = ""
        elif cur and line.startswith("anim_idle") and "=" in line:
            words = line.split("=", 1)[1].replace(",", " ").split()
            out[cur] = words[0] if words else ""
    return out


fresh()
try:
    log = run("mapicons.eval", headless=False, timeout=300)
    check(passed(log), "the script ran clean, to its verdict")
    lines = console_sections(log).get("icons", [])
    icons = {m.group(1): m.groups()[1:] for m in (ICON.match(l) for l in lines) if m}
    bake = next((tuple(int(g) for g in BAKE.match(l).groups()) for l in lines
                 if BAKE.match(l)), None)
    idles = catalog_idles()
    check(len(idles) > 0 and set(icons) == set(idles)
          and all(v[9] == "1" for v in icons.values()),
          f"every catalog kind ({len(idles)}) was loaded and its icon baked",
          f"catalog {sorted(idles)}, reported {sorted(icons)}")
    # The kind's idle: its catalog's first anim_idle clip, else the rig's `idle`.
    wrong = {k: (v[1], idles.get(k) or "idle") for k, v in icons.items()
             if v[1] != (idles.get(k) or "idle")}
    named = [k for k in icons if idles.get(k)]
    check(not wrong and len(named) >= 4,
          f"each icon is drawn in its kind's idle ({len(named)} named by anim_idle)",
          f"drawn / expected: {wrong}")
    # The palette the bake handed the renderer IS the idle's first frame: a bake
    # drawing from any other animator - an un-Played one, a throwaway rest pose -
    # or from none fingerprints differently.
    other = sorted(k for k, v in icons.items() if v[8] != "same")
    check(not other, "...with that pose's own palette, every kind",
          f"drawn with another pose: {other}")
    # The box the bake framed on is that pose's, to the readout's 3 decimals...
    off = {k: (f"{v[4]}x{v[5]}", f"{v[6]}x{v[7]}") for k, v in icons.items()
           if abs(float(v[4]) - float(v[6])) > 0.0015 or abs(float(v[5]) - float(v[7])) > 0.0015}
    check(not off, "the head shots are framed on the pose drawn, every kind",
          f"framed / posed boxes: {off}")
    # ...which for the kit is far narrower than the T-pose it is bound in.
    narrower = {k: (icons[k][4], icons[k][2]) for k in KIT if k in icons
                and float(icons[k][4]) < 0.8 * float(icons[k][2])}
    check(len(narrower) == len(KIT),
          "...the kit's narrower than its T-pose",
          f"frame / bind widths: {[(k, icons[k][4], icons[k][2]) for k in KIT if k in icons]}")
    skinned = [k for k, v in icons.items() if int(v[0]) > 0]
    sizes = {}
    for k in skinned:
        sizes.setdefault(icons[k][0], []).append(k)
    same = max((len(v) for v in sizes.values()), default=0)
    check(bake is not None and bake[1] == len(icons) and bake[2] == len(skinned),
          "the last pass baked them all at once", f"bake {bake}, {len(icons)} kinds")
    check(bake is not None and bake[3] == len(skinned) and same >= 2,
          f"...each rig with its own palette: one upload per skinned kind "
          f"({len(skinned)}), up to {same} rigs the same size",
          f"bake {bake} (passes, kinds, skinned, uploads, reuses); joint counts {sizes}")
finally:
    drop()

# --- phase 23: the monster inspector and patrol routes ----------------------------
print("23 - the monster inspector and patrol routes: the right monster, a live one, this world's")
NEXT = "et_next"  # routeworld.eval switches to it by name
# Both dialogs report three spells: the one the monster or kind OPENED with
# (what a load gave it), the working copy's (which Open may have defaulted)
# and the one the Caster row shows. A reload check reads `opened`: the other
# two would show the default even had the save dropped the spell.
INSP = re.compile(r"editor inspector: monster (\d+) (\S+) (live|gone) tab (-?\d+) waypoints (\d+) "
                  r"archetype (\S+) opened '([^']*)' spell '([^']*)' shown '([^']*)'$")
ROUTE = re.compile(r"editor route: (?:laying monster (\d+) \((\d+) waypoint\(s\)\)|none)$")
MDLG = re.compile(r"monsterdialog: open (\S+) archetype (\S+) opened '([^']*)' spell '([^']*)' "
                  r"shown '([^']*)'$")
NOT_ROUTES = "not the route's"


def inspectors(lines):
    """Each monster inspector status in `lines`, as a dict."""
    out = []
    for l in lines:
        m = INSP.match(l)
        if m:
            out.append({"id": int(m.group(1)), "type": m.group(2), "live": m.group(3) == "live",
                        "tab": int(m.group(4)), "waypoints": int(m.group(5)),
                        "archetype": m.group(6), "opened": m.group(7), "spell": m.group(8),
                        "shown": m.group(9)})
    return out


def dialogs(lines):
    """Each monster type dialog status in `lines` (console or raw log lines), as a dict."""
    out = []
    for l in lines:
        m = MDLG.search(l)
        if m:
            out.append({"line": m.group(0), "type": m.group(1), "archetype": m.group(2),
                        "opened": m.group(3), "spell": m.group(4), "shown": m.group(5)})
    return out


def drop_ent_param(path, kind, x, z, key):
    """Take `key=...` off the .ent record of `kind` at x,z (a scratch world's file)."""
    text = io.open(path, encoding="utf-8", newline="").read()
    out = []
    for l in text.splitlines(keepends=True):
        w = l.split()
        if w[:1] == ["monster"] and w[1:4] == [kind, str(x), str(z)]:
            body = l.rstrip("\r\n")
            l = " ".join(t for t in body.split() if not t.startswith(key + "=")) + l[len(body):]
        out.append(l)
    io.open(path, "w", encoding="utf-8", newline="").write("".join(out))


def drop_cat_field(path, block_id, key):
    """Take the `key = ...` line out of one [id] block of a .cat file (a scratch world's)."""
    text = io.open(path, encoding="utf-8", newline="").read()
    m = re.search(r"^\[" + re.escape(block_id) + r"\]\s*$(.*?)(?=^\[|\Z)", text, re.M | re.S)
    if not m:
        return
    body = re.sub(r"^" + re.escape(key) + r"\s*=.*(\r?\n)?", "", m.group(1), flags=re.M)
    io.open(path, "w", encoding="utf-8", newline="").write(text[:m.start(1)] + body + text[m.end(1):])


def routes(lines):
    """Each route status in `lines`: (monster id, waypoints), or None for no route."""
    out = []
    for l in lines:
        m = ROUTE.match(l)
        if m:
            out.append((int(m.group(1)), int(m.group(2))) if m.group(1) else None)
    return out


def route_keys(lines):
    """The answers to `editor route key ...`, in order: taken / not the route's."""
    return [l.split(": ", 1)[1] for l in lines if l.startswith("editor route key ")]


def after_command(lines, prefix):
    """The lines after the first echoed command starting with `prefix`."""
    for i, l in enumerate(lines):
        if l.startswith("> " + prefix):
            return lines[i + 1:]
    return []


def ent_line(text, kind, x, z):
    """The .ent record of `kind` at x,z (its words), or None."""
    for l in text.splitlines():
        w = l.split()
        if w[:1] == ["monster"] and w[1:4] == [kind, str(x), str(z)]:
            return w
    return None


def level_files(world):
    """{file name: bytes} of a world's levels folder."""
    folder = os.path.join(PROJECTS, world, "levels")
    return {n: io.open(os.path.join(folder, n), "rb").read() for n in sorted(os.listdir(folder))}


settings_before = io.open(SETTINGS, "rb").read() if os.path.isfile(SETTINGS) else None
fresh()
try:
    # 1. THE INSPECTOR AND ITS ROUTE, on et_demo.
    log = run("inspectroute.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    c80, c104, player, caster = (sec.get(n, []) for n in ("c80", "c104", "player", "caster"))

    ins = inspectors(c80)
    a = ins[0] if ins else {}
    b = ins[1] if len(ins) > 1 else {}
    # THE CONTROL: two different monsters really were inspected, and A's route
    # really was laid - or "it reopened on A with three" could be A all along.
    check(a.get("type") == "skel_warrior" and b.get("type") == "skeleton" and a.get("id") != b.get("id"),
          "A (the warrior) and then B (the skeleton) were inspected mid-route", str(ins[:2]))
    # (Every route click prints the route as it then stands.)
    check(routes(c80)[:4] == [(a.get("id"), n) for n in (1, 2, 3, 3)],
          "A's route was laid with three waypoints", str(routes(c80)))
    check(route_keys(c80) == ["taken"] and routes(c80)[-1:] == [None],
          "Enter on the editor map finished it", f"{route_keys(c80)} {routes(c80)}")
    back = ins[2] if len(ins) > 2 else {}
    check(back.get("id") == a.get("id") and back.get("type") == "skel_warrior" and back.get("waypoints") == 3
          and back.get("live"),
          "the inspector reopened on A - not B, the last one inspected - with A's three waypoints",
          str(back))

    ins = inspectors(c104)
    check(ins[:1] and ins[0]["tab"] == 1 and ins[0]["waypoints"] == 3,
          "the Patrol tab is up, showing three waypoints", str(ins[:1]))
    cleared = inspectors(after_command(c104, "editor inspector clearroute"))
    check(cleared[:1] and cleared[0]["tab"] == 1 and cleared[0]["waypoints"] == 0,
          "Clear route leaves the Patrol tab up, showing none (it used to fall back to the AI tab)",
          str(cleared[:1]))

    aid = a.get("id")
    check(route_keys(player) == [NOT_ROUTES, NOT_ROUTES, "taken", "taken"],
          "Backspace and Esc on the player map are not the route's; on the editor map they are",
          str(route_keys(player)))
    check(routes(player) == [(aid, 1), (aid, 2), (aid, 2), (aid, 2), (aid, 2), (aid, 2),
                             (aid, 1), (aid, 1), None, None],
          "the player map left both waypoints and the route; the editor's Backspace took one back",
          str(routes(player)))
    ins = inspectors(player)
    check(ins[-1:] and ins[-1]["id"] == aid and ins[-1]["waypoints"] == 1,
          "...and its Esc reopened A's inspector on the one left", str(ins))

    ins = inspectors(caster)
    spell = ins[0]["spell"] if ins else ""
    check(len(ins) == 1 and ins[0]["type"] == "skeleton" and ins[0]["archetype"] == "caster"
          and spell != "" and ins[0]["shown"] == spell,
          "B made a Caster in its inspector holds the spell its row shows", str(ins))
    d = (dialogs(after_command(caster, "monsterdialog archetype")) or [{}])[0]
    type_spell = d.get("spell", "")
    check(d.get("type") == "skel_coward" and d.get("archetype") == "caster"
          and type_spell != "" and d.get("shown") == type_spell,
          "the skel_coward TYPE made a Caster in the monster dialog holds the spell its row shows",
          d.get("line") or " | ".join(l for l in caster if l.startswith("monsterdialog")))
    arena = io.open(os.path.join(PROJ, r"levels\eval_arena.ent"), encoding="utf-8").read()
    rec = ent_line(arena, "skeleton", 12, 6) or []
    check("archetype=caster" in rec and f"spell={spell}" in rec,
          "the inspector's Save wrote the spell on the .ent record", " ".join(rec))
    coward = cat_block(io.open(os.path.join(PROJ, r"catalog\monsters.cat"), encoding="utf-8").read(),
                       "skel_coward") or {}
    check(coward.get("archetype") == "caster" and coward.get("spell") == type_spell,
          "the monster dialog's Save wrote the spell into monsters.cat",
          str({k: coward.get(k) for k in ("archetype", "spell")}))

    # 2. ...AND A NEW PROCESS LOADS THEM: the kind is built and the skeleton
    # spawns from the files those saves wrote.
    log = run("casterload.eval")
    check(passed(log), "the reload ran clean")
    warns = [l for l in log.splitlines() if "archetype=caster but no spell" in l]
    check(not warns, "no caster-without-a-spell warning at the load", " | ".join(warns[:2]))
    # OPENED, not only the working copy: Open defaults a spell-less caster's
    # working copy to the first spell offered - the very one the pick above
    # defaulted to - so the working copy would read the same had the Save
    # dropped the spell.
    ins = inspectors([l.split("console: ", 1)[1] for l in log.splitlines() if "console: " in l])
    check(ins[:1] and ins[0]["archetype"] == "caster"
          and ins[0]["opened"] == spell == ins[0]["spell"] == ins[0]["shown"],
          "the skeleton loads a Caster with its spell (it opens on it, not on the default)",
          str(ins[:1]))
    d = (dialogs(log.splitlines()) or [{}])[0]
    check(d.get("archetype") == "caster"
          and d.get("opened") == type_spell == d.get("spell") == d.get("shown"),
          "the skel_coward type loads a Caster with its spell (it opens on it, not on the default)",
          d.get("line", "no status"))

    # 3. A CASTER WITH NO SPELL, as a file can hold one: the spells taken back
    # out of this scratch world's files, and a new process loads them.
    ent = os.path.join(PROJ, r"levels\eval_arena.ent")
    cat = os.path.join(PROJ, r"catalog\monsters.cat")
    drop_ent_param(ent, "skeleton", 12, 6, "spell")
    drop_cat_field(cat, "skel_coward", "spell")
    rec = ent_line(io.open(ent, encoding="utf-8").read(), "skeleton", 12, 6) or []
    coward = cat_block(io.open(cat, encoding="utf-8").read(), "skel_coward") or {}
    check("archetype=caster" in rec and not any(t.startswith("spell=") for t in rec)
          and coward.get("archetype") == "caster" and "spell" not in coward,
          "the scratch files now hold both casters without a spell",
          f"{' '.join(rec)} | {coward.get('archetype')} {coward.get('spell')}")
    log = run("casterdefault.eval")
    check(passed(log), "the spell-less load ran clean")
    # THE CONTROL for the reload's "no warning" check: the warning it looks
    # for does come, for the kind and for the instance, when a spell is lost.
    warns = [l for l in log.splitlines() if "archetype=caster but no spell" in l]
    check(any("[skel_coward]" in l for l in warns) and any("the skeleton at 12,6" in l for l in warns),
          "THE CONTROL: the load warns for the spell-less kind and instance",
          " | ".join(warns[:3]) or "no warning")
    sec = console_sections(log)
    ins = inspectors(sec.get("inspector", []))
    first = ins[0] if ins else {}
    check(first.get("archetype") == "caster" and first.get("opened") == ""
          and first.get("spell") == spell == first.get("shown"),
          "a spell-less Caster's inspector opens on none, holding and showing the first spell offered",
          str(ins[:1]))
    check(len(ins) == 2 and ins[1]["id"] == first.get("id") and ins[1]["opened"] == "",
          "...and its Esc put the spell-less original back: reopened, it opens on none again",
          str(ins))
    dl = dialogs(sec.get("dialog", []))
    first = dl[0] if dl else {}
    check(first.get("archetype") == "caster" and first.get("opened") == ""
          and first.get("spell") == type_spell == first.get("shown"),
          "a spell-less Caster type's dialog opens on none, holding and showing the first spell offered",
          first.get("line", "no status"))
    check(len(dl) == 2 and dl[1]["opened"] == "",
          "...and its Esc put the spell-less kind back: reopened, it opens on none again",
          " | ".join(x["line"] for x in dl))
finally:
    drop()

fresh()
harness_game.scratch_world(ROOT, NEXT)
try:
    # 4. A ROUTE DOES NOT OUTLIVE ITS WORLD (C233).
    before = level_files(NEXT)
    log = run("routeworld.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    pre, post, ids = sec.get("before", []), sec.get("after", []), sec.get("ids", [])
    ins = inspectors(pre)
    aid = ins[0]["id"] if ins else None
    check(routes(pre)[-1:] == [(aid, 1)] and aid is not None,
          "a route was being laid on A before the switch", str(routes(pre)))
    check("switching to " + NEXT in pre, "the script switched worlds")
    check(routes(post) and all(r is None for r in routes(post)),
          "after the switch no route is being laid", str(routes(post)))
    check(route_keys(post) == [NOT_ROUTES] and "editor inspector: closed" in post
          and "editor inspector save: no inspector is open" in post,
          "on the player map Esc is not the route's: no inspector opens, its Save has nothing to save",
          " | ".join(l for l in post if l.startswith("editor ")))
    nxt = inspectors(ids)
    check(nxt[:1] and nxt[0]["type"] == "skel_warrior" and nxt[0]["id"] == aid,
          "THE CONTROL: the next world's warrior holds A's id, so a stale route would have reached it",
          f"{nxt[:1]} vs A {aid}")
    check(level_files(NEXT) == before, "the next world's level files are untouched")
finally:
    harness_game.remove_world(ROOT, NEXT)
    drop()

fresh()
try:
    # 5. SAVING THE MONSTER'S TYPE (C232), WINDOWED: the fault was in the drawing.
    log = run("routerespawn.eval", headless=False)
    check("crash: unattended" in log, "the windowed run is unattended (a fatal error exits)")
    check(passed(log), "the windowed script ran to its verdict")
    sec = console_sections(log)
    rt, op, dl = sec.get("route", []), sec.get("open", []), sec.get("dialog", [])
    ins = inspectors(rt)
    aid = ins[0]["id"] if ins else None
    saved = after_command(rt, "typeset")
    check(routes(rt[:len(rt) - len(saved)])[-1:] == [(aid, 1)] and aid is not None,
          "a route was being laid on the warrior", str(routes(rt)))
    check(any(l.startswith("typeset monsters 'skel_warrior': hp = 22") for l in rt),
          "its type was saved mid-route", " | ".join(l for l in rt if l.startswith("typeset")))
    check(routes(saved) and all(r is None for r in routes(saved)),
          "the save respawned the warrior under a new id, and the route ended with the old one",
          str(routes(saved)))
    check(route_keys(rt) == [NOT_ROUTES] and "editor inspector: closed" in after_command(rt, "editor route key"),
          "Enter afterwards reopens nothing", " | ".join(after_command(rt, "editor route key")[:3]))
    ins = inspectors(op)
    check(ins[:1] and ins[0]["type"] == "skel_warrior" and ins[0]["live"] and ins[0]["id"] != aid,
          "the respawned warrior (a new id) has its inspector open", str(ins[:1]))
    tail = [l for l in after_command(op, "typeset") if l.startswith("editor inspector")]
    check(len(tail) == 3 and all(l == "editor inspector: closed" for l in tail),
          "a save of its type closes it first", " | ".join(tail))
    opened = [l for l in dl if MDLG.match(l)]
    tail = [l for l in after_command(dl, "typeset") if l.startswith("monsterdialog")]
    check(len(opened) == 1 and tail and all(l == "monsterdialog: closed" for l in tail),
          "...and the monster dialog", " | ".join(opened + tail))
    errors = [l for l in log.splitlines() if l.startswith("[ERROR]")]
    check(not errors, "no fault in the log through the saves", " | ".join(e[:140] for e in errors[:3]))
finally:
    drop()
    if settings_before is not None:
        io.open(SETTINGS, "wb").write(settings_before)
    elif os.path.isfile(SETTINGS):
        os.remove(SETTINGS)

# --- phase 24: Esc backs out one layer ----------------------------------------------
print("24 - Esc backs out one layer: an open list before its dialog, the editor's and the world's ladders")
DUST = re.compile(r"dust on, density ([\d.]+)$")
LEVELSET = re.compile(r"editor levelsettings: open \S+ dust ([\d.]+) haze \S+ ambient \S+ popup (open|shut)$")


def esc_chunks(lines, want):
    """`lines` cut at each `presskey esc`: what stood before the first key,
    between each two, and after the last - `want` pieces, padded empty."""
    out = [[]]
    for l in lines:
        if l == "> presskey esc":
            out.append([])
        else:
            out[-1].append(l)
    return out + [[] for _ in range(want - len(out))]


def prefixed(lines, prefix):
    """What follows `prefix` on each line that starts with it."""
    return [l[len(prefix):] for l in lines if l.startswith(prefix)]


def said(lines):
    """The game's answers, without the echoed commands and the key's own line."""
    return [l for l in lines if not l.startswith("> ") and not l.startswith("presskey: ")]


def dusts(lines):
    return [float(m.group(1)) for m in map(DUST.match, lines) if m]


def level_dialog(lines):
    """Each open Level settings status: (dust, popup open|shut)."""
    return [(float(m.group(1)), m.group(2)) for m in map(LEVELSET.match, lines) if m]


fresh()
try:
    log = run("dialogesc.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)

    # THE INSPECTOR: a live edit (the warrior made a Caster), then its Facing
    # list opened.
    c = esc_chunks(sec.get("inspector", []), 3)
    ins = inspectors(c[0])
    orig = ins[0] if ins else {}
    check(len(ins) == 2 and orig.get("archetype") not in (None, "caster") and ins[1]["archetype"] == "caster",
          "THE CONTROL: the warrior was made a Caster in its inspector, a live edit", str(ins))
    check(prefixed(c[0], "editor inspector popup: ")[-1:] == ["open"],
          "THE CONTROL: the inspector's Facing list was open when the first Esc came",
          " | ".join(prefixed(c[0], "editor inspector popup: ")))
    ins = inspectors(c[1])
    check(ins[:1] and ins[0]["id"] == orig.get("id") and ins[0]["archetype"] == "caster",
          "the first Esc left the inspector open with the Caster edit (it used to cancel the dialog)",
          str(ins) or " | ".join(l for l in c[1] if l.startswith("editor inspector")))
    check(prefixed(c[1], "editor inspector popup: ") == ["shut"], "...and closed the list",
          " | ".join(prefixed(c[1], "editor inspector popup: ")))
    tail = [l for l in c[2] if l.startswith("editor inspector")]
    check(tail[:1] == ["editor inspector: closed"], "the second Esc cancelled the dialog", " | ".join(tail[:2]))
    ins = inspectors(c[2])
    check(ins[:1] and ins[0]["archetype"] == orig.get("archetype"),
          f"...putting the edit back: reopened, the warrior is a {orig.get('archetype')} again", str(ins))

    # LEVEL SETTINGS: dust typed in (previewed live), the material list opened.
    c = esc_chunks(sec.get("level", []), 3)
    before = dusts(c[0])[:1]
    check(before and abs(before[0] - 0.31) > 0.005 and dusts(c[0])[1:2] == [0.31]
          and level_dialog(c[0])[-1:] == [(0.31, "open")],
          f"THE CONTROL: dust typed into Level settings is previewed ({before} -> 0.31), its material list open",
          f"{dusts(c[0])} {level_dialog(c[0])}")
    check(level_dialog(c[1]) == [(0.31, "shut")] and dusts(c[1]) == [0.31],
          "the first Esc closed only the list: the dialog open, the dust still 0.31 (it used to revert and close)",
          f"{level_dialog(c[1])} {dusts(c[1])} {said(c[1])}")
    check("editor levelsettings: closed" in c[2] and dusts(c[2]) == before,
          f"the second closed the dialog and put the dust back to {before}", " | ".join(said(c[2])))

    # THE GENERATE AND NEW WORLD DIALOGS: a list open, nothing to revert.
    for name, prefix, what in (("generate", "generate dialog popup: ", "the Generate dialog's Style list"),
                               ("newworld", "new world dialog popup: ", "the New world dialog's level list")):
        steps = [prefixed(ch, prefix) for ch in esc_chunks(sec.get(name, []), 3)]
        check(steps[0][-1:] == ["dialog open popup open"], f"THE CONTROL: {what} was open", str(steps[0]))
        check(steps[1] == ["dialog open popup shut"],
              "the first Esc closed only the list (it used to close the dialog)", str(steps[1]))
        check(steps[2] == ["dialog closed popup shut"], "the second closed the dialog", str(steps[2]))

    # THE CREATE DIALOG (decorations' "+ New..."), which had no Esc at all.
    steps = [prefixed(ch, "editor newasset: ") for ch in esc_chunks(sec.get("asset", []), 3)]
    check(steps[0] == ["open popup shut", "open popup open"],
          "THE CONTROL: the create dialog opened, then its source list", str(steps[0]))
    check(steps[1] == ["open popup shut"], "the first Esc closed only the list", str(steps[1]))
    check(steps[2] == ["closed"], "the second closed the dialog (no key could, before)", str(steps[2]))

    # THE EDITOR'S LADDER: a brush armed, the toolbar's level list open.
    steps = [prefixed(ch, "editor levellist: ") for ch in esc_chunks(sec.get("levellist", []), 4)]
    armed = "armed decorations:column"
    check(steps[0][-1:] == [f"open map editor {armed}"],
          "THE CONTROL: a brush armed, the toolbar's level list open", str(steps[0]))
    check(steps[1] == [f"shut map editor {armed}"],
          "the first Esc closed only the list - brush armed, editor up (it used to put the brush down "
          "and leave the list open)", str(steps[1]))
    check(steps[2] == ["shut map editor armed -"], "the second put the brush down", str(steps[2]))
    check(steps[3] == ["shut map closed armed -"], "the third closed the editor", str(steps[3]))

    # THE WORLD EDITOR: a terrain brush armed on the travel screen.
    c = [said(ch) for ch in esc_chunks(sec.get("world", []), 4)]
    check(c[0][-3:] == ["world editing (fog off)", "on the world map", "armed: moor"],
          "THE CONTROL: on the travel screen, editing, the moor brush armed", str(c[0]))
    check(c[1] == ["no terrain armed", "state worldmap"],
          "the first Esc put the brush down and stayed on the travel screen (it used to pause)", str(c[1]))
    check(c[2] == ["state paused"], "the second paused", str(c[2]))
    check(c[3][:1] == ["state worldmap"], "the third resumed on the travel screen", str(c[3]))

    errors = [l for l in log.splitlines() if l.startswith("[ERROR]")]
    check(not errors, "no error in the log", " | ".join(e[:140] for e in errors[:3]))
finally:
    drop()

# --- phase 25: a surface's relief is its texture set's own -----------------------
print("25 - the type editor's relief row names the texture set's own (the baker's record)")
# The record is the baker's (Assets/WornSets.h); `AssetBaker wornsets` prints it,
# so the expected numbers are asked of the same binary pair rather than typed
# here. The debug baker: this judge reads build\debug.
BAKER = os.path.join(os.path.dirname(EXE), "AssetBaker.exe")
ET_WORN = ("\r\n[et_unlisted]\r\ndisplay = No record\r\ntexture = et_nosuchset\r\n"
           "\r\n[et_tuned]\r\ndisplay = Tuned brick\r\ntexture = wall_brick\r\nrelief = 0.07\r\n")
DERIVED = re.compile(r"typeset derived (\S+) = (\S+)$")
RELIEF_ROW = re.compile(r"typeset row walls relief float (\S+)$")


def derived_relief(lines):
    vals = [float(m.group(2)) for m in map(DERIVED.match, lines) if m and m.group(1) == "relief"]
    return vals[0] if len(vals) == 1 else None


records = {}
r = subprocess.run([BAKER, "wornsets"], capture_output=True, text=True, errors="replace")
for line in r.stdout.splitlines():
    parts = line.split()
    if len(parts) == 4:
        records[parts[0]] = float(parts[2])
check(r.returncode == 0 and len(records) > 40, "the baker lists its worn-set records",
      f"exit {r.returncode}, {len(records)} records")
fresh()
try:
    io.open(os.path.join(PROJ, r"catalog\walls.cat"), "a", encoding="utf-8", newline="").write(ET_WORN)
    log = run("wornrelief.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    # (section, the set its type binds, what the schema's default USED to say)
    for name, setname, old in (("wall", "cobblestone_wall", 0.055), ("floor", "cobblestone_floor", 0.045),
                               ("ceiling", "ceiling_rock_porous", 0.08)):
        got = derived_relief(sec.get(name, []))
        want = records.get(setname)
        check(got is not None and want is not None and abs(got - want) < 5e-5 and abs(want - old) > 1e-4,
              f"a {name} type names its set's relief ({setname} {want}, not the old default {old})",
              f"derived {got}")
    got = derived_relief(sec.get("unlisted", []))
    check(got is not None and abs(got - 0.055) < 5e-5,
          "a type on a set no record lists names its kind's default (0.055 for a wall)", f"derived {got}")
    got = derived_relief(sec.get("tuned", []))
    check(got is not None and abs(got - records.get("wall_brick", -1)) < 5e-5,
          "a type that sets its own relief still names the set's beside its slider", f"derived {got}")
    rows = {s: [m.group(1) for m in map(RELIEF_ROW.match, sec.get(s, [])) if m] for s in ("wall", "tuned")}
    check(rows["wall"] == ["Checkbox"] and len(rows["tuned"]) == 1 and "Slider" in rows["tuned"][0],
          "unset, the relief row is the derived checkbox; set, a slider", str(rows))
    check("end" in sec, "the script ran to its end")
finally:
    drop()

# --- phase 26: the cell message in the player's language ---------------------------
print("26 - the cell message and the projectile card speak the player's language; `editor cell` stays the dev's")
LANGS = os.path.join(ROOT, r"assets\lang")


def lang_table(code):
    """{key: text} of assets/lang/<code>.lang, read the way Core/Loc reads it."""
    table = {}
    for line in io.open(os.path.join(LANGS, code + ".lang"), encoding="utf-8-sig").read().splitlines():
        s = line.strip(" \t\r")
        if s and not s.startswith(";") and "=" in s:
            key, text = s.split("=", 1)
            table[key.strip(" \t")] = text.strip(" \t")
    return table


MISSING = "\x02no key {}\x02"  # what say() gives for a key the table lacks


def say(table, key, *args):
    """The line loc::Format makes of `key`: each {} filled in order. A key the
    table lacks gives MISSING, which no game output can match: the game would
    show the raw key there, and an expected line built from that same raw key
    would agree with it - a pattern with no holes formats to itself - so a
    missing key would pass as present."""
    if key not in table:
        return MISSING.format(key)
    parts = table[key].split("{}")
    return parts[0] + "".join(str(a) + p for a, p in zip(args, parts[1:]))


# The squares the script inspects on the active level, in its order. 7,5 holds
# two skeletons the judge authors into the scratch arena's .ent (the editor
# places one monster a square), so the plural monster key is said as well as
# the `.one`.
TWO_MONSTERS = ("monster skel_swarm 7 5 south", "monster skel_swarm 7 5 south")


def cell_lines(table):
    """Each square's line, in the order the script inspects them."""
    floor, wall = say(table, "map.select.floor"), say(table, "map.select.wall")
    one = say(table, "map.select.monsters.one")
    joined = lambda a, b: say(table, "map.joined", a, b)
    return [say(table, "map.select.contents", 0, 0, wall),
            say(table, "map.select.contents", 23, 9, joined(floor, one)),
            say(table, "map.select.contents", 5, 5,
                joined(joined(floor, one), say(table, "map.select.props", 2))),
            say(table, "map.select.contents", 9, 5, joined(floor, say(table, "map.select.props.one"))),
            say(table, "map.select.contents", 7, 5, joined(floor, say(table, "map.select.monsters", 2))),
            # crypt1, browsed: the base word alone, even of its skeleton's square
            say(table, "map.select.contents", 0, 0, wall),
            say(table, "map.select.contents", 10, 4, floor)]


NUM = "\x01"  # a number's place in an expected line (any decimal there)


def shot_rows(table):
    """The thrown torch's card as `table` words it - (label, value) a row, a
    value with NUM where its number goes, None where any damage type's name will
    do - and the torch's on_hit is a burn with a chance, so its payload line
    takes both of its keys."""
    t = lambda key, *a: say(table, key, *a)
    return [(t("map.proj.side"), t("map.proj.fromparty")),
            (t("map.proj.dmgtype"), None),
            (t("map.proj.damage"), NUM),
            (t("map.proj.accuracy"), t("map.proj.accuracy.value", NUM)),
            (t("map.proj.speed"), t("map.proj.speed.value", NUM)),
            (t("map.proj.range"), t("map.proj.range.value", NUM)),
            (t("map.proj.payload"),
             t("map.proj.payload.chance", t("map.proj.payload.dot", "burn", NUM, NUM), NUM))]


def row_says(want, got):
    """Whether the card's value `got` is the expected `want` (NUM = a number)."""
    pattern = re.escape(want).replace(re.escape(NUM), r"\d+(?:\.\d+)?")
    return re.fullmatch(pattern, got) is not None


def card(lines):
    """The `editor projectile: <label> = <value>` rows, as (label, value)."""
    return [tuple(l[len("editor projectile: "):].split(" = ", 1)) for l in lines
            if l.startswith("editor projectile: ") and " = " in l]


def card_agrees(table, rows):
    """Every row of the card worded as `table` words it."""
    want = shot_rows(table)
    names = {v for k, v in table.items() if k.startswith("dmg.")}
    return len(rows) == len(want) and all(
        g[0] == w[0] and (g[1] in names if w[1] is None else row_says(w[1], g[1]))
        for g, w in zip(rows, want))


# The keys the card's VALUES are worded by - the ones this batch moved out of
# English. A card check tells a language's wording from English's only for the
# keys that language words differently: de.lang's speed and range ("{} m/s",
# "{} m") are en.lang's own, so the German card alone cannot see those rows go
# back to English. Each key needs a checked language that words it otherwise.
CARD_VALUE_KEYS = ("map.proj.accuracy.value", "map.proj.speed.value", "map.proj.range.value",
                   "map.proj.payload.dot", "map.proj.payload.chance")


def editor_said(log):
    """The editor's message lines (`editor: ...` in the log), split on the
    script's '--- name ---' echoes like console_sections."""
    out, name = {}, None
    for line in log.splitlines():
        if "console: " in line:
            m = re.match(r"--- (.*) ---$", line.split("console: ", 1)[1])
            if m:
                name = m.group(1)
                out[name] = []
        elif name is not None and "] editor: " in line:
            out[name].append(line.split("] editor: ", 1)[1])
    return out


fresh()
settings_before = io.open(SETTINGS, "rb").read() if os.path.isfile(SETTINGS) else None
try:
    # Two skeletons on one square, authored (the scratch arena's .ent): the
    # editor places one monster a square, so this is the one way the script can
    # inspect a square whose count takes the plural key.
    raw_ent = io.open(ARENA + ".ent", "rb").read().decode("utf-8")
    eol = "\r\n" if "\r\n" in raw_ent else "\n"
    io.open(ARENA + ".ent", "wb").write(
        eol.join([l for l in raw_ent.split(eol) if l] + list(TWO_MONSTERS) + [""]).encode("utf-8"))
    log = run("celltext.eval")
    check(passed(log), "the script ran clean")
    said_in = editor_said(log)
    de, en, ru = lang_table("de"), lang_table("en"), lang_table("ru")
    want_de, want_en = cell_lines(de), cell_lines(en)
    # THE CONTROL: every WORD the lines are built from differs between the two
    # files, so a German match below is the language's, not English (or a key
    # left raw) that happens to agree - "Zelle" alone would tell the lines apart.
    words = [("map.select.wall",), ("map.select.floor",), ("map.select.monsters", 2),
             ("map.select.monsters.one",), ("map.select.props", 2), ("map.select.props.one",)]
    same = [w[0] for w in words if say(de, *w) == say(en, *w) or w[0] not in de or w[0] not in en]
    check(not same, "THE CONTROL: de.lang words every part of the line differently from en.lang "
          "(and both files have every key)", ", ".join(same))
    check(said_in.get("en") == want_en,
          "in English each square says what is on it: a wall, one monster, a monster and two props, "
          "one prop, two monsters, and a browsed level's base word", f"{said_in.get('en')} != {want_en}")
    check(said_in.get("de") == want_de,
          "under `lang de` every word is German - the base, the counts and both forms of each, on the "
          "active level and a browsed one (it used to read 'Zelle 23, 9: floor, 1 monster')",
          f"{said_in.get('de')} != {want_de}")
    # The dev readout is not the player's: under German it is the same English
    # line, which every phase that parses it relies on.
    sec = console_sections(log)
    cells = [m.groups() for m in map(CELL.match, ("console: " + l for l in sec.get("de", []))) if m]
    check([c[:4] for c in cells] == [("eval_arena", "0", "0", "solid"), ("eval_arena", "5", "5", "open")],
          "`editor cell` under `lang de` still prints its English dev readout (solid / open, wall= floor= "
          "ceiling=)", " | ".join(l for l in sec.get("de", []) if l.startswith("editor cell")))
    raw = [l for l in said_in.get("de", []) + said_in.get("en", []) if "map.select" in l or "map.joined" in l]
    check(not raw, "no raw key in any line", " | ".join(raw[:3]))

    # THE PROJECTILE CARD: one thrown torch, read in three languages.
    de_card, en_card = card(sec.get("de shot", [])), card(sec.get("en shot", []))
    ru_card = card(sec.get("ru shot", []))
    check("editor inspect: projectile" in sec.get("de shot", []) and len(de_card) == 7,
          "THE CONTROL: the thrown torch's card opened, its seven rows read",
          " | ".join(sec.get("de shot", [])[:3]))
    # THE CONTROL, per key: a card matches its language's wording AND not
    # English's only where that language words the key otherwise - so each
    # value key the card relies on needs a checked language that does (ru.lang
    # for speed and range, whose German is English's "m/s" and "m"), and both
    # files must have it.
    told = {k: [c for c, t in (("de", de), ("ru", ru)) if k in t and k in en and t[k] != en[k]]
            for k in CARD_VALUE_KEYS}
    check(all(told.values()),
          "THE CONTROL: every value key of the card is worded otherwise than en.lang by de.lang or "
          "ru.lang, so a row put back into English fails one of the matches below",
          ", ".join(f"{k}: {', '.join(v) or 'NONE'}" for k, v in told.items()))
    check(card_agrees(en, en_card), "in English the card reads its units and the burn as en.lang words them",
          str(en_card))
    check(card_agrees(de, de_card),
          "under `lang de` the card's units and its payload line are German too (they were 'pts', "
          "'m/s', 'm' and 'burn 1.5/s for 5.0s (25%)' in every language)", str(de_card))
    check(card_agrees(ru, ru_card),
          "under `lang ru` every row is Russian, the speed and range units too (Cyrillic m/s and m, "
          "where German's read like English's)", str(ru_card))
    # A key a language lacks shows raw on the card; say() already refuses to
    # match one, and this names it.
    raw_card = [r for r in de_card + en_card + ru_card
                if any("map.proj." in s or "map.joined" in s or s.startswith("dmg.") for s in r)]
    check(not raw_card, "no raw key on any card", str(raw_card[:3]))
finally:
    drop()
    # `lang` saved the language: the developer's settings.ini goes back.
    if settings_before is not None:
        io.open(SETTINGS, "wb").write(settings_before)
    elif os.path.isfile(SETTINGS):
        os.remove(SETTINGS)

# --- phase 27: "Use installed" never re-bakes a shared set -----------------------
print("27 - \"Use installed\" on a surface: another kind is refused, a baked set is not re-baked")
# Fixtures (installedsurface.eval's header says what each stands for). A second
# scratch world carries the other world's binding: the worn meshes are every
# world's, so a floor type in ANOTHER world is as much a reason as one here. Its
# type's id is long on purpose: that refusal is wider than the dialog's footer.
OTHER = "et_bind"
FAR_TYPE = "et_far_floor_named_at_length_to_need_a_second_line"
ET_WALLS = ("\r\n[et_unbaked_wall]\r\ndisplay = Unbaked\r\ntexture = et_unbaked\r\n"
            "\r\n[et_baked_wall]\r\ndisplay = Baked\r\ntexture = et_baked\r\n"
            "\r\n[et_stale]\r\ndisplay = Stale\r\n")
ET_IMPORTED = ("\r\n[et_imported_2k]\r\nkind = texture\r\nsource = C:\\nowhere\\et_imported\r\n"
               "surface = ceiling\r\n")
ET_FAR = f"\r\n[{FAR_TYPE}]\r\ndisplay = Far\r\ntexture = et_elsewhere\r\n"
PLAN = re.compile(r"newasset plan (\S+) '([^']*)': (.*)$")
MADE = re.compile(r"newasset (\S+) '([^']*)' from (\S+): (\w+)(?: - (.*))?$")
# The bake a create starts runs DETACHED (platform::Process neither waits for it
# nor ends it with the game), so the pool is read only once it is gone.
BAKER = os.path.join(os.path.dirname(EXE), "AssetBaker.exe")
TIERS = ("low", "med", "high")
# et_baked: a set an editor import left behind - worn meshes in the pool and no
# shipped record, so a bake as another kind would rewrite them (as a floor). A
# copy of cobblestone_wall's, made before the pool is fingerprinted.
BAKED = {f"worn_et_baked_{t}.gltf": f"worn_cobblestone_wall_{t}.gltf" for t in TIERS}
# What the one create that bakes (et_fresh, a set nobody paints) writes.
FRESH = {f"worn_et_nothing_here_{t}.gltf" for t in TIERS}


def worn_digests():
    """{file: sha1} of every worn mesh in the pool - tracked files, shared by
    every world, and what a wrong bake would rewrite."""
    out = {}
    for f in sorted(os.listdir(MODELS)):
        if f.startswith("worn_"):
            with open(os.path.join(MODELS, f), "rb") as fh:
                out[f] = hashlib.sha1(fh.read()).hexdigest()
    return out


def answers(lines, pattern):
    return [m.groups() for m in map(pattern.match, lines) if m]


def warned(key, tid, src, log):
    """Whether onCreate's own check refused this create. Only onCreate writes
    the line, so a refusal the FORM made (before Create) leaves none."""
    return f"create {key} '{tid}' from '{src}' refused:" in log


def audit(log, label):
    """(verdict, findings) of the `uioverlap <label>` run in this log."""
    head = f"uioverlap [{label}] ---"
    if head not in log:
        return None, []
    tail = log.split(head, 1)[1].splitlines()
    findings = [l for l in tail if l.startswith("[info ]   root")]
    verdict = next((l.split("uioverlap: ", 1)[1] for l in tail
                    if l.startswith("[info ] uioverlap: ")), None)
    return verdict, findings


for name, src in BAKED.items():
    shutil.copyfile(os.path.join(MODELS, src), os.path.join(MODELS, name))
worn_before = worn_digests()
fresh()
try:
    io.open(os.path.join(PROJ, r"catalog\walls.cat"), "a", encoding="utf-8", newline="").write(ET_WALLS)
    io.open(os.path.join(PROJ, r"catalog\imports.cat"), "a", encoding="utf-8", newline="").write(ET_IMPORTED)
    other = harness_game.scratch_world(ROOT, OTHER)
    io.open(os.path.join(other, r"catalog\floors.cat"), "a", encoding="utf-8", newline="").write(ET_FAR)
    # WINDOWED: the reason line is audited where it draws.
    log = run("installedsurface.eval", headless=False)
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    check("end" in sec, "the script ran to its end")

    # The rule's answer for each pick.
    plans = {(k, s): a for k, s, a in answers(sec.get("plan", []), PLAN)}
    for key, setname, want, label in (
            ("floors", "wall_brick", "'wall_brick' is a wall set (its own bake record)",
             "a shipped wall set as a floor: refused by its record"),
            ("floors", "et_unbaked", "'et_unbaked' is a wall set (type et_unbaked_wall in et_demo)",
             "a set a wall type of THIS world paints with, as a floor: refused, naming the type"),
            ("floors", "et_baked", "'et_baked' is a wall set (type et_baked_wall in et_demo)",
             "a set with worn meshes and no record that a wall type paints with, as a floor: refused"),
            ("walls", "et_elsewhere", f"'et_elsewhere' is a floor set (type {FAR_TYPE} in et_bind)",
             "a set only ANOTHER world's floor type paints with, as a wall: refused, naming that world"),
            ("walls", "et_imported", "'et_imported' is a ceiling set (imported as one into et_demo)",
             "a set an import baked as a ceiling, as a wall: refused, naming the import")):
        got = plans.get((key, setname), "")
        check(got.startswith("refused - " + want), label, f"plan: {got!r}")
    check(plans.get(("walls", "cobblestone_wall")) == "use as it is",
          "a baked wall set as a second wall type: used as it is, no bake",
          f"plan: {plans.get(('walls', 'cobblestone_wall'))!r}")
    for setname in ("et_late", "et_nothing_here"):
        check(plans.get(("floors", setname)) == "bake",
              f"{setname}, painted by nobody and with no worn meshes, is one a create would bake",
              f"plan: {plans.get(('floors', setname))!r}")

    # Through the dialog, keyed by the new type's id: (catalog, source, outcome, why).
    made = {}
    for name in ("floor of a wall set", "floor of this world's wall", "floor of a baked wall set",
                 "wall of another world's floor", "picked, then painted elsewhere", "second wall",
                 "fresh set"):
        for key, tid, src, outcome, why in answers(sec.get(name, []), MADE):
            made.setdefault(tid, []).append((key, src, outcome, why or ""))
    # The FORM refuses these, judged at the pick: Create never reaches onCreate,
    # whose own check would have logged its refusal.
    for tid, want in (("et_brick_floor", "'wall_brick' is a wall set"),
                      ("et_unbaked_floor", "'et_unbaked' is a wall set"),
                      ("et_baked_floor", "'et_baked' is a wall set"),
                      ("et_elsewhere_wall", "'et_elsewhere' is a floor set")):
        key, src, outcome, why = (made.get(tid) or [("", "", "", "")])[-1]
        oncreate = warned(key, tid, src, log)
        check(outcome == "refused" and why.startswith(want) and not oncreate,
              f"the form refuses {tid} before Create, saying why (onCreate never asked)",
              f"{outcome} - {why} | onCreate refused it: {oncreate}")

    # The reason line: the longest refusal is wider than the footer, and the
    # audit, run while it showed, finds it wrapped inside its own area.
    far = [l for l in sec.get("wall of another world's floor", [])
           if l.startswith("newasset: open - 'et_elsewhere'")]
    verdict, findings = audit(log, "newasset_refused")
    check(bool(far) and len(far[0]) > 150 and verdict is not None and verdict.startswith("clean"),
          "the longest refusal (150+ characters) stays inside the footer, left of Create (uioverlap)",
          f"audit: {verdict!r} {findings[:3]} | shown: {far[:1]}")

    # A STALE pick: judged fine when picked, then a wall type is renamed onto the
    # set. Create reaches onCreate, whose own check refuses it - and the form
    # stays open, saying why.
    stale = made.get("et_late_floor", [])
    picked = [m for m in stale if m[2] == "picked"]
    created = [m for m in stale if m[2] != "picked"]
    check(bool(picked) and picked[0][3] == "ready",
          "et_late picked as a floor: the form has nothing against it", str(picked))
    check(any(l == "typeset rename walls 'et_stale': done"
              for l in sec.get("picked, then painted elsewhere", [])),
          "...then wall type et_stale is renamed et_late, painting that set")
    check(bool(created) and created[0][2] == "refused"
          and created[0][3].startswith("'et_late' is a wall set (type et_late in et_demo)")
          and warned("floors", "et_late_floor", "et_late", log),
          "...and Create is refused by onCreate's own check, saying why", str(created))
    check(any(l.startswith("newasset: open - 'et_late' is a wall set")
              for l in sec.get("picked, then painted elsewhere", [])),
          "...and the form stays open showing it (it used to close over its own message)")

    check((made.get("et_cobble2") or [("", "", "", "")])[-1][2] == "created",
          "a second wall type off a baked wall set is created by the dialog's Create",
          str(made.get("et_cobble2")))
    check(any(l == "newasset: closed" for l in sec.get("second wall", [])),
          "...and the dialog closed, done")
    check("Created type 'et_cobble2' in walls" in log and "Added 'et_cobble2' to the level palette" in log,
          "...and the type is in walls.cat and on the level's palette")
    # The positive control, THROUGH the dialog: a set nobody paints and with no
    # meshes is baked, as the new kind - and it is the only create that bakes.
    check((made.get("et_fresh") or [("", "", "", "")])[-1][2] == "baking",
          "a floor off a set nobody paints, with no worn meshes, bakes them",
          str(made.get("et_fresh")))
    bakes = [l for l in log.splitlines() if "AssetBaker: " in l]
    check(len(bakes) == 1 and "wornblock floor et_nothing_here " in bakes[0],
          "...as a floor, and that is the only AssetBaker any create started",
          " | ".join(b[:200] for b in bakes[:3]))

    walls = io.open(os.path.join(PROJ, r"catalog\walls.cat"), encoding="utf-8").read()
    floors = io.open(os.path.join(PROJ, r"catalog\floors.cat"), encoding="utf-8").read()
    cobble = cat_block(walls, "et_cobble2") or {}
    check(cobble.get("texture") == "cobblestone_wall" and cat_block(walls, "et_elsewhere_wall") is None
          and all(cat_block(floors, t) is None
                  for t in ("et_brick_floor", "et_unbaked_floor", "et_baked_floor", "et_late_floor")),
          "on disk: et_cobble2 binds cobblestone_wall, and no refused type was written", str(cobble))
finally:
    drop()
    harness_game.remove_world(ROOT, OTHER)
# The bake outlives the game: wait for it, so what it writes is counted.
deadline = time.time() + 180
while harness_game.running_copies(BAKER) and time.time() < deadline:
    time.sleep(0.5)
check(not harness_game.running_copies(BAKER), "the bake a create started has finished")
worn_after = worn_digests()
changed = sorted(f for f in worn_before if worn_after.get(f) != worn_before[f])
added = sorted(set(worn_after) - set(worn_before) - FRESH)
check(len(worn_before) > 100 and not changed and not added,
      f"every worn mesh in the pool is byte for byte as it was ({len(worn_before)} files, et_baked's "
      "copies among them), and none was added but the fresh set's",
      f"changed: {changed[:6]} added: {added[:6]}")
check(FRESH <= set(worn_after), "...whose three tiers the bake did write",
      str(sorted(FRESH - set(worn_after))))

# --- phase 28: `editor place` takes the viewed level's free face ------------------
print("28 - `editor place` hangs a wall kind on the viewed level's first free face")
PLACED = re.compile(r"editor place: (\S+) at (\d+),(\d+)(?: on (\w+))?$")


def placed(lines):
    """(id, x, z, face) of each `editor place:` answer - face None if it took none."""
    return [(m.group(1), int(m.group(2)), int(m.group(3)), m.group(4))
            for m in map(PLACED.match, lines) if m]


def wall_records(stem, kind):
    """The (type, x, z, facing) of each `fixture` or `niche` record of a saved
    level that names a facing, as a sorted list (a repeat would show twice)."""
    text = io.open(os.path.join(PROJ, "levels", stem + ".map"), encoding="utf-8").read()
    return sorted((m.group(1), int(m.group(2)), int(m.group(3)), m.group(4))
                  for m in re.finditer(rf"^{kind} (\S+) (\d+) (\d+) (north|east|south|west)\b", text, re.M))


fresh()
try:
    before = {(stem, kind): wall_records(stem, kind)
              for stem in ("crypt1", "eval_arena") for kind in ("fixture", "niche")}
    # THE CONTROL: the squares are what the script takes them for - crypt1's
    # own sconces on 5,1 and 4,3 hold their north faces, eval_arena has none.
    check({("sconce", 5, 1, "north"), ("sconce", 4, 3, "north")} <= set(before[("crypt1", "fixture")])
          and not before[("eval_arena", "fixture")],
          "THE CONTROL: crypt1's sconces hold 5,1 north and 4,3 north; eval_arena has no fixture",
          str(before))
    log = run("editorplace.eval")
    check(passed(log), "the script ran clean (its two refusals expected, nothing else refused)")
    sec = console_sections(log)
    check("end" in sec, "the script ran to its end")
    for name, want, label in (
            ("active", [("sconce", 1, 1, "north"), ("sconce", 1, 1, "west")],
             "on the active level a sconce named north, then one with no face: north is taken, so west"),
            ("taken", [("sconce", 5, 1, "south")],
             "on crypt1, BROWSED, a sconce at 5,1 passes north (crypt1's own sconce) for south - "
             "where the old pick took eval_arena's north rock, the very face that is taken"),
            ("plain", [("sconce", 2, 7, "north")],
             "on crypt1 a sconce at 2,7 takes north, where eval_arena's square has no wall to pick"),
            ("niches", [("niche", 12, 5, "east"), ("niche", 12, 5, "west")],
             "a niche at 12,5 takes east, a second west, and a third is refused"),
            ("full", [], "a sconce at 4,3, whose one solid face holds a sconce, is refused"),
            ("erased", [("niche", 12, 5, "east"), ("sconce", 5, 1, "north")],
             "after `editor erase 13 5` a niche at 12,5 takes east again, and after "
             "`editor erase 5 1` (crypt1's own sconce) a sconce there takes north - the "
             "erase rebuilt the browsed snapshot the default face reads")):
        check(placed(sec.get(name, [])) == want, label, str(sec.get(name, [])))
    erased = [ln for ln in sec.get("erased", []) if ln.startswith("editor erase: ")]
    check(erased == ["editor erase: 13,5", "editor erase: 5,1"],
          "both erases answered as having erased something", str(sec.get("erased", [])))
    after = {(stem, kind): wall_records(stem, kind)
             for stem in ("crypt1", "eval_arena") for kind in ("fixture", "niche")}
    for (stem, kind), add in ((("crypt1", "fixture"), [("sconce", 5, 1, "south"), ("sconce", 2, 7, "north")]),
                              (("crypt1", "niche"), [("niche", 12, 5, "east"), ("niche", 12, 5, "west")]),
                              (("eval_arena", "fixture"), [("sconce", 1, 1, "north"), ("sconce", 1, 1, "west")]),
                              (("eval_arena", "niche"), [])):
        check(after[(stem, kind)] == sorted(before[(stem, kind)] + add),
              f"saved, {stem}'s {kind} records gain exactly the placements above",
              f"before {before[(stem, kind)]} after {after[(stem, kind)]}")
finally:
    drop()


# --- phase 30: a world switch carries no question and runs under no bake --------
print("30 - a world switch: no question carries over, and none happens under a bake")
# worldswitch.eval's header says what each step stands for. The second world is a
# scratch copy too, so the switch opens nothing real; the sets are et_-named, so
# cleanup() takes what their bakes write into the pool (the import's maps too).
OTHER_WORLD = "et_other"
SWITCH_SET = "et_switch_bake"   # a floor set nobody paints, with no worn meshes
SWITCH_TYPE = "et_switchbake"
IMPORT_TYPE = "et_switchimp"    # imported from IMPORT_DIR: its set is <type>_2k
IMPORT_DIR = os.path.join(IMPORT_SRC, IMPORT_TYPE)
# The folder as the script names it: one word with FORWARD slashes, which Windows
# reads as well, and which eval_script's word swap (a regex replacement, where a
# backslash is an escape) puts in as written. It is what imports.cat records.
IMPORT_WORD = IMPORT_DIR.replace("\\", "/")
DOWNED = re.compile(r"confirm: '([^'\r\n]*)' taken down unanswered - ([^\r\n]*)")


def write_png(path, size):
    """A small RGB PNG, a two-tone check - all an import needs is an albedo map,
    and a small one keeps the debug baker's BC7 pass to a moment."""
    rows = b"".join(b"\x00" + bytes(c for x in range(size)
                                     for c in ((150, 120, 90) if (x // 8 + y // 8) % 2 else (90, 80, 70)))
                    for y in range(size))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def world_files(world):
    """{path under the world: bytes} - its manifest and every catalog file."""
    folder = os.path.join(PROJECTS, world)
    out = {"project.ini": open(os.path.join(folder, "project.ini"), "rb").read()}
    for f in sorted(os.listdir(os.path.join(folder, "catalog"))):
        out["catalog\\" + f] = open(os.path.join(folder, "catalog", f), "rb").read()
    return out


def cat_blocks(data):
    """{id: fields} of every [id] block in a .cat file's bytes."""
    text = data.decode("utf-8")
    return {i: cat_block(text, i) for i in block_ids(text)}


fresh()
try:
    harness_game.scratch_world(ROOT, OTHER_WORLD)
    # The import's source: a folder of one albedo map, as a download would be.
    shutil.rmtree(IMPORT_SRC, ignore_errors=True)
    os.makedirs(IMPORT_DIR)
    write_png(os.path.join(IMPORT_DIR, IMPORT_TYPE + "_albedo.png"), 64)
    demo_before, other_before = world_files(SCRATCH), world_files(OTHER_WORLD)
    log = run("worldswitch.eval", words={"ET_IMPORT_DIR": IMPORT_WORD})
    check(passed(log), "the script ran clean (each of its four probes refused)")
    sec = console_sections(log)
    check("end" in sec, "the script ran to its end")

    # 1. A question does not cross a world switch (C115).
    asked = [l for l in sec.get("asked", []) if l.startswith("confirm: open - '")]
    check(bool(asked), "the exit's question is up before the switch", str(sec.get("asked", [])[:4]))
    title = asked[0][len("confirm: open - '"):-1] if asked else None
    downs = DOWNED.findall(log)
    check(downs == [(title, "the world was unloaded")],
          "...and taken down unanswered by the UNLOAD - not left for the next world's new game, "
          "which a switch to a world that fails to load never reaches", str(downs))
    switched = sec.get("switched", [])
    check(any(l.startswith("world et_other ") for l in switched) and "confirm: none" in switched,
          "in et_other no question is up", str(switched[:6]))
    check("refused, as expected: 'confirm yes'" in log, "...and a Yes is refused: nothing is up to answer")

    # 2. No switch under a bake (C234).
    bake = sec.get("bake", [])
    check(any(l.startswith(f"newasset floors '{SWITCH_TYPE}' from {SWITCH_SET}: baking") for l in bake),
          "\"Use installed\" on a floor set with no worn meshes starts a bake", str(bake[:3]))
    check(any(l.startswith(f"bake: running - floors '{SWITCH_TYPE}'") for l in bake),
          "...which `bake` sees running")
    check(any(l.startswith(f"worlds load: an asset bake is running ('{SWITCH_TYPE}')") for l in bake)
          and any(l.startswith("world et_demo ") for l in bake),
          "a world switch asked meanwhile is refused, naming the bake, and et_demo stays loaded",
          str(bake[:8]))
    check(any(l.startswith(f"bake: the baker for '{SWITCH_TYPE}' exited (code 0)") for l in bake),
          "the baker exits cleanly", " | ".join(l for l in bake if l.startswith("bake")))
    landed = sec.get("landed", [])
    check("bake: idle" in landed and "newasset: closed" in landed
          and any(l.startswith("world et_demo ") for l in landed),
          "it landed: no bake running, the dialog closed, et_demo still the world", str(landed))
    check(f"Created type '{SWITCH_TYPE}' in floors" in log, "...and wrote its type, into the world loaded")

    # 3. ...nor under an import, in either of its runs (C234's imports.cat half).
    imp, step1 = sec.get("import", []), sec.get("import step 1", [])
    check(any(l.startswith(f"newasset floors '{IMPORT_TYPE}' from {IMPORT_WORD}: baking") for l in imp),
          "Import, Browse handed a folder of one albedo map, starts a bake", str(imp[:5]))
    for lines, step, what in ((imp, 0, "its maps"), (step1, 1, "its worn meshes")):
        check(any(l.startswith(f"bake: running - floors '{IMPORT_TYPE}', step {step}") for l in lines)
              and any(l.startswith(f"worlds load: an asset bake is running ('{IMPORT_TYPE}')") for l in lines)
              and any(l.startswith("world et_demo ") for l in lines),
              f"while step {step} ({what}) runs, a world switch is refused naming the import, "
              "and et_demo stays loaded", str(lines[:8]))
    exits = [(l, s) for s, lines in ((0, imp), (1, step1)) for l in lines
             if l.startswith(f"bake: the baker for '{IMPORT_TYPE}' exited (code 0)")]
    check(len(exits) == 2 and "its second step starts" in exits[0][0] and exits[0][1] == 0
          and "it lands" in exits[1][0] and exits[1][1] == 1,
          "both runs exit cleanly, the first handing on to the second", str(exits))
    check(log.count("refused, as expected: 'worlds load et_other'") == 3,
          "...three switches refused in all: under the install's bake and each of the import's runs")
    imported = sec.get("imported", [])
    check("bake: idle" in imported and "newasset: closed" in imported
          and any(l.startswith("world et_demo ") for l in imported),
          "the import landed: no bake running, the dialog closed, et_demo still the world", str(imported))
    check(f"Created type '{IMPORT_TYPE}' in floors" in log, "...and wrote its type, into the world loaded")
    bakes = [l for l in log.splitlines() if "AssetBaker: " in l]
    # The import's maps say their green flip either way (code-review C393): a
    # folder with no normal map is sent --no-flip-green.
    check(len(bakes) == 3 and f"wornblock floor {SWITCH_SET} " in bakes[0]
          and f" import \"{IMPORT_WORD}\" " in bakes[1]
          and bakes[1].rstrip().endswith(f" {IMPORT_TYPE}_2k --no-flip-green")
          and f"wornblock floor {IMPORT_TYPE} " in bakes[2],
          "the AssetBaker runs were those three: the install's floor bake, the import's maps under "
          f"{IMPORT_TYPE}_2k (sent --no-flip-green), then its floor bake",
          " | ".join(b[:200] for b in bakes[:4]))

    # ...read off the disk: the other world untouched, this one by the two types
    # and the import's record.
    demo_after, other_after = world_files(SCRATCH), world_files(OTHER_WORLD)
    moved = sorted(f for f in set(other_before) | set(other_after) if other_before.get(f) != other_after.get(f))
    check("catalog\\imports.cat" in other_after and not moved,
          "et_other's manifest and catalogs, imports.cat among them, are byte for byte as they were",
          str(moved))
    moved = sorted(f for f in set(demo_before) | set(demo_after) if demo_before.get(f) != demo_after.get(f))
    check(moved == ["catalog\\floors.cat", "catalog\\imports.cat"],
          "et_demo's: floors.cat and imports.cat changed and no other file", str(moved))
    before = cat_blocks(demo_before["catalog\\floors.cat"])
    after = cat_blocks(demo_after.get("catalog\\floors.cat", b""))
    made, made_imp = after.get(SWITCH_TYPE) or {}, after.get(IMPORT_TYPE) or {}
    check(set(after) - set(before) == {SWITCH_TYPE, IMPORT_TYPE}
          and all(after.get(i) == f for i, f in before.items())
          and made.get("texture") == SWITCH_SET and made_imp.get("texture") == IMPORT_TYPE,
          f"...floors.cat by exactly [{SWITCH_TYPE}] (texture = {SWITCH_SET}) and [{IMPORT_TYPE}] "
          f"(texture = {IMPORT_TYPE}), every other floor as it was", f"{made} {made_imp}")
    ib = cat_blocks(demo_before["catalog\\imports.cat"])
    ia = cat_blocks(demo_after.get("catalog\\imports.cat", b""))
    rec = ia.get(IMPORT_TYPE + "_2k") or {}
    check(set(ia) - set(ib) == {IMPORT_TYPE + "_2k"} and all(ia.get(i) == f for i, f in ib.items())
          and rec.get("kind") == "texture" and rec.get("source") == IMPORT_WORD
          and rec.get("surface") == "floor" and rec.get("flip_green") == "0",
          f"...imports.cat by exactly the import's record ([{IMPORT_TYPE}_2k]: a texture, its folder, "
          "baked as a floor, no green flip), every other as it was - the installed set's type records "
          "none", str(rec))
finally:
    drop()
    harness_game.remove_world(ROOT, OTHER_WORLD)


# --- phase 31: the stash rules ------------------------------------------------------
print("31 - a level visited or read is not stashed; unsaved work survives a load elsewhere")

STASH_LISTS = re.compile(r"stashes: maps=(\S+) ents=(\S+) states=(\S+)$")
STASH_ACTIVE = re.compile(r"stashes: active (\S+) map=(\w+) ents=(\w+)( parked)?$")
MAPINFO = re.compile(r"\d+x\d+ map, start \d+,\d+, (\d+) walkable")


def stash_reads(lines):
    """Every `stashes` answer in a section, in order, as a dict: the three lists
    as sets of stems, then the active level, its map's and records' word and
    whether it is parked."""
    out, lists = [], None
    for line in lines:
        m = STASH_LISTS.match(line)
        if m:
            lists = [set() if g == "none" else set(g.split(",")) for g in m.groups()]
            continue
        m = STASH_ACTIVE.match(line)
        if m and lists is not None:
            out.append({"maps": lists[0], "ents": lists[1], "states": lists[2],
                        "active": m.group(1), "map": m.group(2), "rec": m.group(3),
                        "parked": bool(m.group(4))})
            lists = None
    return out


def first_read(sec, name):
    reads = stash_reads(sec.get(name, []))
    return reads[0] if reads else {}


def saved_lists(log):
    """Each savemap's written stems, in order."""
    return [[s.strip() for s in m.group(1).split(",")]
            for m in re.finditer(r"console: saved levels: (.*)", log)]


def walkable_in(lines):
    got = [int(m.group(1)) for m in (MAPINFO.match(l) for l in lines) if m]
    return got[0] if got else None


def grid_cell(files, stem, x, z):
    """The glyph a level file's grid has at (x, z), or None."""
    text = files.get(stem + ".map", b"").decode("utf-8")
    rows = [l for l in text.splitlines() if l and l[0] in "#.PDTF"]
    return rows[z][x] if z < len(rows) and x < len(rows[z]) else None


def ent_records(files, stem):
    """The set of (kind, type, x, z) records of a level's .ent bytes."""
    text = files.get(stem + ".ent", b"").decode("utf-8")
    return {tuple(l.split()[:4]) for l in text.splitlines()
            if l and not l.startswith(";") and len(l.split()) >= 4}


def moved(before, after, names):
    """Which of `names` are not byte for byte as they were."""
    return [n for n in names if before.get(n) != after.get(n)]


# 1. THE VISIT, THE QUERIES AND THE RENAME (C308, C307).
fresh()
try:
    before = level_files(SCRATCH)
    log = run("stashvisit.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    check("end" in sec, "the script ran to its end")
    start, there, back = (first_read(sec, n) for n in ("start", "on crypt2", "back"))
    check(start.get("active") == "eval_arena" and not start.get("maps", {1})
          and not start.get("ents", {1}) and start.get("map") == "clean",
          "after the reset: on eval_arena, nothing stashed, its map as filed", str(start))
    # THE CONTROL: the walk really happened - each readout stands where it says,
    # with the dynamic state of the levels just left held, as a stair holds it.
    check(there.get("active") == "crypt2" and {"crypt1", "eval_arena"} <= there.get("states", set())
          and back.get("active") == "crypt1" and {"crypt2", "eval_arena"} <= back.get("states", set()),
          "the walk happened: on crypt2, then crypt1, each level left holding its dynamic state",
          f"{there} | {back}")
    check(not there.get("maps", {1}) and not there.get("ents", {1})
          and not back.get("maps", {1}) and not back.get("ents", {1}),
          "...and no map or .ent was stashed by it (a level only visited is not an edit)",
          f"{there} | {back}")
    check(there.get("map") == "clean" and back.get("map") == "clean",
          "each level stood on reads clean against its file", f"{there} | {back}")
    saves = saved_lists(log)
    check(len(saves) == 2 and saves[0] == ["crypt1"],
          "savemap after the walk writes crypt1, the level stood on, alone", str(saves[:1]))

    queries, after_q = stash_reads(sec.get("queries", [])), first_read(sec, "after queries")
    q_lines = sec.get("queries", [])
    check("delete 'dungeon1': allowed" in q_lines,
          "`dungeons what dungeon1` got as far as the stair walk (its last rule) and allowed it",
          str([l for l in q_lines if l.startswith("delete")]))
    check(any(l.startswith("dungeons dialog: open 'dungeon1' confirming") for l in q_lines)
          and any(l.startswith("dungeons dialog: closed") for l in q_lines),
          "the type editor's Delete asked the same (it went on to confirm), and was cancelled",
          str([l for l in q_lines if l.startswith("dungeons dialog")]))
    check(bool(queries) and not queries[0]["maps"] and not queries[0]["ents"]
          and not after_q.get("maps", {1}) and not after_q.get("ents", {1}),
          "...and neither the question nor the cancelled delete stashed a level",
          f"{queries[:1]} | {after_q}")

    renamed, back_again = first_read(sec, "rename"), first_read(sec, "renamed back")
    check("renamed level 'crypt2' -> 'crypt2b'" in sec.get("rename", [])
          and "renamed level 'crypt2b' -> 'crypt2'" in sec.get("rename", []),
          "crypt2 was renamed, and back", str([l for l in log.splitlines() if "renamed level" in l]))
    check(log.count("rename level: repointed stairs written in crypt1") == 2,
          "each rename repointed crypt1's stair and wrote crypt1 at once - the level that changed",
          str(log.count("rename level: repointed stairs written in")))
    check("crypt2b" in renamed.get("states", set()) and not renamed.get("maps", {1})
          and not renamed.get("ents", {1}) and not back_again.get("maps", {1})
          and not back_again.get("ents", {1}),
          "...and no other level was stashed to look for a stair (the states follow the name)",
          f"{renamed} | {back_again}")
    check(len(saves) == 2 and saves[1] == ["crypt1"],
          "the last savemap still writes crypt1 alone", str(saves))

    after = level_files(SCRATCH)
    untouched = ["crypt2.map", "crypt2.ent", "eval_arena.map", "eval_arena.ent"]
    check(not moved(before, after, untouched),
          "crypt2's and eval_arena's files are byte for byte as the copy was made",
          str(moved(before, after, untouched)))
    check(b"stairs stairs_down 1 1 south dest=crypt2 " in after.get("crypt1.map", b"")
          and not any(n.startswith("crypt2b") for n in after),
          "crypt1's stair names crypt2 again, and no crypt2b file is left")
finally:
    drop()

# 2. UNSAVED WORK SURVIVES A SAVE LOADED ON ANOTHER LEVEL (C298).
fresh()
try:
    before = level_files(SCRATCH)
    log = run("stashload.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    check("end" in sec, "the script ran to its end")
    w0, w1, w2 = (walkable_in(sec.get(n, [])) for n in ("before", "painted", "returned"))
    pre, painted, undone, repainted = (first_read(sec, n)
                                       for n in ("before", "painted", "undone", "repainted"))
    check(pre.get("active") == "crypt1" and pre.get("map") == "clean",
          "on crypt1 before the paint, its map as filed", str(pre))
    check(w0 is not None and w1 == w0 - 1 and painted.get("map") == "edited",
          "the wall at 9,1 took a floor square, and the map reads edited", f"{w0} -> {w1}, {painted}")
    check(undone.get("map") == "clean" and repainted.get("map") == "edited",
          "THE MEASURE: clean again after the undo, edited again after the repaint",
          f"{undone} | {repainted}")
    loaded = first_read(sec, "loaded")
    check(any(l.startswith("loaded: " + SAVES["stashtest"]) for l in sec.get("repainted", []) +
              sec.get("loaded", [])) and loaded.get("active") == "eval_arena",
          "the save made on eval_arena loaded there", str(loaded))
    check(loaded.get("maps") == {"crypt1"},
          "...and crypt1's map was stashed on the way out, though the load drops its dynamic state",
          str(loaded))
    saves = saved_lists(log)
    check(saves[:1] == [["eval_arena", "crypt1"]],
          "savemap writes eval_arena and crypt1, from the stash", str(saves))
    back_on = first_read(sec, "returned")
    check(back_on.get("active") == "crypt1" and w2 == w1,
          "going back to crypt1 finds the wall", f"{w2} walkable, painted {w1}")
    after = level_files(SCRATCH)
    check(grid_cell(before, "crypt1", 9, 1) == "." and grid_cell(after, "crypt1", 9, 1) == "#",
          "crypt1.map on disk carries the wall at 9,1",
          f"{grid_cell(before, 'crypt1', 9, 1)} -> {grid_cell(after, 'crypt1', 9, 1)}")
finally:
    drop()

# 3. A BROWSED LEVEL STASHES ONLY THE LAYER AN EDIT CHANGES (C307).
fresh()
try:
    before = level_files(SCRATCH)
    log = run("stashremote.eval")
    check(passed(log), "the script ran clean (the erase of nothing refused)")
    sec = console_sections(log)
    check("end" in sec, "the script ran to its end")
    empty, painted, item = (first_read(sec, n) for n in ("empty erase", "painted", "item erase"))
    # FIRST, on a level holding no stash: a later edit's stash cannot stand in
    # for one the erase made, so each map rung (the props, the looks) is judged
    # as well as the records one - after the paint, the map half could not fail.
    check("refused, as expected: 'editor erase 4 1'" in log and empty.get("active") == "eval_arena"
          and empty.get("maps") == set() and empty.get("ents") == set(),
          "an erase of nothing on crypt2, which held no stash, is refused and stashes neither "
          "layer (every rung tried on a copy)", str(empty))
    check(painted.get("active") == "eval_arena" and painted.get("maps") == {"crypt2"}
          and painted.get("ents") == set(),
          "a wall painted on crypt2 stashes its map and not its .ent", str(painted))
    check(any(l.startswith("editor erase: 12,1") for l in sec.get("painted", []) +
              sec.get("item erase", [])) and item.get("maps") == {"crypt2"}
          and item.get("ents") == {"crypt1"},
          "an item erased on crypt1 stashes its .ent and not its map", str(item))
    saves = saved_lists(log)
    check(saves == [["eval_arena", "crypt1", "crypt2"]],
          "savemap writes eval_arena (stood on), crypt1 and crypt2", str(saves))
    after = level_files(SCRATCH)
    check(not moved(before, after, ["crypt2.ent", "crypt1.map"]),
          "crypt2.ent and crypt1.map are byte for byte as the copy was made",
          str(moved(before, after, ["crypt2.ent", "crypt1.map"])))
    check(grid_cell(before, "crypt2", 3, 1) == "." and grid_cell(after, "crypt2", 3, 1) == "#",
          "crypt2.map carries the wall at 3,1")
    gone = ent_records(before, "crypt1") - ent_records(after, "crypt1")
    check(gone == {("item", "potion_health_minor", "12", "1")}
          and ent_records(after, "crypt1") <= ent_records(before, "crypt1"),
          "crypt1.ent lost the potion at 12,1 and nothing else", str(gone))
finally:
    drop()

# 4. A PARKED LEVEL LIVE AGAIN IN PLACE KEEPS NO STASH OF ITSELF; A GAME BEGUN
#    ELSEWHERE KEEPS THE EDIT IT LEFT (C298's second trigger).
fresh()
try:
    before = level_files(SCRATCH)
    log = run("stashpark.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    check("end" in sec, "the script ran to its end")
    w0, w1, w_put, w_back, w_again, w_ret = (
        walkable_in(sec.get(n, [])) for n in ("before", "painted", "put back", "back",
                                               "painted again", "returned"))
    pre, painted, parked, loaded, put, left, back, again, newgame, ret = (
        first_read(sec, n) for n in ("before", "painted", "parked", "loaded", "put back", "left",
                                     "back", "painted again", "new game", "returned"))
    check(pre.get("active") == "crypt1" and pre.get("map") == "clean" and not pre.get("maps", {1}),
          "on crypt1 before the paint, its map as filed and nothing stashed", str(pre))
    check(w0 is not None and w1 == w0 - 1 and painted.get("map") == "edited",
          "the wall at 9,1 took a floor square, and the map reads edited", f"{w0} -> {w1}, {painted}")
    # THE CONTROL: the park really happened, and stashed what it should.
    check(parked.get("active") == "crypt1" and parked.get("parked") is True
          and parked.get("maps") == {"crypt1"},
          "walking out to the world parked crypt1 and stashed its edited map", str(parked))
    check(any(l.startswith("loaded: " + SAVES["stashpark"]) for l in sec.get("parked", []) +
              sec.get("loaded", [])) and loaded.get("active") == "crypt1"
          and loaded.get("parked") is False,
          "the save made on crypt1 loaded there, in place, and crypt1 is no longer parked",
          str(loaded))
    check(not loaded.get("maps", {1}) and not loaded.get("ents", {1}),
          "...and nothing of crypt1 is left stashed: the live level is the level again",
          str(loaded))
    check(put.get("map") == "clean" and w_put == w0,
          "THE PREMISE: the square put back as filed - the map reads clean, the floor is back",
          f"{w_put} walkable (filed {w0}), {put}")
    saves = saved_lists(log)
    check(saves[:1] == [["crypt1"]],
          "a savemap during the stay writes crypt1 once (the park's copy no longer goes "
          "over it)", str(saves[:1]))
    check(left.get("active") == "crypt2" and not left.get("maps", {1})
          and not left.get("ents", {1}),
          "leaving crypt1 clean by the stairs stashes nothing of it", str(left))
    check(back.get("active") == "crypt1" and w_back == w0 and back.get("map") == "clean",
          "going back to crypt1 finds the floor, not the wall the park stashed",
          f"{w_back} walkable (filed {w0}), {back}")
    check(w_again == w0 - 1 and again.get("map") == "edited",
          "a wall painted at 10,1, and the map reads edited", f"{w_again}, {again}")
    check(newgame.get("active") == "eval_arena" and newgame.get("maps") == {"crypt1"}
          and not newgame.get("ents", {1}),
          "a new game begun on eval_arena stashed crypt1's map on the way out, not its .ent",
          str(newgame))
    check(saves[1:2] == [["eval_arena", "crypt1"]],
          "savemap writes eval_arena and crypt1, from the stash", str(saves))
    check(ret.get("active") == "crypt1" and w_ret == w0 - 1,
          "going back to crypt1 finds the wall at 10,1", f"{w_ret} walkable, painted {w_again}")
    after = level_files(SCRATCH)
    check(grid_cell(after, "crypt1", 9, 1) == "." and grid_cell(before, "crypt1", 10, 1) == "."
          and grid_cell(after, "crypt1", 10, 1) == "#",
          "crypt1.map on disk: a floor at 9,1 as filed, the wall at 10,1",
          f"9,1 {grid_cell(after, 'crypt1', 9, 1)}, 10,1 {grid_cell(before, 'crypt1', 10, 1)} -> "
          f"{grid_cell(after, 'crypt1', 10, 1)}")
    check(not moved(before, after, ["crypt2.map", "crypt2.ent"]),
          "crypt2's files, a level only visited, are byte for byte as the copy was made",
          str(moved(before, after, ["crypt2.map", "crypt2.ent"])))
finally:
    drop()


# --- phase 32: the records tell the truth -------------------------------------------
print("32 - spawns saved as filed, ids never reused, no respawn or undo loses work, "
      "no editor removal trips the ledger")

MONLINE = re.compile(r"^\s+(\S+) @ (-?\d+),(-?\d+)\s+hp (-?[\d.]+)(.*?)\s+aware=(\d)$")
BRKLINE = re.compile(r"^\s+(door|decoration|fixture) (\S+) @ (-?\d+),(-?\d+) hp=([\d.]+)/")
PIPE = re.compile(r"PIPELINE RESULT=(\w+) checks=(\d+) violations=(\d+)")


def mons(lines):
    """The `monsters` rows of a section, in list order: dicts of type, x, z, hp,
    dead and aware."""
    out = []
    for l in lines:
        m = MONLINE.match(l)
        if m:
            out.append({"type": m.group(1), "x": int(m.group(2)), "z": int(m.group(3)),
                        "hp": float(m.group(4)), "dead": "(dead)" in m.group(5),
                        "aware": m.group(6) == "1"})
    return out


def mon_at(rows, type_, x, z):
    hits = [r for r in rows if r["type"] == type_ and r["x"] == x and r["z"] == z]
    return hits[0] if hits else None


def brks(lines):
    """The `breakables` rows of a section, in list order: (kind, type, x, z, hp)."""
    return [(m.group(1), m.group(2), int(m.group(3)), int(m.group(4)), float(m.group(5)))
            for m in (BRKLINE.match(l) for l in lines) if m]


def monster_lines(data):
    """The `monster ...` lines of a level's .ent bytes, in file order."""
    return [l.rstrip() for l in data.decode("utf-8").splitlines() if l.startswith("monster ")]


def doors_said(lines):
    return [l for l in lines if re.match(r"door \d+,\d+ -> (open|shut)$", l)]


def archetypes(lines):
    return re.findall(r"^editor inspector: monster \d+ (\S+) live .* archetype (\S+) ", "\n".join(lines),
                      re.M)


# 1. THE SPAWNS (C326).
fresh()
try:
    before = level_files(SCRATCH)
    log = run("recordspawn.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    check("end" in sec, "the script ran to its end")
    filed = monster_lines(before.get("eval_arena.ent", b""))
    spawns = {tuple(l.split()[1:4]) for l in filed}
    pre, post = mons(sec.get("before", [])), mons(sec.get("after", []))
    check(len(filed) == 6 and len(pre) == 6
          and all((r["type"], str(r["x"]), str(r["z"])) in spawns for r in pre),
          "before: eval_arena's six monsters, each on the square its record names", str(pre))
    # THE CONTROL: the fight happened, so the squares a save could take differ.
    away = [r for r in post if (r["type"], str(r["x"]), str(r["z"])) not in spawns]
    check(len(post) == 6 and bool(away) and any(r["dead"] for r in post)
          and any(r["aware"] for r in post),
          "after the fight: a monster off its square, one dead, one aware",
          f"away={away} dead={[r for r in post if r['dead']]}")
    check(any(l.startswith("saved levels: ") and "eval_arena" in l for l in sec.get("after", [])),
          "savemap wrote eval_arena")
    after = level_files(SCRATCH)
    # THE PREMISE of "as filed": the save WROTE the file. One that wrote nothing
    # leaves the filed lines in place, and they would compare equal to
    # themselves. The filed .ent is hand-written; the writer heads its own.
    mark = b"written by the in-game editor"
    check(mark not in before.get("eval_arena.ent", b"") and mark in after.get("eval_arena.ent", b""),
          "THE PREMISE: savemap rewrote eval_arena.ent (the filed one is hand-written, and the "
          "writer heads its own)", after.get("eval_arena.ent", b"")[:120].decode("utf-8", "replace"))
    written = monster_lines(after.get("eval_arena.ent", b""))
    check(written == filed,
          "eval_arena.ent's monster lines are byte for byte as filed (each at its spawn)",
          f"{written} vs {filed}")
finally:
    drop()

# 2. A RECORD'S ID IS NEVER HANDED ON, AND AN ERASE IS UNDONE WHOLE (C327).
RECLINE = re.compile(r"^editor records (\S+) (\d+),(\d+): (?:(none)|(\S+) (\S+) id=(\d+) held=([01]))$")


def records(lines):
    """The `editor records` rows of a section: dicts of stem, x, z and - unless
    the square held none - kind, type, id and held."""
    out = []
    for l in lines:
        m = RECLINE.match(l)
        if m:
            row = {"stem": m.group(1), "x": int(m.group(2)), "z": int(m.group(3))}
            if not m.group(4):
                row.update(kind=m.group(5), type=m.group(6), id=int(m.group(7)),
                           held=m.group(8) == "1")
            out.append(row)
    return out


fresh()
try:
    log = run("recordarrive.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    check("end" in sec, "the script ran to its end")
    swarm = mon_at(mons(sec.get("killed", [])), "skel_swarm", 13, 13)
    check(bool(swarm) and swarm["dead"],
          "the swarm at 13,13 - eval_arena's last record - lies dead", str(swarm))
    # THE PREMISE: the swarm's record, and its death held under that record's id.
    first = records(sec.get("killed", []))
    swarm_id = first[0].get("id") if first else None
    check(len(first) == 1 and first[0]["stem"] == "eval_arena" and first[0].get("type") == "skel_swarm"
          and first[0].get("held") is True,
          "from crypt1, browsed eval_arena's 13,13 holds the swarm's record, its death held under "
          "that record's id", str(first))
    check(any(l == "editor erase: 13,13" for l in sec.get("killed", [])),
          "the swarm's record erased on the browsed level", str(sec.get("killed", [])[-8:]))
    # 1. UNDO: the record back with its id, and the held diff for that id with it
    # (EraseRemote used to drop it, and the undo snapshot - the ACTIVE level's
    # state alone - could not bring it back).
    undone = records(sec.get("undone", []))
    check(len(undone) == 1 and undone[0]["stem"] == "eval_arena" and undone[0].get("type") == "skel_swarm"
          and undone[0].get("id") == swarm_id and undone[0].get("held") is True,
          "UNDO: the swarm's record is back with its id, and its death is still held", str(undone))
    back = mon_at(mons(sec.get("back", [])), "skel_swarm", 13, 13)
    check(bool(back) and back["dead"],
          "back on eval_arena after the undo, the swarm still lies dead (not risen)",
          str(mons(sec.get("back", []))))
    # 2. A NEW ID: the record placed after the swarm's erase takes an id above
    # every one the level had - never the swarm's, which its held death names.
    placed = sec.get("placed", [])
    check(any(l == "editor erase: 13,13" for l in sec.get("back", []))
          and any(l == "editor place: skeleton at 15,15" for l in sec.get("back", [])),
          "from crypt1 again, the swarm's record erased and a skeleton placed at 15,15 on the "
          "browsed level", str(sec.get("back", [])[-8:]))
    after = records(placed)
    gone = [r for r in after if (r["x"], r["z"]) == (13, 13)]
    new = [r for r in after if (r["x"], r["z"]) == (15, 15)]
    check(len(gone) == 1 and "id" not in gone[0], "13,13 holds no record now", str(gone))
    check(len(new) == 1 and new[0].get("type") == "skeleton" and swarm_id is not None
          and new[0].get("id", -1) > swarm_id and new[0].get("held") is False,
          "the skeleton's record took a new id - above the erased swarm's, which no held diff "
          "can name on it (max(id)+1 handed it the swarm's)", f"swarm id={swarm_id} new={new}")
    reads = stash_reads(placed)
    check(bool(reads) and reads[0]["active"] == "crypt1" and "eval_arena" in reads[0]["ents"]
          and "eval_arena" in reads[0]["states"],
          "eval_arena's records stashed and its state (the dead swarm) held, from crypt1",
          str(reads[:1]))
    arrived = mons(sec.get("arrived", []))
    skel = mon_at(arrived, "skeleton", 15, 15)
    check(bool(skel) and not skel["dead"] and skel["hp"] > 0,
          "back on eval_arena, the skeleton stands alive at 15,15 (not dead on the swarm's square)",
          str(arrived))
    check(not any(r["type"] == "skel_swarm" for r in arrived) and len(arrived) == 6,
          "the swarm is gone and the other five stand as they were", str(arrived))
finally:
    drop()

# 3. A RESPAWN FROM THE RECORDS LOSES NOTHING; AN UNDO RAISES NOBODY (C311).
fresh()
try:
    before = level_files(SCRATCH)
    log = run("recordrespawn.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    check("end" in sec, "the script ran to its end")
    start, undone = mons(sec.get("start", [])), mons(sec.get("undone", []))
    check(bool(mon_at(start, "skel_coward", 12, 1)), "the coward stood at 12,1 to begin with",
          str(start))
    check(any(b[:4] == ("decoration", "barrel", 16, 16) for b in brks(sec.get("start", []))),
          "the barrel was placed (the edit the undo takes back)", str(brks(sec.get("start", []))))
    check(not any(r["type"] == "skel_coward" for r in undone) and len(undone) == 5,
          "UNDO: the barrel's undo leaves the erased coward erased", str(undone))
    check(not any(b[1] == "barrel" for b in brks(sec.get("undone", []))),
          "...and takes the barrel back", str(brks(sec.get("undone", []))))

    def kept(name, label, skel="skeleton"):
        lines = sec.get(name, [])
        rows = mons(lines)
        dead = mon_at(rows, skel, 12, 6)
        check(bool(mon_at(rows, skel, 16, 18)) and not mon_at(rows, skel, 16, 18)["dead"],
              f"{label}: the skeleton the editor placed at 16,18 is there", str(rows))
        check(bool(dead) and dead["dead"], f"{label}: the skeleton killed at 12,6 is still dead",
              str(dead))
        check(not any(r["type"] == "skel_coward" for r in rows),
              f"{label}: the erased coward has not come back", str(rows))
        check(any(b[:4] == ("decoration", "crate", 18, 18) for b in brks(lines)),
              f"{label}: the crate the editor placed at 18,18 is there", str(brks(lines)))
        check(any(l.startswith("flooritems 14,13: potion_health ") for l in lines),
              f"{label}: the potion dropped at 14,13 lies there",
              str([l for l in lines if l.startswith("flooritems")]))
        return rows

    staged_skel = mon_at(kept("staged", "staged"), "skeleton", 16, 18)
    # THE CONTROL that the type Save rebuilt anything: it raises the skeleton
    # kind's hp from 16 to 20, and the placed skeleton - untouched, so nothing
    # held for it - is respawned from the kind read afresh. Every check of what
    # was KEPT would pass just as well if the Save had respawned nothing.
    saved_skel = mon_at(mons(sec.get("type saved", [])), "skeleton", 16, 18)
    check(bool(staged_skel) and staged_skel["hp"] == 16.0
          and bool(saved_skel) and saved_skel["hp"] == 20.0,
          "THE CONTROL: the placed skeleton reads hp 16 staged and hp 20 after the type Save "
          "(the hp the Save gave its kind) - the Save respawned the level",
          f"staged={staged_skel} saved={saved_skel}")
    check(any(b[:4] == ("door", "wooden_door", 2, 5) for b in brks(sec.get("staged", []))),
          "staged: the door placed at 2,5", str(brks(sec.get("staged", []))))
    # The inspector's own readout after the pick, before its Save (the lines
    # ahead of the "staged" header).
    check(("skel_archer", "sentry") in archetypes(sec.get("undone", [])),
          "staged: the archer made a sentry in its inspector",
          str(archetypes(sec.get("undone", []))))
    for name, label, skel in (("type saved", "after the type Save", "skeleton"),
                              ("renamed", "after the Rename", "bones")):
        kept(name, label, skel)
        lines = sec.get(name, [])
        check(("skel_archer", "sentry") in archetypes(lines),
              f"{label}: the archer is still a sentry", str(archetypes(lines)))
        check(doors_said(lines) == ["door 2,5 -> shut", "door 2,5 -> open"],
              f"{label}: the door was still open (a hand on it shuts it)", str(doors_said(lines)))
    back = mons(sec.get("renamed back", []))
    check(bool(mon_at(back, "skeleton", 16, 18)) and not any(r["type"] == "bones" for r in back),
          "renamed back: skeletons again", str(back))
    refs = re.compile(r"monsters 'skel_lurker': (\d+) level record\(s\), (\d+) other")
    before_refs = [refs.match(l).groups() for l in sec.get("renamed back", []) if refs.match(l)]
    after_refs = [refs.match(l).groups() for l in sec.get("delete refused", []) if refs.match(l)]
    check(before_refs == [("0", "0")],
          "THE PREMISE: no level and no catalog names skel_lurker", str(before_refs))
    check(any(l.startswith("typeset delete monsters 'skel_lurker': refused")
              for l in sec.get("renamed back", [])) and after_refs == [("1", "0")],
          "a delete of the type only an editor-placed lurker uses is refused, and its "
          "record counts", f"{after_refs} {[l for l in sec.get('renamed back', []) if 'typeset' in l]}")
    # THE FINAL SAVE wrote the file read below. The inspector's Save (staged)
    # writes the level too, and its file alone holds every line looked for but
    # the lurker's - placed after it, so only this save can have written that.
    check(any(l.startswith("saved levels: ") and "eval_arena" in l
              for l in sec.get("delete refused", [])),
          "the closing savemap wrote eval_arena", str(sec.get("delete refused", [])))
    after = level_files(SCRATCH)
    ent = after.get("eval_arena.ent", b"").decode("utf-8")
    lines = monster_lines(after.get("eval_arena.ent", b""))
    check(any(l.split()[1:4] == ["skel_lurker", "20", "20"] for l in lines),
          "the save writes the lurker placed after the inspector's Save (so it is this save's file)",
          str(lines))
    check(any(l.split()[1:4] == ["skeleton", "16", "18"] for l in lines)
          and any(l.split()[1:4] == ["skeleton", "12", "6"] for l in lines)
          and not any(l.split()[1] == "skel_coward" for l in lines),
          "the save writes the placed skeleton and the dead one's spawn, and no coward", str(lines))
    check(any(l.split()[1:4] == ["skel_archer", "25", "7"] and "archetype=sentry" in l.split()
              for l in lines), "...the archer as a sentry", str(lines))
    check(any(l.startswith("door wooden_door 2 5 ") for l in ent.splitlines()),
          "...and the door at 2,5", ent)
finally:
    drop()

# 4. NO EDITOR REMOVAL TRIPS THE LEDGER (C355).
fresh()
try:
    log = run("recordledger.eval")
    check(passed(log), "the script ran clean (strict: a violation would have ended it)")
    sec = console_sections(log)
    check("end" in sec, "the script ran to its end")
    staged, erased = sec.get("staged", []), sec.get("erased", [])
    ms, bs = mons(staged), brks(staged)
    # THE PREMISE of each removal: what slides up has other hit points.
    order = [(r["type"], r["hp"]) for r in ms]
    check(order[:4] == [("skel_coward", 10.0), ("skeleton", 16.0), ("skel_archer", 12.0),
                        ("skel_warrior", 22.0)],
          "staged: the monsters in list order coward 10, skeleton 16, archer 12, warrior 22",
          str(order))
    props = [b for b in bs if b[0] == "decoration"]
    doors = [b for b in bs if b[0] == "door"]
    check([b[1] for b in props] == ["barrel", "crate", "crate"]
          and len({b[4] for b in props}) == 3,
          "staged: a barrel, then two crates battered apart - three hit points", str(props))
    check([b[2:4] for b in doors] == [(2, 5), (6, 5), (10, 5)]
          and len({b[4] for b in doors}) == 3,
          "staged: three doors, the last two battered apart - three hit points", str(doors))
    pipes = [PIPE.search(l) for l in staged + erased]
    pipes = [p for p in pipes if p]
    check(len(pipes) == 2 and all(p.group(1) == "PASS" and p.group(3) == "0" for p in pipes)
          and int(pipes[1].group(2)) > int(pipes[0].group(2)),
          "the ledger checked on through the removals and found no violation",
          str([p.group(0) for p in pipes]))
    check(any("strict=on" in l for l in erased), "...with strict on", str(erased))
    check("one-pipeline violation" not in log, "no violation in the log",
          str([l for l in log.splitlines() if "one-pipeline violation" in l][:3]))
    left = mons(erased)
    check([r["type"] for r in left] == ["skel_warrior", "skel_mage", "skel_swarm"],
          "every removal happened: three monsters left", str(left))
    left_b = brks(erased)
    check([b[1:4] for b in left_b] == [("wooden_door", 10, 5), ("crate", 20, 16)],
          "...one door and one crate left", str(left_b))
finally:
    drop()


# --- phase 33: placement on a browsed level, and an item into a niche -------------
print("33 - a browsed level's window, hung props and niche item land there; an item goes into a niche")
CELL33 = re.compile(r"editor cell (\S+) (\d+),(\d+) (\w+) wall=(\S+)/\S+ .* bore=(\S+)$")


def cells33(lines):
    """(level, x, z, open|solid, wall store, bore) of each `editor cell` line."""
    return [(m.group(1), int(m.group(2)), int(m.group(3)), m.group(4), m.group(5), m.group(6))
            for m in map(CELL33.match, lines) if m]


def level_text33(stem, ext):
    return io.open(os.path.join(PROJ, "levels", stem + ext), encoding="utf-8").read()


def grid33(text):
    """The saved .map's grid rows (the lines between the two ';' fences that
    hold only grid glyphs)."""
    return [l for l in text.splitlines() if l and set(l) <= set("#.PDTF")]


def records33(text, kind):
    """The token lists of each `<kind> ...` record line."""
    return [l.split() for l in text.splitlines() if l.startswith(kind + " ")]


def hung_on_floor33(text):
    """Every `wall=` decoration record of a saved .map whose wall is OPEN floor
    in that map's own grid - what the parser asserts on."""
    rows = grid33(text)
    steps = {"north": (0, -1), "east": (1, 0), "south": (0, 1), "west": (-1, 0)}
    bad = []
    for r in records33(text, "decoration"):
        wall = next((t.split("=", 1)[1] for t in r if t.startswith("wall=")), None)
        if wall not in steps:
            continue
        x, z = int(r[2]) + steps[wall][0], int(r[3]) + steps[wall][1]
        if 0 <= z < len(rows) and 0 <= x < len(rows[z]) and rows[z][x] != "#":
            bad.append(" ".join(r))
    return bad


def apples33(ent_text, x, z):
    """The `item apple <x> <z>` records of a .ent, each as its niche= ('' none)."""
    out = []
    for r in records33(ent_text, "item"):
        if r[1:4] == ["apple", str(x), str(z)]:
            out.append(next((t.split("=", 1)[1] for t in r if t.startswith("niche=")), ""))
    return out


fresh()
try:
    c1map0, ea_map0 = level_text33("crypt1", ".map"), level_text33("eval_arena", ".map")
    c1ent0, ea_ent0 = level_text33("crypt1", ".ent"), level_text33("eval_arena", ".ent")
    # THE CONTROL: the squares are what the script takes them for. crypt1's 8,2,
    # 3,2 and 6,2 are rock with floor north and south; 2,3 is rock and 6,3 has no
    # other wall; 12,5 is floor between rock east and west; no level has a bore,
    # a banner or a niche item; eval_arena's 6,23 and 9,23 are its border rock.
    g = grid33(c1map0)
    check(all(g[2][x] == "#" and g[1][x] == "." and g[3][x] == "." for x in (3, 6, 8))
          and g[3][2] == "#" and g[3][5] == "." and g[3][7] == "." and g[4][6] == "."
          and g[5][11] == "#" and g[5][12] == "." and g[5][13] == "#" and g[6][12] == ".",
          "THE CONTROL: crypt1's squares are as the script takes them", "\n".join(g))
    check(not records33(c1map0, "bore") and not records33(ea_map0, "bore")
          and not [r for r in records33(c1map0, "decoration") if r[1] == "banner"]
          and "niche=" not in c1ent0 and "niche=" not in ea_ent0
          and grid33(ea_map0)[23][6] == "#" and grid33(ea_map0)[23][9] == "#",
          "THE CONTROL: no bore, no banner on crypt1 and no niche item anywhere; eval_arena's 6,23 "
          "and 9,23 are its border rock")
    log = run("browsedplace.eval")
    check(passed(log), "the script ran clean (its two refusals expected, nothing else refused)")
    sec = console_sections(log)
    check("end" in sec, "the script ran to its end - every parse of crypt1 after the save "
          "survived (a banner record facing open floor is a DN_ASSERT)",
          "\n".join(harness_game.fatal_lines(log)[:5]))

    # 1. THE WINDOW lands in the BROWSED level's stash, and the erase takes it.
    bore = sec.get("bore", [])
    cb = cells33(bore)
    check(len(cb) == 2 and cb[0][:4] == ("crypt1", 8, 2, "solid") and cb[0][5] == "-"
          and cb[1][5] == "window/z",
          "a window bored from crypt1's 8,3 north face, crypt1 browsed, bores crypt1's block 8,2 "
          "along z (the brush bored the ACTIVE level, here open floor, and refused)", str(bore))
    check("stashes: maps=crypt1 ents=none" in " | ".join(bore)
          and any(l.startswith("stashes: active eval_arena map=clean") for l in bore),
          "...into crypt1's stash, map only, and the active level's map stays clean", str(bore))
    erase = sec.get("erase", [])
    ce = cells33(erase)
    check("editor erase: 8,2" in erase and len(ce) == 1 and ce[0][5] == "-"
          and ce[0][4] == cb[1][4] == "pin1",
          "the erase of 8,2 takes the window and leaves the block's pinned wall (the browsed "
          "ladder had no bore rung and reset the wall instead)", str(erase))

    # 2. HUNG PROPS re-hang when their wall opens - in the saved file, which then loads.
    banner = sec.get("banner", [])
    cbn = cells33(banner)
    check([c[:4] for c in cbn] == [("crypt1", 3, 2, "open"), ("crypt1", 6, 2, "open")]
          and "editor place: banner at 3,3 on north" in banner
          and "editor place: banner at 6,3 on north" in banner,
          "two banners hung on crypt1, then the walls behind them opened by floor paint", str(banner))
    c1map = level_text33("crypt1", ".map")
    banners = sorted(" ".join(r) for r in records33(c1map, "decoration") if r[1] == "banner")
    check(banners == ["decoration banner 3 3 west wall=west"],
          "saved, 3,3's banner hangs on its west wall and 6,3's, with no wall left, is gone",
          str(banners))
    check(not hung_on_floor33(c1map), "no `wall=` record of the saved crypt1.map faces open floor",
          str(hung_on_floor33(c1map)))
    check(not records33(c1map, "bore"), "the saved crypt1.map has no bore (it was erased)",
          str(records33(c1map, "bore")))
    check(not records33(level_text33("eval_arena", ".map"), "bore"),
          "...and the active level eval_arena gained none either")
    chk = sec.get("check", [])
    check(any(l.startswith("validate: ") for l in chk) and "viewing crypt1" in chk
          and any(c[:4] == ("crypt1", 3, 2, "open") for c in cells33(chk))
          and any(l.startswith("editor issues: ") for l in chk),
          "after a reset - no stash left, crypt1 read from the file just written - the Check, a "
          "browse of crypt1 and the live check all ran", str(chk))

    # 3. AN ITEM INTO A NICHE, by the brush: on the browsed level and the active one.
    bn = sec.get("browsedniche", [])
    check("editor place: niche at 12,5 on east" in bn
          and "editor place: apple at 12,5 on east" in bn
          and "editor place: apple at 12,6" in bn,
          "on crypt1 a niche cut in 12,5's east wall, an apple placed into it, one on the floor "
          "of 12,6 (12,5's west wall, rock with no niche, refused - the expected refusal)", str(bn))
    check("editor ghost: square 13,5 face 12,5 east place 12,5 niche east" in bn,
          "the pointer over the block 13,5 by its west edge picks the face 12,5 east and the "
          "ghost is the niche (the item brush tracked no face)", str(bn))
    gh = sec.get("ghost", [])
    slot = [l for l in gh if l.startswith("editor ghost: square 6,21 ")]
    check("editor ghost: square 6,23 face 6,22 south place 6,22 niche south" in gh
          and "editor ghost: square 9,23 face 9,22 south refused map.place.nofloor" in gh
          and len(slot) == 1 and re.match(r"editor ghost: square 6,21 face none place 6,21 slot \d$",
                                          slot[0]),
          "on eval_arena the pointer over 6,23 by its north edge: the face 6,22 south, a ghost "
          "INTO its niche; over 9,23, a face but no niche - refused as rock; over the floor 6,21, "
          "no face and a quarter", str(gh))
    check("editor ghost: clicked 6,23" in gh and "editor place: apple at 6,22 on south" in gh,
          "the click at 6,23 placed what the ghost showed, and `editor place` naming the wall "
          "a second", str(gh))
    check(any("editor place: no wall north of 6,21 for apple to go into (6,20 is open)" in l
              for l in gh),
          "`editor place` naming 6,21's north wall, the floor 6,20 behind it, is refused as no "
          "wall to go into (it laid a floor apple on 6,20 and said it went into the niche)", str(gh))
    c1ent, ea_ent = level_text33("crypt1", ".ent"), level_text33("eval_arena", ".ent")
    check(apples33(ea_ent0, 6, 20) == [] and apples33(ea_ent, 6, 20) == [],
          "...and the saved eval_arena.ent has no apple on 6,20", str(records33(ea_ent, "item")))
    check(apples33(c1ent, 12, 5) == ["east"] and apples33(c1ent, 12, 6) == [""],
          "saved, crypt1.ent holds the apple IN 12,5's east niche (niche=east) and the floor "
          "apple of 12,6 with none", str(records33(c1ent, "item")))
    check(apples33(ea_ent, 6, 22) == ["south", "south"],
          "...and eval_arena.ent the two apples in 6,22's south niche (niche=south)",
          str(records33(ea_ent, "item")))
    check(len(records33(c1map, "niche")) == len(records33(c1map0, "niche")) + 1
          and ["niche", "niche", "12", "5", "east"] in records33(c1map, "niche"),
          "crypt1.map gained exactly the niche at 12,5 east", str(records33(c1map, "niche")))
finally:
    drop()


# --- phase 34: the brush by id, an erase of nothing, the start square -------------
print("34 - the brush holds its type across levels; an empty erase keeps the redo; the start stays")
CELL34 = re.compile(r"editor cell (\S+) (\d+),(\d+) (\w+) wall=(\S+) floor=(\S+) ceiling=(\S+) bore=\S+$")


def sections34(log):
    """The console AND the editor's message lines, split on '--- name ---' -
    an editor report (its message line) is what says why a brush did nothing."""
    out, name = {}, None
    for line in log.splitlines():
        if "console: " in line:
            text = line.split("console: ", 1)[1]
        elif "editor: " in line:
            text = "editor: " + line.split("editor: ", 1)[1]
        else:
            continue
        m = re.match(r"--- (.*) ---$", text)
        if m:
            name = m.group(1)
            out[name] = []
        elif name is not None:
            out[name].append(text)
    return out


def cells34(lines, x, z):
    """(open|solid, wall, floor, ceiling) of each `editor cell` line for x,z."""
    return [(m.group(4), m.group(5), m.group(6), m.group(7))
            for m in map(CELL34.match, lines) if m and (int(m.group(2)), int(m.group(3))) == (x, z)]


def armed34(lines):
    """The `editor palette item floors` listings, each as {id: armed}."""
    out, cur = [], None
    for l in lines:
        m = re.match(r"editor palette item floors (\S+) .* armed=(\d)$", l)
        if m:
            if cur is None:
                cur = {}
                out.append(cur)
            cur[m.group(1)] = m.group(2) == "1"
        else:
            cur = None
    return out


def floor_palette34(stem):
    text = io.open(os.path.join(PROJ, "levels", stem + ".map"), encoding="utf-8").read()
    return next((l.split()[2:] for l in text.splitlines() if l.startswith("palette floor ")), [])


# The developer's settings.ini beside the exe: the script's `editor palette
# catalog off` saves map_show_catalog there (MapEditor::SetShowCatalog), so the
# file is put back as it was found, whatever happened.
settings_before = io.open(SETTINGS, "rb").read() if os.path.isfile(SETTINGS) else None
fresh()
try:
    # THE CONTROL: both levels list the same four floors, slate and cobble among
    # none of them, so a fifth entry is each level's own.
    shared = ["floor_temple", "floor_ancient_stone", "floor_slabs", "floor_cobble_mossy"]
    check(floor_palette34("crypt1") == shared and floor_palette34("eval_arena") == shared,
          "THE CONTROL: crypt1 and eval_arena start with the same four floors",
          f"{floor_palette34('crypt1')} | {floor_palette34('eval_arena')}")
    log = run("brushundo.eval")
    check(passed(log), "the script ran clean (its refusals expected, nothing else refused)")
    sec = sections34(log)
    check("end" in sec, "the script ran to its end", "\n".join(harness_game.fatal_lines(log)[:5]))
    caught = [l for l in log.splitlines() if "exception on '" in l]
    check(not caught, "no frame threw on the way (a caught throw is not a refusal)", "\n".join(caught[:3]))

    # 1. C352: the brush is its TYPE, on any level browsed.
    br = sec.get("browse", [])
    check("editor palette add: floor_cobble -> crypt1 floors, armed floor_cobble" in br
          and "editor palette add: floor_slate -> eval_arena floors, armed floor_slate" in br,
          "crypt1's fifth floor is cobble, eval_arena's slate - the brush armed with slate", str(br))
    lists = armed34(br)
    check(len(lists) == 3, "three floor listings were read", str(lists))
    if len(lists) == 3:
        check([k for k, v in lists[0].items() if v] == ["floor_slate"],
              "on eval_arena the slate row is the one lit", str(lists[0]))
        check("floor_cobble" in lists[1] and not any(lists[1].values()),
              "browsed to crypt1, NO row lights - crypt1 has no slate (the row number lit its fifth, "
              "cobble)", str(lists[1]))
        check(lists[2].get("floor_slate") is True and [k for k, v in lists[2].items() if v] == ["floor_slate"],
              "after the paint crypt1 lists slate, and it is the row lit", str(lists[2]))
    check(any(l.endswith("armed floors:floor_slate") for l in br),
          "the brush is still floors:floor_slate on crypt1", str(br))
    c44 = cells34(br, 4, 4)
    check(len(c44) == 2 and c44[1][2] == "pin5/floor_slate" and c44[0][2] != c44[1][2],
          "a click on crypt1's 4,4 paints SLATE there, enrolled as its sixth floor (the row number "
          "painted crypt1's fifth, cobble)", str(c44))

    # 2. C353: an erase of nothing keeps the redo, live and browsed.
    rd = sec.get("redo", [])
    a = cells34(rd, 5, 18)
    check(len(a) == 4 and a[1][2] == "pin2/floor_slabs" and a[2] == a[0] and a[3] == a[1],
          "eval_arena 5,18: painted slabs, undone back as it was, and REDONE after the empty "
          "erase (an empty erase took an undo step and wiped the redo)", str(a))
    b = cells34(rd, 5, 5)
    check(len(b) == 4 and b[0][2].startswith("hash/") and b[1][2] == "pin2/floor_slabs"
          and b[2] == b[0] and b[3] == b[1],
          "browsed crypt1 5,5: erased bare, painted slabs, undone, and REDONE after the empty erase",
          str(b))
    check("editor records eval_arena 7,18: none" in rd and "editor records crypt1 5,5: none" in rd
          and rd.count("redone") == 2,
          "the erased squares held no record, and both redos ran", str(rd))
    check("editor: Nothing to erase at 7, 18" in rd and "editor: Nothing to erase at 5, 5" in rd
          and not any(l.startswith("editor: Reset cell 7, 18") for l in rd),
          "each empty erase SAYS nothing was there (it said the cell was reset)", str(rd))

    # 3. C348: the start square stays open.
    st = sec.get("start", [])
    check(any("editor place: wall_marble at 14,12 refused - the start square stays open" in l for l in st)
          and any("editor place: wall_marble at 7,7 refused - the start square stays open" in l for l in st),
          "a wall on eval_arena's start 14,12 and on browsed crypt1's 7,7 is refused", str(st))
    s1412 = cells34(st, 14, 12)
    check(len(s1412) == 2 and all(c[0] == "open" for c in s1412)
          and [c[0] for c in cells34(st, 13, 12)] == ["solid"]
          and [c[0] for c in cells34(st, 15, 13)] == ["solid"]
          and [c[0] for c in cells34(st, 7, 7)] == ["open"],
          "the start squares stay open; the rectangle raised the walls round 14,12", str(st))
    check(st.count("editor: The start square 14, 12 stays open") == 2
          and st.count("editor: The start square 7, 7 stays open") == 1,
          "said once a gesture: the place, the rectangle, the browsed place", str(st))

    # 4. C348: a crop that leaves out the start is refused, naming it.
    cr = sec.get("crop", [])
    check("editor: Can't crop it there - the start square 14, 12 would be cut off" in cr
          and "editor: Can't crop it there - the start square 7, 7 would be cut off" in cr,
          "a crop leaving out eval_arena's start and one leaving out crypt1's are refused naming the "
          "start (it was counted only as a floor square)", str(cr))
    check(any(l.startswith("editor view: crypt1 14x10 ") for l in cr),
          "crypt1 is its old size after the refusal", str(cr))
finally:
    drop()
    if settings_before is not None:
        io.open(SETTINGS, "wb").write(settings_before)
    elif os.path.isfile(SETTINGS):
        os.remove(SETTINGS)


# --- phase 35: the types the game authors are found by flag ---------------------
print("35 - an ambush's exit stair and a generated lock are found by flag: a rename or delete reaches them")
DOOR35 = re.compile(r"^\s*\d+ (\S+) @ \d+,\d+ ")
LOCKS35 = re.compile(r"^generate: built .* locks (\d+)/(\d+),")
POS35 = re.compile(r"^(\d+),(\d+) facing (\w+)$")
STEP35 = {"north": (0, -1), "east": (1, 0), "south": (0, 1), "west": (-1, 0)}


def doors35(lines):
    """The door types `doors` listed, in order ([] for `no doors`)."""
    return [m.group(1) for m in map(DOOR35.match, lines) if m]


def locks35(lines):
    """(got, wanted) of each `generate: built` report."""
    return [(int(m.group(1)), int(m.group(2))) for m in map(LOCKS35.match, lines) if m]


fresh()
try:
    log = run("exitlockrename.eval")
    check(passed(log), "the rename script ran clean (its one refusal expected)")
    sec = console_sections(log)
    check("end" in sec, "and ran to its end - no abort on a model nobody had",
          "\n".join(harness_game.fatal_lines(log)[:5]))

    # 1. C334: the lock follows its type's rename.
    lr = sec.get("lock renamed", [])
    check("typeset rename doors 'wooden_door': done" in lr and locks35(lr) == [(1, 1)]
          and doors35(lr) == ["oak_door"],
          "wooden_door renamed oak_door: the generated floor's one lock is an oak_door, and it "
          "loaded (the generator wrote `wooden_door`)", str(lr))

    # 2. C333: the ambush's way out follows the exit type's rename.
    er = sec.get("exit renamed", [])
    way = re.search(r"encounter: the way out is stairs (\S+) (\d+) (\d+) (\w+) dest=- ", log)
    check("typeset rename stairs 'stairs_exit': done" in er and way is not None
          and way.group(1) == "gate_stair",
          "stairs_exit renamed gate_stair: the ambush's way out is a gate_stair (it was the "
          "literal stairs_exit, and the ambush aborted)", way.group(0) if way else str(er))
    poses = [m.groups() for m in map(POS35.match, er) if m]
    ok = way is not None and len(poses) == 2
    if ok:
        (x0, z0, f0), (x1, z1, _) = poses
        dx, dz = STEP35.get(f0, (0, 0))
        ok = ((int(x0), int(z0), f0) == (int(way.group(2)), int(way.group(3)), way.group(4))
              and (int(x1), int(z1)) == (int(x0) + dx, int(z0) + dz))
    check(ok, "the party arrives on it facing the way it faces, and a step forward moves it there",
          str(poses))

    # 3. C333: the last exit type cannot be deleted; a second one could.
    le = sec.get("last exit", [])
    check("newasset stairs 'gate_stair_b' from gate_stair: created" in le
          and "typeset delete stairs 'gate_stair_b': done" in le,
          "THE CONTROL: with two exit types, one of them deletes", str(le))
    check(any(l.startswith("typeset delete stairs 'gate_stair': refused - The world's last exit stair")
              for l in le),
          "the last one is refused, said as the last exit (it is in use too; that is not what is "
          "said)", str(le))
finally:
    drop()

fresh()
try:
    log = run("exitlockdelete.eval")
    check(passed(log), "the delete script ran clean")
    sec = console_sections(log)
    check("end" in sec, "and ran to its end - every generated floor loaded",
          "\n".join(harness_game.fatal_lines(log)[:5]))
    ld = sec.get("lock deleted", [])
    check("typeset delete doors 'wooden_door': done" in ld and locks35(ld) == [(1, 1)]
          and doors35(ld) == ["stone_door"],
          "wooden_door deleted: the lock is the next offered door with a hand-hold, stone_door",
          str(ld))
    lm = sec.get("lock marked", [])
    check(locks35(lm) == [(1, 1)] and doors35(lm) == ["portcullis"],
          "the portcullis marked `lock = 1`: the mark wins over the fallback", str(lm))
    nl = sec.get("no lock door", [])
    check(locks35(nl) == [(0, 1)] and "no doors" in nl,
          "nothing qualifies (no mark, no other door with an opener): no lock, 0 of the 1 asked "
          "for, and no door", str(nl))
finally:
    drop()


# --- phase 40: one world tick ----------------------------------------------------
print("40 - one world tick: the open console holds the world as play does, and follows a stair")
CLOCK = re.compile(r"worldclock updates=(\d+) seconds=([\d.]+) level=(\S+) state=(\S+) runs=(\w+) "
                   r"held=(\S+) console=(\w+) editor=(\w+)$")


def clocks(lines):
    """Every `worldclock` reading in a section, as a dict."""
    keys = ("updates", "seconds", "level", "state", "runs", "held", "console", "editor")
    out = []
    for m in map(CLOCK.match, lines):
        if m:
            d = dict(zip(keys, m.groups()))
            d["updates"], d["seconds"] = int(d["updates"]), float(d["seconds"])
            out.append(d)
    return out


def brief(c):
    return " | ".join(f"u={r['updates']} s={r['seconds']:.2f} {r['level']} {r['state']} runs={r['runs']} "
                      f"held={r['held']} console={r['console']} editor={r['editor']}" for r in c)


fresh()
# The developer's settings.ini beside the exe: nothing this script does is a
# setting (an `editor tool` would save the picked tool there), so the run must
# leave it as it found it - checked, and put back whatever happened.
settings_before = io.open(SETTINGS, "rb").read() if os.path.isfile(SETTINGS) else None
try:
    log = run("worldtick.eval")
    check(passed(log), "the script ran clean")
    settings_after = io.open(SETTINGS, "rb").read() if os.path.isfile(SETTINGS) else None
    moved = sorted(set((settings_after or b"").decode("utf-8", "replace").splitlines())
                   ^ set((settings_before or b"").decode("utf-8", "replace").splitlines()))
    check(settings_after == settings_before, "the run left settings.ini beside the exe as it found it",
          " | ".join(moved[:6]))
    sec = console_sections(log)
    check("end" in sec, "the script ran to its end")

    # A paused editor stays paused through every way of asking for Editor mode
    # it is already in; only a real flip clears it (C78). Each ask goes through
    # MapView::SetMode(Editor), and the pause is pressed again before each, so
    # each reading after an ask fails on its own if that ask clears it.
    # One reading per `editor pause` line: the press and its readback, then
    # after the bare `editor`, the re-press and `editor pick`, the re-press and
    # `editor issues`, `editor off` and `editor` again.
    pauses = [l.split("editor pause: ", 1)[1] for l in sec.get("pause", []) if l.startswith("editor pause: ")]
    if len(pauses) != 9:
        check(False, "nine readings of the editor's pause", str(pauses))
    else:
        check(pauses[:2] == ["paused", "paused"] and pauses[3] == pauses[5] == "paused",
              "THE CONTROL: the pause button presses, and each re-press before an ask holds",
              str([pauses[0], pauses[1], pauses[3], pauses[5]]))
        check(pauses[2] == "paused", "a bare `editor` leaves the editor paused (it used to unpause it)",
              pauses[2])
        check(pauses[4] == "paused", "...so does `editor pick`, the eyedropper, which asks for Editor mode",
              pauses[4])
        check(pauses[6] == "paused", "...and `editor issues`, which asks for it too", pauses[6])
        check(pauses[7:] == ["no editor", "running"],
              "THE CONTROL: `editor off` and back - a real flip - still clears it", str(pauses[7:]))

    # The open console over a paused editor runs nothing; over a running one it
    # runs (the control - the readout can see a tick).
    c = clocks(sec.get("frozen", []))
    if len(c) != 4:
        check(False, "four clock readings over the editor", brief(c))
    else:
        a, b, cc, d = c
        check(a["console"] == "open" and a["editor"] == "paused" and a["held"] == "paused",
              "the console is open over the paused editor, and the decision names the pause", brief(c[:1]))
        check(b["updates"] == a["updates"] and b["seconds"] == a["seconds"],
              "...and the world took no update in four frames (it used to run under the console)", brief(c[:2]))
        check(cc["runs"] == "yes" and d["updates"] > cc["updates"] and d["seconds"] > cc["seconds"],
              "THE CONTROL: unpaused, the same console's frames run the world", brief(c[2:]))

    # An editor dialog holds the world under the console too.
    lines = sec.get("dialog", [])
    c = clocks(lines)
    # `open` says where it stands, then the bare readings before and after Esc.
    opened = [l for l in lines if l.startswith("editor levelsettings: ")]
    check(len(opened) == 3 and all(l.startswith("editor levelsettings: open") for l in opened[:2])
          and opened[2] == "editor levelsettings: closed",
          "the Level settings dialog opened, and Esc closed it", str(opened))
    if len(c) != 3:
        check(False, "three clock readings round the dialog", brief(c))
    else:
        check(c[0]["held"] == "dialog" and c[0]["console"] == "open" and c[1]["updates"] == c[0]["updates"],
              "the open console over an editor dialog runs nothing, and says it is the dialog", brief(c[:2]))
        check(c[2]["runs"] == "yes" and c[2]["held"] == "-", "...and the dialog shut, the world runs",
              brief(c[2:]))

    # The sheet over a level is not a pause: the world runs under it, console
    # shut (the control) and open (it used to freeze).
    c = clocks(sec.get("sheet", []))
    if len(c) != 4:
        check(False, "four clock readings over the sheet", brief(c))
    else:
        h, i, j, k = c
        check(all(r["state"] == "sheet" for r in c), "the sheet stood open for all four", brief(c))
        check(h["console"] == "shut" and i["updates"] > h["updates"] and i["seconds"] > h["seconds"],
              "THE CONTROL: the console shut, the world runs under the sheet", brief(c[:2]))
        check(j["console"] == "open" and k["updates"] > j["updates"] and k["seconds"] > j["seconds"],
              "the console open over the sheet, the world clock still advances (it used to freeze)",
              brief(c[2:]))

    # An exit stair stepped onto under the console: its question goes up, and
    # holds the world - the console used to leave the transition latched.
    lines = sec.get("exit", [])
    c = clocks(lines)
    poses = [l for l in lines if re.match(r"\d+,\d+ facing ", l)]
    if len(c) != 4:
        check(False, "four clock readings round the exit", brief(c))
    else:
        l0, m, n, o = c
        check(l0["level"] == "crypt1" and l0["console"] == "open" and l0["runs"] == "yes",
              "THE CONTROL: on crypt1 with the console open, before the step, the world runs", brief(c[:1]))
        check(m["updates"] > l0["updates"], "...and it ran the step", brief(c[:2]))
        check(m["held"] == "prompt" and m["console"] == "open" and m["state"] == "playing",
              "the exit taken under the console was followed: its question is up (it stayed latched)",
              brief(c[1:2]))
        check(n["updates"] == m["updates"], "...and the question holds the world under the console",
              brief(c[1:3]))
        check(o["runs"] == "yes" and o["held"] == "-" and o["level"] == "crypt1",
              "...until Esc answers No: the party stays, and the world runs", brief(c[3:]))
    check(bool(poses) and poses[-1].startswith("7,8 "), "the party stands on the exit stair",
          str(poses[-1:]))

    # A stair down stepped onto under the console is followed at once: the
    # party is on crypt2 while the console is still up.
    c = clocks(sec.get("stair", []))
    if len(c) != 2:
        check(False, "two clock readings round the stair", brief(c))
    else:
        check(c[0]["level"] == "crypt1" and c[0]["console"] == "open",
              "THE CONTROL: before the step, crypt1 with the console open", brief(c[:1]))
        check(c[1]["level"] == "crypt2" and c[1]["console"] == "open" and c[1]["state"] == "playing",
              "the stair taken under the console was followed: crypt2, the console still open (it stayed "
              "latched on crypt1 until the console shut)", brief(c[1:]))
finally:
    drop()
    if settings_before is not None:
        io.open(SETTINGS, "wb").write(settings_before)
    elif os.path.isfile(SETTINGS):
        os.remove(SETTINGS)


# --- phase 41: the editor map's wheel zoom and toolbar hover ---------------------
print("41 - a wheel zoom keeps the point under the pointer, and a toolbar click leaves nothing lit")
VIEWAT = re.compile(r"^editor view: \S+ \d+x\d+ map (-?\d+),(-?\d+) (\d+)x(\d+) band \d+ "
                    r"zoom=([\d.]+) pan=(-?[\d.]+),(-?[\d.]+) hover=(\S+) cell=(-?\d+),(-?\d+)"
                    r"(?: at=(-?[\d.]+),(-?[\d.]+))?$")
BUTTON = re.compile(r"^editor button level at (-?\d+) (-?\d+)$")


def views(lines):
    """Every `editor view` reading in a section, as a dict."""
    out = []
    for m in map(VIEWAT.match, lines):
        if m:
            g = m.groups()
            out.append({"map": tuple(int(v) for v in g[0:4]), "zoom": float(g[4]),
                        "pan": (float(g[5]), float(g[6])), "hover": g[7],
                        "cell": (int(g[8]), int(g[9])),
                        "at": (float(g[10]), float(g[11])) if g[10] is not None else None})
    return out


fresh()
try:
    # Where to point, read off the game: the map's rectangle at zoom 1 and the
    # Level button, so nothing here depends on the window's size. Both scripts
    # run WITH THEIR WINDOW: the modal section's hover is dropped by a Render
    # with no Update before it, and a headless run renders nothing.
    log = run("mapaim.eval", headless=False)
    check(passed(log), "the aiming script ran clean")
    sec = console_sections(log)
    v = views(sec.get("aim", []))
    b = [m for m in map(BUTTON.match, sec.get("aim", [])) if m]
    if len(v) != 1 or len(b) != 1:
        check(False, "the map's rectangle and the Level button, read", str(sec.get("aim")))
    else:
        mx, my, mw, mh = v[0]["map"]
        # Near the FAR corner, where the old zoom drifted most (it slid the point
        # ~5% of its distance from the map's centre each notch).
        zx, zy = mx + int(mw * 0.85), my + int(mh * 0.85)
        lx, ly = b[0].groups()
        log = run("mapview.eval", headless=False,
                  words={"ET_ZX": str(zx), "ET_ZY": str(zy), "ET_LX": lx, "ET_LY": ly})
        check(passed(log), "the script ran clean")
        sec = console_sections(log)
        check("end" in sec, "the script ran to its end")

        z = views(sec.get("zoom", []))
        if len(z) != 8 or any(r["at"] is None for r in z):
            check(False, "eight readings of the map point under the pointer", str(z))
        else:
            zooms = [r["zoom"] for r in z]
            check(zooms[0] == 1.0 and zooms[1] == 1.0 and 1.0 < zooms[2] < zooms[3] < zooms[4] < 10.0
                  and zooms[5] == zooms[6] == 10.0 and zooms[7] == 1.0,
                  "THE CONTROL: the wheel zoomed - 1, out at 1, in by notches, clamped at 10, back to 1",
                  str(zooms))
            a0 = z[0]["at"]
            check(0.0 < a0[0] and 0.0 < a0[1], "THE CONTROL: the pointer is over the map", str(a0))
            drift = max(max(abs(r["at"][0] - a0[0]), abs(r["at"][1] - a0[1])) for r in z)
            check(drift < 0.005,
                  "every zoom kept the map point under the pointer (it slid away each notch)",
                  f"worst {drift:.4f} squares: " + " ".join(f"{r['at'][0]:.3f},{r['at'][1]:.3f}" for r in z))
            check(z[1]["pan"] == z[0]["pan"],
                  "a notch out at zoom 1 left the pan as it was (it pushed the map off-centre)",
                  f"{z[0]['pan']} -> {z[1]['pan']}")
            check(z[6]["pan"] == z[5]["pan"],
                  "a notch in at zoom 10 left the pan as it was (it kept sliding the map)",
                  f"{z[5]['pan']} -> {z[6]['pan']}")

        h = views(sec.get("hover", []))
        ls = [l for l in sec.get("hover", []) if l.startswith("editor levelsettings: ")]
        if len(h) != 2 or len(ls) != 2:
            check(False, "two hover readings and two dialog readings", str(sec.get("hover")))
        else:
            check(h[0]["hover"] == "level", "THE CONTROL: the pointer over the Level button lights it",
                  h[0]["hover"])
            check(ls[0].startswith("editor levelsettings: open") and ls[1] == "editor levelsettings: closed",
                  "THE CONTROL: the click opened Level settings, and Esc closed it", str(ls))
            check(h[1]["hover"] == "none" and h[1]["cell"] == (-1, -1),
                  "under the dialog the click opened, nothing is hovered (the button stayed lit, "
                  "its tooltip under the dim)", f"hover={h[1]['hover']} cell={h[1]['cell']}")

        # A dialog opened by the CONSOLE has no click site to clear the hover:
        # only the Render-path rule (a Render with no Update before it calls
        # ClearHover) can drop the square under the pointer.
        m = views(sec.get("modal", []))
        ms = [l for l in sec.get("modal", []) if l.startswith("editor levelsettings: ")]
        # Three dialog readings: the `open` itself, the status under it, and the
        # status after Esc.
        if len(m) != 2 or len(ms) != 3:
            check(False, "two hover readings and three dialog readings round the console's open",
                  str(sec.get("modal")))
        else:
            check(m[0]["cell"] != (-1, -1), "THE CONTROL: the pointer over the map hovers a square",
                  f"cell={m[0]['cell']}")
            check(all(l.startswith("editor levelsettings: open") for l in ms[:2])
                  and ms[2] == "editor levelsettings: closed",
                  "THE CONTROL: the console opened Level settings, it stayed open, and Esc closed it",
                  str(ms))
            check(m[1]["cell"] == (-1, -1) and m[1]["hover"] == "none",
                  "under a dialog the console opened, a drawn frame left no square hovered "
                  "(the Render-path ClearHover)", f"hover={m[1]['hover']} cell={m[1]['cell']}")
finally:
    drop()


# --- phase 50: a model loads under whichever extension is installed --------------
print("50 - a type's model loads as .gltf or .glb, an item wears its texture, and what "
      "cannot load is refused")
# The premise, read off the pool: each model is installed under the extension its
# type's loader does NOT prefer, and only that one (an item prefers .glb, a
# decoration .gltf) - so both loads and the check below fail on the old rule.
MODEL_FILE = re.compile(r"modelfile (\S+) '([^']*)' (\S+): (\S+) installed=(\d) loaded=(\d)$")
ITEM_LOOK = re.compile(r"modelfile (\S+) '([^']*)' set=(\S+) parts=(\d+) wears=(\d+) drawn=(\S+) "
                       r"previewed=(\S+)$")
PROBE = re.compile(r"textures: prop (\S+) tier=(\S+) albedo=(\S+)")
LEVELCHECK = re.compile(r"levelcheck RESULT=(\S+) .*\bmissing_models=(\d+)")
# What came of a `newasset ... installed`: phase 27's line (MADE is a name two
# phases use, so this one is its own).
CREATED = re.compile(r"newasset (\S+) '([^']*)' from (\S+): (\w+)(?: - (.*))?$")
SET = "cobblestone_wall"
# The decoration's model: a .glb-only one whose one shipped type (the amulet, in
# armor.cat) the run takes out of its scratch world first, so nothing else there
# opens the file - every item kind is built at world load, and an item on the
# decoration's model (rock.glb, which two items name) would have the file open
# before the place did anything.
DECO_MODEL = "moonstone_amulet"


def model_files(lines):
    """{(category, id): (file, installed, loaded)} from a section's `modelfile` lines."""
    return {(c, i): (f, inst == "1", ld == "1")
            for c, i, field, f, inst, ld in answers(lines, MODEL_FILE) if field == "model"}


def item_looks(lines):
    """[(set, parts, wears, drawn, previewed)] from a section's item `modelfile` lines."""
    return [(s, int(p), int(w), d, pv) for _, _, s, p, w, d, pv in answers(lines, ITEM_LOOK)]


def probed(lines):
    """[(tier, albedo)] of the set's `textures load` lines in a section."""
    return [(t, a) for s, t, a in answers(lines, PROBE) if s == SET]


def naming(catalogs, model):
    """Every block in a world's catalogs that could open `model`: one whose `model`
    names it, or whose id is it (a type with no `model` loads its id)."""
    out = []
    for name in sorted(os.listdir(catalogs)):
        if name.endswith(".cat"):
            text = io.open(os.path.join(catalogs, name), encoding="utf-8").read()
            out += [f"{name}:{b}" for b in block_ids(text)
                    if b == model or (cat_block(text, b) or {}).get("model") == model]
    return out


have = lambda f: os.path.isfile(os.path.join(MODELS, f))
check(have("lever_handle.gltf") and not have("lever_handle.glb") and have(DECO_MODEL + ".glb")
      and not have(DECO_MODEL + ".gltf"),
      "THE PREMISE: lever_handle is installed only as .gltf (where an item's loader prefers .glb), "
      f"{DECO_MODEL} only as .glb (where a decoration's prefers .gltf)")
en = lang_table("en")
nomodel = lambda name, field: say(en, "map.type.nomodel", name, field)
# `lang` and `quality` both save to settings.ini: the developer's copy goes back.
settings_before = io.open(SETTINGS, "rb").read() if os.path.isfile(SETTINGS) else None
fresh()
try:
    catalogs = os.path.join(PROJ, "catalog")
    drop_block(os.path.join(catalogs, "armor.cat"), DECO_MODEL)
    # eval_arena's levers' corner (batch 39) is two lever records, and a lever's
    # kind opens lever_handle at the level's load - the file only the weapon may
    # open here. This scratch copy loses them (the hidden niche stays, unrevealed).
    arena_path = os.path.join(PROJ, r"levels\eval_arena.ent")
    raw = io.open(arena_path, "rb").read().decode("utf-8")
    eol = "\r\n" if "\r\n" in raw else "\n"
    io.open(arena_path, "wb").write(eol.join(
        l for l in raw.split(eol) if not re.match(r"\s*button\s+lever\b", l)).encode("utf-8"))
    arena_ent = io.open(arena_path, encoding="utf-8").read()
    check(not naming(catalogs, DECO_MODEL) and not re.search(r"^\s*button\s+lever\b", arena_ent, re.M),
          f"...and in the scratch world no type names {DECO_MODEL} once the amulet is out (so only "
          "the decoration can open it), and eval_arena has no lever (whose handle is lever_handle)",
          str(naming(catalogs, DECO_MODEL)))
    log = run("modelfiles.eval")
    check(passed(log), "the first run ran clean (the place and every refusal as the script expects)")
    sec = console_sections(log)
    check("end" in sec, "...to its end")
    made = {tid: (key, src, outcome, why or "") for key, tid, src, outcome, why
            in answers(sec.get("create", []) + sec.get("refuse", []), CREATED)}
    check(made.get("et_gltf_weapon", ("",) * 4)[2] == "created"
          and made.get("et_glb_deco", ("",) * 4)[2] == "created",
          f"\"Use installed\" makes a weapon on lever_handle and a decoration on {DECO_MODEL}",
          str(made))
    deco_file = DECO_MODEL + ".glb"
    unplaced = model_files(sec.get("create", [])).get(("decorations", "et_glb_deco"))
    check(unplaced == (deco_file, True, False),
          f"before the place, the decoration's {deco_file} is installed and NOT yet opened",
          str(unplaced))
    files = model_files(sec.get("files", []))
    check(files.get(("weapons", "et_gltf_weapon"), ("",))[0] == "lever_handle.gltf"
          and files[("weapons", "et_gltf_weapon")][1],
          "the weapon's model resolves to lever_handle.gltf, installed (an item used to open .glb)",
          str(files.get(("weapons", "et_gltf_weapon"))))
    check(files.get(("decorations", "et_glb_deco")) == (deco_file, True, True),
          f"...and the place OPENED it (it used to open {DECO_MODEL}.gltf and abort)",
          str(files.get(("decorations", "et_glb_deco"))))
    looks, probe = item_looks(sec.get("files", [])), probed(sec.get("files", []))
    check(len(looks) == 1 and looks[0][:3] == (SET, 1, 1) and len(probe) == 1
          and looks[0][4] == probe[0][1],
          f"the weapon's one part wears its texture, {SET}: the details preview is handed the set's "
          "own albedo (an item's texture was never read: an imported one drew white)",
          f"looks {looks} | set {probe}")
    check(len(looks) == 1 and looks[0][3] == "undrawn",
          "...and drawn= says the headless run never drew it - it is a draw's record, not a "
          "material worked out for the readout", str(looks))
    bad = made.get("et_bad_deco", ("",) * 4)
    check(bad[2] == "refused" and bad[3] == nomodel("et_no_such_model", "model")
          and not warned("decorations", "et_bad_deco", "et_no_such_model", log),
          "the create dialog's FORM refuses a model installed as neither, saying why (Create "
          "never reached)", str(bad))
    refused = sec.get("refuse", [])
    check(f"typeset decorations 'et_glb_deco': model refused - {nomodel('et_no_such_model', 'model')}"
          in refused,
          "the type editor's Save (typeset) refuses a model installed as neither")
    check(f"typeset decorations 'et_glb_deco': model refused - {nomodel('et_glb_deco', 'model')}"
          in refused,
          "...and refuses taking the model away, since the id it falls back to is no file")
    want_note = nomodel("et_no_such_model", "model")
    check(f"typeset dialog: save refused - {want_note}" in refused
          and f"typeset dialog: open decorations 'et_glb_deco' - {want_note}" in refused,
          "the type editor's own Save, after the picker hands it such a model, refuses and stays "
          "open with the reason in its notice")
    decos = io.open(os.path.join(PROJ, r"catalog\decorations.cat"), encoding="utf-8").read()
    weapons = io.open(os.path.join(PROJ, r"catalog\weapons.cat"), encoding="utf-8").read()
    deco, weapon = cat_block(decos, "et_glb_deco") or {}, cat_block(weapons, "et_gltf_weapon") or {}
    check(deco.get("model") == DECO_MODEL and cat_block(decos, "et_bad_deco") is None
          and weapon.get("model") == "lever_handle" and weapon.get("texture") == SET,
          f"on disk: et_glb_deco still names {DECO_MODEL}, et_gltf_weapon lever_handle in " + SET
          + ", and the refused type was never written", f"{deco} | {weapon}")
    arena_map = io.open(os.path.join(PROJ, r"levels\eval_arena.map"), encoding="utf-8").read()
    check(re.search(r"^decoration et_glb_deco 5 5\b", arena_map, re.M) is not None,
          "the decoration is saved on eval_arena's 5,5")

    # THE RELOAD: a second process on the same world, WITH ITS WINDOW - drawn=
    # is a rendered frame's record, and a headless run renders none. Its load
    # builds every item kind - the weapon among them - and eval_arena's the
    # decoration.
    log = run("modelreload.eval", headless=False)
    check("crash: unattended" in log, "the windowed run is unattended (a fatal error exits)")
    check(passed(log), "the reload ran clean (the old rule aborted it at the world load)")
    sec = console_sections(log)
    check("end" in sec, "...to its end")
    files = model_files(sec.get("reload", []))
    check(files.get(("weapons", "et_gltf_weapon")) == ("lever_handle.gltf", True, True),
          "after the reload the weapon's lever_handle.gltf is installed and OPENED (no lever in "
          "eval_arena: its item kind opened it)", str(files.get(("weapons", "et_gltf_weapon"))))
    check(files.get(("decorations", "et_glb_deco")) == (deco_file, True, True),
          f"...and the decoration's {deco_file} OPENED, by eval_arena's load (no other type here "
          "names it)", str(files.get(("decorations", "et_glb_deco"))))
    said = editor_said(log).get("reload", [])
    on_square = say(en, "map.select.contents", 5, 5,
                    say(en, "map.joined", say(en, "map.select.floor"), say(en, "map.select.props.one")))
    check(on_square in said, "the decoration stands on 5,5, loaded with its level", str(said))
    lc = answers(sec.get("reload", []), LEVELCHECK)
    check(lc == [("PASS", "0")] and "levelcheck: missing model" not in log,
          "levelcheck passes the world with both: it resolves each as its loader does", str(lc))
    looks = item_looks(sec.get("reload", []))
    check(len(looks) == 1 and looks[0][:3] == (SET, 1, 1),
          "the reloaded weapon still wears its set on its one part", str(looks))
    check("drop et_gltf_weapon at 14,11: laid" in sec.get("reload", []),
          "the weapon is laid on the floor ahead of the party, where the scene draws it")
    # What the draws HANDED the part (DrawPart's stamp from the last rendered
    # frame: the floor item, its icon) and what the details preview is handed,
    # each against the set's own albedo at the tier in force. A quality swap
    # frees and reloads the set's maps: 1k, then 4k - and a draw handed a map
    # kept from before would read "stale", one handed none "none".
    for name, tier, size in (("drawn", None, None), ("low", "1k", "1024x1024"),
                             ("ultra", "4k", "4096x4096")):
        looks, probe = item_looks(sec.get(name, [])), probed(sec.get(name, []))
        want = probe[0][1] if len(probe) == 1 else None
        check(len(looks) == 1 and want is not None and (tier is None or probe[0] == (tier, size))
              and looks[0][3] == want and looks[0][4] == want,
              f"{'at the tier in force' if tier is None else f'after quality -> {name}'}, the "
              f"weapon's part is DRAWN and previewed with {SET}'s own "
              f"{'albedo' if tier is None else tier + ' albedo'}",
              f"looks {looks} | set {probe}")
    reloads = [int(n) for n in re.findall(r"Quality switched to .*?(\d+) prop set\(s\) reloaded", log)]
    check(any(n > 0 for n in reloads), "...and the swaps really reloaded the prop sets", str(reloads))
finally:
    drop()
    if settings_before is not None:
        io.open(SETTINGS, "wb").write(settings_before)
    elif os.path.isfile(SETTINGS):
        os.remove(SETTINGS)


# --- phase 51: the kind caches ---------------------------------------------------
print("51 - kind caches: a prop by its catalog, an item rebuilt in place, a rune by its kind")
PROPKIND = re.compile(r"propkind (\S+) '([^']*)' file=(\S+) set=(\S+) multi=\d authored=\d$")
DETAIL_ROW = re.compile(r"item details row (\S+) = (.*)$")
ICON = re.compile(r"itemicon (\S+): (glyph|tablet|icon|none)(?: (\S+))?$")
EFFECT_ICON = re.compile(r"itemicon effect (\S+): (glyph|tablet|icon|none)(?: (\S+))?$")
RENAMED = re.compile(r"typeset rename (\S+) '([^']*)': (done|refused)(?: - (.*))?$")
FLYING = re.compile(r"flooritems flying: (.+?) charge ")
ON_FLOOR = re.compile(r"flooritems (\d+),(\d+): (\S+) slot ")


def append_block(path, block_id, fields):
    """Appends an [id] block to a .cat file, in the file's own line ending."""
    raw = io.open(path, "rb").read().decode("utf-8")
    eol = "\r\n" if "\r\n" in raw else "\n"
    lines = ["", f"[{block_id}]"] + [f"{k} = {v}" for k, v in fields.items()]
    text = raw + ("" if raw.endswith(eol) else eol) + eol.join(lines) + eol
    io.open(path, "wb").write(text.encode("utf-8"))


def add_field(path, block_id, key, value):
    """Adds `key = value` as the first field of an existing [id] block, in the
    file's own line ending. False when the block is not there."""
    raw = io.open(path, "rb").read().decode("utf-8")
    eol = "\r\n" if "\r\n" in raw else "\n"
    head = f"[{block_id}]{eol}"
    if head not in raw:
        return False
    io.open(path, "wb").write(raw.replace(head, f"{head}{key} = {value}{eol}", 1).encode("utf-8"))
    return True


def detail_rows(lines):
    """[{row id: value}] - one dict per `itemdetails rows` in the section."""
    out, cur = [], None
    for l in lines:
        m = DETAIL_ROW.match(l)
        if m:
            if cur is None:
                cur = {}
                out.append(cur)
            cur[m.group(1)] = m.group(2)
        else:
            cur = None
    return out


def packs(lines):
    """[{member: [slot ids]}] - one per `inventory status` line in the section."""
    out = []
    for l in lines:
        if l.startswith("inventory: "):
            out.append({int(m): ids.split() for m, ids in re.findall(r"\| (\d+):([^|]*)", l)})
    return out


def tenths(v):
    """A number as the details dialog shows it (one decimal)."""
    return f"{float(v):.1f}"


en = lang_table("en")
related = lambda tid, cat: say(en, "newasset.err.related", tid, say(en, f"map.cat.{cat}"))
settings_before = io.open(SETTINGS, "rb").read() if os.path.isfile(SETTINGS) else None
fresh()
try:
    catalogs = os.path.join(PROJ, "catalog")
    decos_path = os.path.join(catalogs, "decorations.cat")
    decos = io.open(decos_path, encoding="utf-8").read()
    grate = cat_block(decos, "portcullis_grate")
    check(grate is not None and cat_block(decos, "portcullis") is None,
          "THE PREMISE: the shipped decoration is portcullis_grate, and no decoration is named "
          "portcullis (the door's id) any more", str(grate))
    # The decoration under the door's id, as every world shipped it until the
    # rename - written by hand, since the editor refuses it now.
    append_block(decos_path, "portcullis", grate or {})
    append_block(os.path.join(catalogs, "items.cat"), "et_kenaz",
                 {"name": "item.rune_air", "category": "rune", "weight": "0.5",
                  "holdable": "1", "symbol": "air"})
    # The effect icons: the Sowilo light's `icon = rune_light`, as every world
    # shipped it until the effect classes carried their own rune (the kit
    # renames that tablet away), and an item override on poison (a control).
    effects_path = os.path.join(catalogs, "effects.cat")
    effects = io.open(effects_path, encoding="utf-8").read()
    check(all("icon" not in (cat_block(effects, e) or {"icon": 1})
              for e in ("light", "stoneskin", "sight", "poison", "burn")),
          "THE PREMISE: no shipped effect names an icon item (a ward, Sight and a light wear "
          "their own rune)")
    check(add_field(effects_path, "light", "icon", "rune_light")
          and add_field(effects_path, "poison", "icon", "apple"),
          "the scratch effects.cat takes [light] icon = rune_light and [poison] icon = apple")
    weapons_before = io.open(os.path.join(catalogs, "weapons.cat"), encoding="utf-8").read()
    dagger_damage = (cat_block(weapons_before, "dagger") or {}).get("damage")
    log = run("kindcaches.eval")
    check(passed(log), "the script ran clean (each refusal it probes refused)")
    sec = console_sections(log)
    check("end" in sec, "...to its end")

    # THE WEAPON: its details read before and after a type-editor save made with
    # them open, and a dagger in the air across that save.
    thrown, saved, landed = (sec.get(s, []) for s in ("weapon", "weapon saved", "weapon landed"))
    rows = detail_rows(thrown) + detail_rows(landed)
    before = rows[0].get("damage") if rows else None
    after = rows[1].get("damage") if len(rows) > 1 else None
    check(dagger_damage is not None and before == tenths(dagger_damage),
          f"the dagger's details show its catalog damage ({dagger_damage}) before the save",
          str(rows))
    check(answers(thrown, FLYING) == [("dagger",)],
          "a dagger thrown in the frozen corridor is in the air before the save", str(thrown[-3:]))
    status = [l for l in saved if l.startswith("item details: ")]
    check(status[:1] and status[0].startswith("item details: closed"),
          "the save closed the open details dialog - its preview held the model the rebuild "
          "frees (it stayed open, drawing freed meshes)", str(status))
    check(answers(saved, FLYING) == [("dagger",)],
          "after the save the flight still reads `dagger` through the kind it holds - rebuilt IN "
          "PLACE (an erased and rebuilt kind leaves it holding freed memory)", str(saved))
    floor = answers(landed, ON_FLOOR)
    check(not answers(landed, FLYING) and [f[2] for f in floor] == ["dagger"],
          "...and it lands as a dagger", str(landed[:4]))
    check(after == "9.5" and after != before,
          "...and the reopened details show 9.5 after `typeset weapons dagger damage 9.5`, with "
          "no reload (the save never rebuilt an item's kind)", str(rows))

    # THE RUNE under an id that is not rune_<symbol>.
    rune = sec.get("rune", [])
    icons = answers(rune, ICON)
    check(icons[:2] == [("et_kenaz", "tablet", "air"), ("et_kenaz", "glyph", "air")],
          "et_kenaz draws as the air rune: its carved tablet in a socket, the glyph where a "
          "control asks (by the id's `rune_` spelling it drew nothing)", str(icons))
    slots = [p.get(2, []) for p in packs(rune)]
    shown = [l.endswith("memorize=1") for l in rune if l.startswith("item details: open")]
    pressed = [l for l in rune if l.startswith("item details: memorized")
               or l.startswith("item details: no Memorize")]
    check(len(slots) == 2 and slots[0][3:4] == ["et_kenaz"],
          "given to Maren, it lies in her pack's fourth slot (what the script opens)", str(slots))
    check(shown[:1] == [True] and pressed[:1] == ["item details: memorized"]
          and len(slots) == 2 and slots[1][3:4] == ["-"],
          "its details offer Memorize, and pressing it learns the rune and spends the tablet "
          "(Memorize asked the id's spelling)", f"{shown} {pressed} {slots}")
    check(len(shown) == 2 and not shown[1],
          "...a second et_kenaz then offers none: Maren knows air, by the kind's symbol", str(shown))

    # THE STARTER KIT names the world's tablets.
    kit = sec.get("kit", [])
    check(answers(kit, RENAMED)[:1] == [("items", "rune_light", "done", None)],
          "rune_light is renamed et_sowilo", str(answers(kit, RENAMED)))
    kpacks = packs(kit)
    casters = [kpacks[0].get(m, []) for m in (2, 3)] if kpacks else []
    check(len(casters) == 2 and all("et_sowilo" in p and "rune_light" not in p for p in casters),
          "a new game's two casters each carry et_sowilo, not rune_light (the kit named "
          "rune_<symbol> ids outright)", str(casters))
    check("pack += et_sowilo" in kit and answers(kit, ICON)[:1] == [("et_sowilo", "tablet", "light")],
          "`rune light` gives et_sowilo, and it draws as the light tablet", str(kit[-8:]))
    effect_icons = {e: (look, rune) for e, look, rune in answers(kit, EFFECT_ICON)}
    check(effect_icons.get("light") == ("glyph", "light"),
          "the Sowilo light's effect icon still wears the Sowilo glyph, its `icon = rune_light` "
          "naming a tablet the rename took away (it fell to the tinted square)", str(effect_icons))
    check(effect_icons.get("stoneskin") == ("glyph", "protect"),
          "a ward naming no icon wears its own rune, Protect", str(effect_icons))
    check(effect_icons.get("poison") == ("icon", None) and effect_icons.get("burn") == ("none", None),
          "THE CONTROLS: poison's `icon = apple` wins as the apple's icon, and burn - no item, no "
          "rune - is the tinted square", str(effect_icons))

    # THE PORTCULLISES: a door and a decoration of one id, side by side.
    doors = io.open(os.path.join(catalogs, "doors.cat"), encoding="utf-8").read()
    door = cat_block(doors, "portcullis") or {}
    kinds = {(c, i): (f, s) for c, i, f, s in answers(sec.get("portcullis", []), PROPKIND)}
    deco_kind, door_kind = kinds.get(("decorations", "portcullis")), kinds.get(("doors", "portcullis"))
    stem = lambda f: f.rsplit(".", 1)[0] if f else None
    check(deco_kind is not None and door_kind is not None,
          "the door portcullis and the decoration portcullis are TWO kinds, one per catalog "
          "(one cache by bare id gave the second the first's)", str(sorted(kinds)))
    check(deco_kind is not None and stem(deco_kind[0]) == (grate or {}).get("model")
          and deco_kind[1] == (grate or {}).get("texture"),
          f"the decoration wears its own model and set ({(grate or {}).get('model')}, "
          f"{(grate or {}).get('texture')})", str(deco_kind))
    check(door_kind is not None and stem(door_kind[0]) == door.get("model")
          and door_kind[1] == door.get("texture"),
          f"...and the door its own ({door.get('model')}, {door.get('texture')})", str(door_kind))

    # RELATED CATALOGS: create, duplicate and rename refuse an id one of them holds.
    rel = sec.get("related", [])
    made = {tid: (key, outcome, why or "") for key, tid, src, outcome, why in answers(rel, CREATED)}
    for tid, key, cat, what in (("wooden_door", "decorations", "doors", "a decoration named for a door"),
                                ("torch", "weapons", "items", "a weapon named for an item"),
                                ("portcullis_grate", "doors", "decorations",
                                 "a door DUPLICATED under a decoration's id")):
        got = made.get(tid, ("",) * 3)
        check(got[:2] == (key, "refused") and got[2] == related(tid, cat),
              f"create refuses {what} ({tid}), saying so in its own words", str(got))
    check(made.get("et_free_weapon", ("",) * 3)[1] == "created",
          "THE CONTROL: a weapon under a free id is created", str(made.get("et_free_weapon")))
    renames = {(key, tid): (outcome, why) for key, tid, outcome, why in answers(rel, RENAMED)}
    check(renames.get(("doors", "wooden_door")) == ("refused", related("portcullis_grate", "decorations")),
          "a door renamed onto a decoration's id is refused", str(renames))
    check(renames.get(("weapons", "dagger")) == ("refused", related("apple", "items")),
          "a weapon renamed onto an item's id is refused", str(renames))
    check(renames.get(("weapons", "khukri"), ("",))[0] == "done",
          "THE CONTROL: a weapon renamed to a free id is renamed", str(renames))
    disk = {n: io.open(os.path.join(catalogs, n + ".cat"), encoding="utf-8").read()
            for n in ("decorations", "weapons", "doors")}
    check(cat_block(disk["decorations"], "wooden_door") is None
          and cat_block(disk["weapons"], "torch") is None
          and cat_block(disk["doors"], "portcullis_grate") is None
          and cat_block(disk["doors"], "wooden_door") is not None
          and cat_block(disk["weapons"], "dagger") is not None
          and cat_block(disk["weapons"], "et_free_weapon") is not None
          and cat_block(disk["weapons"], "et_free_khukri") is not None
          and cat_block(disk["weapons"], "khukri") is None,
          "on disk: nothing refused was written, and both controls were")
finally:
    drop()
    if settings_before is not None:
        io.open(SETTINGS, "wb").write(settings_before)
    elif os.path.isfile(SETTINGS):
        os.remove(SETTINGS)


# --- phase 52: a surface's Save is written once its bake lands; an import says its flip
print("52 - a surface type's re-baking Save is written only once its bake lands; an import "
      "always says its green flip")
# typebake.eval's header says what each step stands for. Every set here is
# et_-named, so cleanup() takes what the bakes and imports write into the pool.
TB_WALLS = ("\r\n[et_tb_wall]\r\ndisplay = Bake test\r\ntexture = cobblestone_wall\r\n"
            "\r\n[et_tb_other]\r\ndisplay = Bake test 2\r\ntexture = cobblestone_wall\r\n")
TB_LOCKED = os.path.join(MODELS, "worn_et_tb_locked_low.gltf")
TB_FRESH = {f"worn_et_tb_fresh_{t}.gltf" for t in ("low", "med", "high")}
TB_DIALOG = {f"worn_et_tb_dialog_{t}.gltf" for t in ("low", "med", "high")}
TB_GREEN = 100   # the import folders' normal maps; a flip leaves 155
TB_IMPORTS = {"dx": ("glossy_slab_normal_dx.png", False),   # a "gl" inside a word
              "gl": ("glossy_slab_normal_ogl.png", True)}   # the GL token at the end
TB_FIELDS = re.compile(r"typeset field (\S+) = ?(.*)$")


def solid_png(path, size, rgb):
    """A small RGB PNG of one colour - what each import folder's maps are."""
    rows = b"".join(b"\x00" + bytes(rgb) * size for _ in range(size))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def bakes_of(log, needle):
    """Every `AssetBaker: <command>` line naming `needle`."""
    return [l.split("AssetBaker: ", 1)[1] for l in log.splitlines() if "AssetBaker: " in l and needle in l]


fresh()
try:
    walls_path = os.path.join(PROJ, r"catalog\walls.cat")
    io.open(walls_path, "a", encoding="utf-8", newline="").write(TB_WALLS)
    other_before = cat_block(io.open(walls_path, encoding="utf-8").read(), "et_tb_other")
    # The set whose bake cannot write: its low tier stands read-only.
    with open(TB_LOCKED, "wb") as fh:
        fh.write(b"editortest phase 52: read-only, so the bake of et_tb_locked fails\n")
    os.chmod(TB_LOCKED, stat.S_IREAD)
    words = {}
    for tag, (normal, _) in TB_IMPORTS.items():
        folder = os.path.join(IMPORT_SRC, "et_tb_" + tag)
        os.makedirs(folder, exist_ok=True)
        solid_png(os.path.join(folder, f"et_tb_{tag}_albedo.png"), 32, (140, 120, 100))
        solid_png(os.path.join(folder, normal), 32, (128, TB_GREEN, 255))
        words["ET_IMPORT_" + tag.upper()] = folder.replace("\\", "/")
    log = run("typebake.eval", words=words)
    check(passed(log), "the script ran clean (its two probes refused, nothing else)")
    sec = console_sections(log)
    check("end" in sec, "the script ran to its end")

    def texture_in(name):
        got = {m.group(1): m.group(2).strip() for m in map(TB_FIELDS.match, sec.get(name, [])) if m}
        return got.get("texture")

    # 1. `typeset` of a texture bakes, and the catalog waits for the bake.
    check(texture_in("before") == "cobblestone_wall", "THE CONTROL: et_tb_wall names cobblestone_wall",
          str(sec.get("before")))
    fresh_lines = sec.get("fresh", [])
    check(any(l.startswith("typeset walls 'et_tb_wall': texture = et_tb_fresh - baking") for l in fresh_lines)
          and any(l.startswith("bake: running - walls 'et_tb_fresh', step 1 (a restyle)") for l in fresh_lines),
          "`typeset walls et_tb_wall texture et_tb_fresh` starts a bake of et_tb_fresh "
          "(it wrote the field with no bake at all)", str(fresh_lines[:4]))
    check(texture_in("fresh") == "cobblestone_wall",
          "...and while it runs the catalog still names cobblestone_wall (the Save waits for it)",
          f"mid-bake texture: {texture_in('fresh')!r}")
    check("refused, as expected: 'typeset walls et_tb_other texture et_tb_second'" in log
          and "A bake is already running ('et_tb_fresh')" in log and not bakes_of(log, "et_tb_second"),
          "a second re-baking save meanwhile is refused, naming the running bake, and starts nothing")
    landed = sec.get("fresh landed", [])
    check("bake: idle" in landed and texture_in("fresh landed") == "et_tb_fresh"
          and "type editor: walls 'et_tb_wall' saved - its worn meshes baked" in log,
          "landed clean: et_tb_wall names et_tb_fresh", f"{landed[:2]} texture {texture_in('fresh landed')!r}")
    check(TB_FRESH <= set(own_worn()), "...whose three worn tiers the bake wrote",
          str(sorted(TB_FRESH - set(own_worn()))))
    # 2. A failed bake writes nothing - through `typeset` and the dialog's Save.
    check(any("wornblock wall et_tb_locked " in b for b in bakes_of(log, "et_tb_locked"))
          and len(bakes_of(log, "et_tb_locked")) == 2,
          "et_tb_other given et_tb_locked bakes it - by `typeset` and by the dialog's Save",
          " | ".join(bakes_of(log, "et_tb_locked")))
    check(log.count("type editor: walls 'et_tb_other' not saved - 'et_tb_other' was not saved: "
                    "its worn meshes did not bake (AssetBaker exit 1") == 2,
          "...the baker exits 1 both times (its low tier is read-only), and neither save is written")
    check(texture_in("locked landed") == "cobblestone_wall",
          "after the `typeset` bake failed, et_tb_other still names cobblestone_wall",
          f"texture {texture_in('locked landed')!r}")
    shown = [l for l in sec.get("dialog landed", []) if l.startswith("typeset dialog: open walls 'et_tb_other'")]
    check(bool(shown) and "was not saved: its worn meshes did not bake" in shown[0],
          "the dialog whose Save failed stays open, saying why (it used to close, the field written)",
          str(sec.get("dialog landed", [])[:3]))
    check(texture_in("dialog landed") == "cobblestone_wall",
          "...and reopened, et_tb_other still names cobblestone_wall", f"texture {texture_in('dialog landed')!r}")
    # 2b. The dialog's Save that lands clean: frozen while it bakes, closed once
    # it lands, the field written (LandRestyleBake's dialog branch - the
    # `typeset` landing above has no dialog to close).
    dfresh = sec.get("dialog fresh", [])
    check(any(l.startswith("bake: running - walls 'et_tb_dialog', step 1 (a restyle)") for l in dfresh)
          and len(bakes_of(log, "et_tb_dialog")) == 1,
          "the dialog's Save of et_tb_wall given et_tb_dialog starts one bake of et_tb_dialog", str(dfresh[:6]))
    status = [l for l in dfresh if l.startswith("typeset dialog: ")]
    check(len(status) >= 2 and status[-1].startswith("typeset dialog: open walls 'et_tb_wall'"),
          "...and while it runs the dialog stays up (frozen behind its baking notice)", str(status))
    dlanded = sec.get("dialog fresh landed", [])
    dstatus = [l for l in dlanded if l.startswith("typeset dialog: ")]
    check("bake: idle" in dlanded and dstatus == ["typeset dialog: closed"]
          and log.count("type editor: walls 'et_tb_wall' saved - its worn meshes baked") == 2,
          "landed clean, the dialog that made the Save CLOSES and the Save is written",
          f"{dlanded[:3]} status {dstatus}")
    check(texture_in("dialog fresh reopened") == "et_tb_dialog",
          "...reopened, et_tb_wall names et_tb_dialog", f"texture {texture_in('dialog fresh reopened')!r}")
    check(TB_DIALOG <= set(own_worn()), "...whose three worn tiers the bake wrote",
          str(sorted(TB_DIALOG - set(own_worn()))))
    # 3. A set painted as another kind is refused before any bake.
    check("refused, as expected: 'typeset walls et_tb_other texture floor_cobble'" in log
          and "'floor_cobble' is a floor set" in log and not bakes_of(log, "floor_cobble"),
          "a shipped FLOOR set as a wall's texture is refused before any bake, by its record")
    # 4. A field that bakes nothing is written at once.
    plain = sec.get("plain", [])
    check("typeset walls 'et_tb_wall': height_scale = 0.03" in plain and "bake: idle" in plain,
          "height_scale, which bakes nothing, is written at once with no bake", str(plain))
    walls_after = io.open(walls_path, encoding="utf-8").read()
    wall = cat_block(walls_after, "et_tb_wall") or {}
    check(wall.get("texture") == "et_tb_dialog" and wall.get("height_scale") == "0.03",
          "on disk: et_tb_wall names et_tb_dialog (the dialog's landed Save) with height_scale 0.03",
          str(wall))
    check(cat_block(walls_after, "et_tb_other") == other_before,
          "on disk: et_tb_other is exactly as written before the run - no failed or refused save "
          "reached it", f"{cat_block(walls_after, 'et_tb_other')} vs {other_before}")
    # 5. An import always says its flip (C393), and the pool shows it.
    from BakerWriteTest import read_png
    imports = cat_blocks(io.open(os.path.join(PROJ, r"catalog\imports.cat"), "rb").read())
    for tag, (normal, gl) in TB_IMPORTS.items():
        tid = "et_tb_" + tag
        sent = [b for b in bakes_of(log, f" {tid}_2k") if " import " in b]
        flag = "--flip-green" if gl else "--no-flip-green"
        check(len(sent) == 1 and sent[0].rstrip().endswith(f" {tid}_2k {flag}"),
              f"the dialog's import of {normal} sends {flag}"
              + ("" if gl else " (a 'gl' inside a word, which the old test read as OpenGL)"),
              " | ".join(sent))
        rec = imports.get(tid + "_2k") or {}
        check(rec.get("flip_green") == ("1" if gl else "0"),
              f"...and its imports.cat record says flip_green = {'1' if gl else '0'}", str(rec))
        packed = os.path.join(TEXTURES, tid + "_2k_n.png")
        greens = sorted({px[1] for px in read_png(packed)}) if os.path.isfile(packed) else []
        want = 255 - TB_GREEN if gl else TB_GREEN
        check(greens == [want], f"...and the packed {tid}_2k_n.png's green is {want}", f"greens {greens}")
    check(f"Created type 'et_tb_dx' in walls" in log and f"Created type 'et_tb_gl' in walls" in log,
          "both imports landed as wall types")
finally:
    drop()
    if os.path.exists(TB_LOCKED):
        os.chmod(TB_LOCKED, stat.S_IREAD | stat.S_IWRITE)


# --- phase 55: a lever's reveal swaps in pre-built walls ---------------------------
print("55 - a lever's niche reveal swaps in pre-built walls, a fresh bake's every time")
LOOKS = re.compile(r"niche looks chunk (\d+,\d+) names=(\S+) live=(\d+) held=(\d+)$")
REBUILT = re.compile(r"niche looks: presses rebuilt in play (\d+)$")
WALLS = re.compile(r"geomhash \S+ walls=([0-9a-f]+) ")
LAYOUT = re.compile(r"geomlayout \S+ fresh=\S+ live=\S+ (\w+)$")


def listings(lines):
    """Each `niche looks` listing in `lines`, in order: its chunk rows as
    (chunk, names, live, held)."""
    out, cur = [], []
    for l in lines:
        m = LOOKS.match(l)
        if m:
            cur.append(m.groups())
        elif cur:
            out.append(cur)
            cur = []
    return out + [cur] if cur else out


def niche_state(lines, niche_at="niche 3,22 s: "):
    """A section's last niche reading, `niche looks` lines (the last listing),
    and geomhash's wall print and layout verdict."""
    niche = [l.split(": ", 1)[1] for l in lines if l.startswith(niche_at)]
    found = listings(lines)
    looks = found[-1] if found else []
    walls = [m.group(1) for m in map(WALLS.match, lines) if m]
    layout = [m.group(1) for m in map(LAYOUT.match, lines) if m]
    return (niche[-1] if niche else None, looks, walls[-1] if walls else None,
            layout[-1] if layout else None)


def lives(looks, name="arena_secret"):
    """{chunk: live} of the chunks a looks listing names `name` in, or {} if any
    of them is reached by another name too or holds other than one look."""
    mine = [l for l in looks if name in l[1].split(",")]
    if any(n != name or h != "1" for _, n, _, h in mine):
        return {}
    return {c: int(v) for c, _, v, _ in mine}


def add_pillar(levels):
    """The pillar nichelooks.eval ends on, written into the scratch copy of
    eval_arena (no suite's ground has one): a pillar at 8,7, a hidden niche
    `arena_pillar` on its south face opening onto 8,8, and a lever at 8,6 on its
    north face. The niche's west neighbour 7,8 lies in chunk 1,2 (x 4..7, z
    8..11), where no floor square borders rock - a chunk with no walls at all."""
    path = os.path.join(levels, "eval_arena.map")
    text = io.open(path, encoding="utf-8", newline="").read()
    eol = "\r\n" if "\r\n" in text else "\n"
    lines = text.splitlines(keepends=True)
    grid = [i for i, l in enumerate(lines) if l.startswith("#")]
    body = lines[grid[7]].rstrip("\r\n")
    if body[7:10] != "...":
        raise RuntimeError(f"eval_arena row 7 is not open round 8,7: {body!r}")
    lines[grid[7]] = body[:8] + "#" + body[9:] + lines[grid[7]][len(body):]
    if not lines[-1].endswith("\n"):
        lines[-1] += eol
    lines.append("niche niche 8 8 north name=arena_pillar hidden=1" + eol)
    io.open(path, "w", encoding="utf-8", newline="").write("".join(lines))
    path = os.path.join(levels, "eval_arena.ent")
    text = io.open(path, encoding="utf-8", newline="").read()
    if text and not text.endswith("\n"):
        text += eol
    io.open(path, "w", encoding="utf-8", newline="").write(
        text + "button lever 8 6 south target=arena_pillar" + eol)


fresh()
try:
    add_pillar(os.path.join(PROJ, "levels"))
    log = run("nichelooks.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    read = {k: niche_state(sec.get(k, [])) for k in
            ("loaded", "the lever wired to nothing", "the reveal", "shut again",
             "opened by hand, shut by the lever", "the wall repainted under it, then the reveal",
             "a new game", "revealed for the save", "a load")}
    shut0, looks0, walls0, lay0 = read["loaded"]
    check(shut0 == "shut" and lives(looks0) == {"0,5": 0, "1,5": 0} and lay0 == "match",
          "loaded: the niche is shut, both chunks it reaches hold one look, the walls are the map's",
          str(read["loaded"]))
    check(lives(looks0, "arena_pillar") == {"2,2": 0},
          "...and of the pillar's niche's two chunks only 2,2 holds a look: 1,2 has no walls in "
          "any state", str(looks0))
    rebuilt = {k: [int(m.group(1)) for m in map(REBUILT.match, sec.get(k, [])) if m] for k in sec}
    check(all(rebuilt.get(k) for k in ("loaded", "the pillar")) and
          all(n == 0 for v in rebuilt.values() for n in v),
          "no press rebuilt walls in play, in any section", str(rebuilt))
    for name, want, live, same in (("the lever wired to nothing", "shut", 0, True),
                                   ("the reveal", "open", 1, False),
                                   ("shut again", "shut", 0, True),
                                   ("opened by hand, shut by the lever", "shut", 1, True),
                                   ("the wall repainted under it, then the reveal", "open", 1, False)):
        state, looks, walls, layout = read[name]
        check(state == want and lives(looks) == {"0,5": live, "1,5": live} and layout == "match"
              and (walls == walls0) == same,
              f"{name}: the niche {want}, look {live} on show in both chunks, the walls a fresh "
              f"bake's{' and as loaded' if same else ' and moved'}", str(read[name]))
    # Between the hand and the lever: the hand's rebuild built the looks again
    # from the open state, so the lever's press is the FIRST flip of those.
    hand = listings(sec.get("opened by hand, shut by the lever", []))
    check(hand and lives(hand[0]) == {"0,5": 0, "1,5": 0},
          "a niche opened by hand rebuilds its chunks and their looks from that state", str(hand))
    paint = listings(sec.get("the wall repainted under it, then the reveal", []))
    check(paint and lives(paint[0]) == {"0,5": 0, "1,5": 0},
          "so does repainting the wall it is cut into", str(paint))
    # A REAL new game (`newgame`, StartNewGame): the live map still has the niche
    # open, so it is ResetForNewGame's batched re-stamp that shuts the walls.
    state, looks, walls, layout = read["a new game"]
    check(state == "shut" and lives(looks) == {"0,5": 0, "1,5": 0} and layout == "match",
          "a new game shuts it and starts the looks over (its batched re-stamp)",
          str(read["a new game"]))
    # A load of a save made with it open: ResetForNewGame shuts it, then
    # ApplyActiveSnapshot opens it again - each re-stamp batched, and the looks
    # built again from the open state the load leaves.
    state, _, walls_r, layout = read["revealed for the save"]
    check(state == "open" and layout == "match", "revealed again for the save",
          str(read["revealed for the save"]))
    state, looks, walls, layout = read["a load"]
    check(state == "open" and lives(looks) == {"0,5": 0, "1,5": 0} and layout == "match"
          and walls == walls_r,
          "a load restores it open, the walls a fresh bake's and as saved, the looks built "
          "again from that state", str(read["a load"]))
    state, looks, walls, layout = niche_state(sec.get("the pillar", []), "niche 8,8 n: ")
    check(state == "open" and lives(looks, "arena_pillar") == {"2,2": 1} and layout == "match"
          and walls != read["a load"][2],
          "the pillar's niche: revealed by its lever with the look flipped in 2,2, the walls a "
          "fresh bake's - and (above) nothing rebuilt for 1,2, which has no walls",
          str((state, looks, walls, layout)))
    check("end" in sec, "the script ran to its end")
finally:
    drop()


# --- phase 60: the door inspector's Open on a leaf that cannot shut ---------------
print("60 - the door inspector's Open: a wrecked leaf stays open, and none shuts on a monster")
DOORI = re.compile(r"editor inspector: door (\d+),(\d+) open=([01]) live=([01-]) broken=([01-]) tab -?\d+$")
DOORROW = re.compile(r"\s+\d+ \S+ @ (\d+),(\d+) (open|shut) authored=(open|shut)( broken)?(?: name=\S+)?$")


def door_inspectors(lines):
    """Each door inspector status in `lines`: (cell, box ticked, leaf open, wrecked)."""
    out = []
    for l in lines:
        m = DOORI.match(l)
        if m:
            out.append(((int(m.group(1)), int(m.group(2))), m.group(3) == "1", m.group(4) == "1",
                        m.group(5) == "1"))
    return out


def door_rows(lines, cell):
    """Each `doors` row for the door on `cell`: (state in play, authored, wrecked)."""
    out = []
    for l in lines:
        m = DOORROW.match(l)
        if m and (int(m.group(1)), int(m.group(2))) == cell:
            out.append((m.group(3), m.group(4), bool(m.group(5))))
    return out


def hud_lines(lines):
    """The HUD message log lines a `messages` readout printed."""
    return [l[4:] for l in lines if l.startswith("  | ")]


fresh()
try:
    log = run("doorinspector.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    check("end" in sec, "the script ran to its end")

    # A SMASHED DOOR: wrecked, it stands open; Open ticked then unticked.
    sm = sec.get("smashed", [])
    rows = door_rows(sm, (12, 12))
    st = door_inspectors(sm)
    wrecked = rows[:1] == [("open", "shut", True)]
    check(wrecked, "the smashed door stands open, wrecked, its record still shut", str(rows))
    check(wrecked and [s[1] for s in st] == [False, True, False] and all(s[0] == (12, 12) for s in st),
          "its inspector's Open, ticked then unticked, takes the authored state both ways", str(st))
    check(wrecked and len(st) == 3 and st[2][2] and st[2][3] and rows[1:] == [("open", "shut", True)],
          "...and the wrecked leaf STAYS OPEN unticked (it used to shut, and then would not open)",
          f"inspector {st} doors {rows}")
    said = hud_lines(sm)
    check(any(m.startswith("The doorway is wrecked") for m in said), "...saying why", str(said))
    ent = io.open(os.path.join(PROJ, r"levels\eval_arena.ent"), encoding="utf-8").read()
    rec = next((l.split() for l in ent.splitlines() if l.split()[:4] == ["door", "wooden_door", "12", "12"]),
               None)
    check(rec is not None and "open=1" not in rec,
          "the inspector's Save wrote the authored state: the record holds no open=1",
          " ".join(rec) if rec else "no door record at 12,12")

    # A MONSTER IN THE DOORWAY: the chooser's door row, Open ticked then unticked.
    mo = sec.get("monster", [])
    alive = [l for l in mo if re.match(r"\s+skeleton @ 15,12\s+hp [\d.]+\s", l) and "(dead)" not in l]
    picked = "editor inspect: inspector" in mo
    rows = door_rows(mo, (15, 12))
    st = door_inspectors(mo)
    check(bool(alive) and picked and st[:1] and st[0][0] == (15, 12) and st[0][2],
          "a skeleton stands in an open doorway, and the chooser's door row opened its inspector",
          f"skeleton {alive} picked {picked} inspector {st[:1]}")
    # REFUSED, the record's close too: the box ticks itself again and the door
    # stays authored open. The leaf alone held is not enough - the Save writes
    # the skeleton where it stands, so a record that took the close would shut
    # the door on it at the next load.
    check(len(st) == 3 and [s[1] for s in st] == [False, True, True] and st[2][2]
          and rows[:1] == [("open", "open", False)],
          "unticking Open is refused: the leaf stays open on the skeleton (it used to close on it) "
          "and so does the authored state, the box ticked again",
          f"inspector {st} doors {rows}")
    check("Something is blocking the doorway." in hud_lines(mo), "...saying why", str(hud_lines(mo)))
    # Esc reverts toward the shut it was placed as: refused the same way. Then the
    # level, saved by `savemap` - the inspector's Save would apply the ticked box
    # once more first and so mend a record the untick had left shut.
    check(len(rows) == 2 and rows[1] == ("open", "open", False),
          "...and Esc's revert to the shut it was placed as is refused too", f"doors {rows}")
    ent = io.open(os.path.join(PROJ, r"levels\eval_arena.ent"), encoding="utf-8").read()
    recs = [l.split() for l in ent.splitlines() if l.split()[2:4] == ["15", "12"]]
    door = next((r for r in recs if r[:2] == ["door", "wooden_door"]), None)
    held = [r for r in recs if r[:2] == ["monster", "skeleton"]]
    check(any(l.startswith("saved levels: ") and "eval_arena" in l for l in mo)
          and door is not None and "open=1" in door and len(held) == 1,
          "the level saved holds the skeleton in the doorway beside a door record of open=1, "
          "so the next load cannot shut the door on it",
          f"records on 15,12: {[' '.join(r) for r in recs]}")

    # THE CONTROL: nobody in the doorway, the same untick shuts the leaf.
    em = sec.get("empty", [])
    st = door_inspectors(em)
    rows = door_rows(em, (18, 12))
    check(len(st) == 3 and [s[1] for s in st] == [False, True, False] and st[1][2] and not st[2][2]
          and rows == [("shut", "shut", False)],
          "THE CONTROL: with the doorway empty, the same untick shuts the leaf",
          f"inspector {st} doors {rows}")
finally:
    drop()


# --- phase 61: a placed monster's leash anchor is its own square -----------------
print("61 - a monster made in the editor or by `spawn` is leashed from its own square")
LEASHROW = re.compile(r"\s+(\S+) @ (\d+),(\d+)  id (-?\d+)  spawn (\d+),(\d+)  anchor (-?\d+),(-?\d+)"
                      r"  range ([\d.]+)$")
LEASHWARN = re.compile(r"\s+warn crypt2 @(\d+),(\d+) map\.check\.leashrock (\S+)$")
# What the judge plants in crypt2.ent: open floor on row 1 and row 6, each
# monster leashed from a square of its own (the checker reads the files).
LEASH_PLANTS = {(3, 1): "0,0", (8, 1): "99,99", (5, 6): "6,6"}


def leash_rows(lines):
    """Each `leash` row: {cell, id, spawn, anchor, range}."""
    out = []
    for l in lines:
        m = LEASHROW.match(l)
        if m:
            g = [int(v) for v in m.groups()[1:8]]
            out.append({"cell": (g[0], g[1]), "id": g[2], "spawn": (g[3], g[4]),
                        "anchor": (g[5], g[6]), "range": float(m.group(9))})
    return out


def placed(lines):
    """The placed monsters' `leash` rows (id -1: no record), keyed by spawn."""
    return {r["spawn"]: r for r in leash_rows(lines) if r["id"] < 0}


fresh()
try:
    # Appended in the file's own line ending, on a line of their own.
    c2 = os.path.join(PROJ, r"levels\crypt2.ent")
    text = io.open(c2, encoding="utf-8", newline="").read()
    eol = "\r\n" if "\r\n" in text else "\n"
    text += ("" if text.endswith("\n") else eol) + "".join(
        f"monster skeleton {x} {z} south leash=2 leashfrom={cell}{eol}"
        for (x, z), cell in LEASH_PLANTS.items())
    io.open(c2, "w", encoding="utf-8", newline="").write(text)
    log = run("leashanchor.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    check("end" in sec, "the script ran to its end")

    # THE CHECKER: a leash anchored on rock or off the map, and not on floor.
    warned = {(int(m.group(1)), int(m.group(2))): m.group(3)
              for m in (LEASHWARN.match(l) for l in sec.get("checked", [])) if m}
    check(warned == {(3, 1): "0,0", (8, 1): "99,99"},
          "validate flags the leash anchored on rock (0,0) and off the map (99,99), "
          "not the one on open floor (6,6)",
          f"crypt2's map.check.leashrock findings, by monster square: {warned or 'none'}")

    # MADE: each anchored on its own square - and the level's own skeletons (the
    # loader's path, right before) are the control the readout can show it.
    made = placed(sec.get("made", []))
    authored = [r for r in leash_rows(sec.get("made", [])) if r["id"] >= 0]
    check(len(authored) == 6 and all(r["anchor"] == r["spawn"] for r in authored),
          "the level's six skeletons are anchored on their spawns (the loader's path)",
          str(authored))
    check(set(made) == {(16, 18), (20, 18)}
          and all(r["anchor"] == r["spawn"] and r["cell"] == r["spawn"] for r in made.values()),
          "the brush's skeleton (16,18) and `spawn`'s (20,18) are each anchored on its own square "
          "(they read 0,0)", str(made))

    # MOVED: the move tool takes the anchor with the spawn.
    moved = placed(sec.get("moved", []))
    check(set(moved) == {(14, 18), (20, 18)} and moved.get((14, 18), {}).get("anchor") == (14, 18),
          "the placed skeleton moved to 14,18 takes its anchor along (it stayed on 0,0)", str(moved))

    # RETURNED and LOADED: rebuilt from the held state and from the save.
    for name, what in (("returned", "after crypt1 and back"), ("loaded", "after the save is loaded")):
        rows = placed(sec.get(name, []))
        check(set(rows) == {(14, 18), (20, 18)}
              and all(r["anchor"] == r["spawn"] for r in rows.values()),
              f"{what}, both placed skeletons are anchored on their own squares", str(rows))

    # LEASHED, SHOVED, HOME: the leash measured from 14,18, and the walk home to it.
    leashed = placed(sec.get("leashed", []))
    check(len(leash_rows(sec.get("leashed", []))) == 1
          and leashed.get((14, 18), {}).get("range") == 1.0,
          "the level's skeletons and the spawned one erased; the placed one's inspector set "
          "its leash to 1", str(leash_rows(sec.get("leashed", []))))
    shoved = placed(sec.get("shoved", [])).get((14, 18), {})
    check(shoved.get("cell") == (11, 18),
          "the gust shoved it three squares west, to 11,18 (toward 0,0)", str(shoved))
    home = placed(sec.get("home", [])).get((14, 18), {})
    check(shoved.get("cell") == (11, 18) and home.get("cell") == (14, 18),
          "...and it walked back EAST to its square, 14,18 (it went on toward the corner)",
          f"shoved {shoved} home {home}")
    unaware = [l for l in sec.get("home", []) if re.match(r"\s+skeleton @ \d+,\d+\s+hp ", l)]
    check(len(unaware) == 1 and unaware[0].endswith("aware=0"),
          "...never having noticed the party - the leash took it home, not a chase", str(unaware))

    # THE FILE: savemap (and the inspector's Save before it) wrote no leashfrom=.
    ent = io.open(os.path.join(PROJ, r"levels\eval_arena.ent"), encoding="utf-8").read()
    mons = [l for l in ent.splitlines() if l.startswith("monster ")]
    check(any(l.split()[:4] == ["monster", "skeleton", "14", "18"] and "leash=1" in l.split()
              for l in mons) and not any("leashfrom=" in l for l in mons),
          "eval_arena.ent holds the placed skeleton at 14,18 with leash=1 and no leashfrom= "
          "(it wrote leashfrom=0,0)", str(mons))

    # AUTHORED: the making sets the spawn as the default now, so a record's own
    # leashfrom= must still be read AFTER it - the open-floor plant's 6,6, not its
    # spawn (every anchor checked above is a default).
    plant = [r for r in leash_rows(sec.get("authored", [])) if r["spawn"] == (5, 6) and r["id"] >= 0]
    check(len(plant) == 1 and plant[0]["anchor"] == (6, 6),
          "on crypt2 the skeleton planted at 5,6 with leashfrom=6,6 is anchored on 6,6, as authored "
          "(the default must not win over the record)", str(plant))
finally:
    drop()

# --- the real tree: LAST, after every phase --------------------------------------
print("the real tree: dungeon-demo and the library as the run found them")
cleanup()
real.check(check)

print()
print("PASS" if failures == 0 else f"FAIL - {failures} check(s) failed")
sys.exit(0 if failures == 0 else 1)
