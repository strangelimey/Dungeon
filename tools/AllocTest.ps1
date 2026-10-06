# ============================================================================
# tools\AllocTest.ps1 - the steady-state allocation regression test.
#
# ARCHITECTURE.md says a steady-state frame allocates nothing on the heap. This
# is the run that checks it: launch the game, start a new game, let the world
# settle, then have the in-game guard measure a window of genuinely steady
# frames (Core/AllocTrack + the `alloctest` dev command). Exit code 0 = PASS.
#
#   .\tools\AllocTest.ps1                    # debug build, 10-second window
#   .\tools\AllocTest.ps1 -Seconds 30
#   .\tools\AllocTest.ps1 -Wounded           # the REGENERATING steady state
#   .\tools\AllocTest.ps1 -Melee             # a monster swinging at the party
#   .\tools\AllocTest.ps1 -Rest              # resting, a monster behind a shut door
#   .\tools\AllocTest.ps1 -RestReach         # resting, a frozen monster with a way through
#   .\tools\AllocTest.ps1 -Cast              # a bolt in flight + an open spellbook
#   .\tools\AllocTest.ps1 -Impact            # bolts landing, expiring, a blast, a crate alight
#   .\tools\AllocTest.ps1 -Burst             # burst bolts on the party, a ward, a gust's repel
#   .\tools\AllocTest.ps1 -Swing             # the party swinging, and a severe fumble dropping a torch
#   .\tools\AllocTest.ps1 -Hand              # the hand spells: light, douse, flare, fill, pebble
#   .\tools\AllocTest.ps1 -Light             # the Sowilo lights: cast, stacked, grown, a flare's dazzle
#   .\tools\AllocTest.ps1 -Pause             # Esc to the pause menu and back
#   .\tools\AllocTest.ps1 -Exit              # Help clicked, an exit stair's "Leave?" answered No, a pit fall
#   .\tools\AllocTest.ps1 -Sheet             # the sheet: hover, tabs, item dialog
#   .\tools\AllocTest.ps1 -Panels            # drag and resize the floating HUD
#   .\tools\AllocTest.ps1 -Minimal [-Sheet]  # any mode, under the party-card layout
#   .\tools\AllocTest.ps1 -Items             # pack -> cursor -> floor -> cursor -> pack; the item pose + picks
#   .\tools\AllocTest.ps1 -Packs             # swap a 4-slot and an 8-slot bag
#   .\tools\AllocTest.ps1 -Throw             # lift a rock, throw it at a wall, again
#   .\tools\AllocTest.ps1 -Throw -ThrowItem torch_lit   # ...a lit torch (its light and flame)
#   .\tools\AllocTest.ps1 -Wear moonstone_amulet       # any mode with a worn light on member 0
#   .\tools\AllocTest.ps1 -Walk              # key turns: the party AND the pad's stones
#   .\tools\AllocTest.ps1 -Lights            # 64 test lights; then the floor glows, the ceiling, the shadow cache
#   .\tools\AllocTest.ps1 -Lights -ShadowSelfTest   # ...those checks handed a stale cache
#   .\tools\AllocTest.ps1 -OnHitTypo         # swings with a typo'd on_hit: its warning
#   .\tools\AllocTest.ps1 -PartyPage         # the party creation page, idle, on the title
#   .\tools\AllocTest.ps1 -Config release    # needs -DDN_TRACK_ALLOCS=ON
#
# THE RULE HAS NO EXCEPTIONS: an allocation in a settled frame is a bug, and
# that includes frames where something HAPPENED. A bump message, a level line, a
# monster's swing - since docs/message-allocation.md printing any of them
# allocates nothing, so the guard carries no notion of an event and no list of
# things it forgives. (It used to: "allocation proportional to events is not
# what the rule forbids" was written here as policy, and it was a
# rationalisation of a defect - loc::Tr copying text the table already owned.)
#
# The party stands still in the default run because that is the BASELINE - the
# least a steady state can be - not because events are excused. A still party
# simply keeps the event paths OUT of the window, and a path outside the window
# passes whether it allocates or not. That is what the modes below are for:
# each one puts an event path INSIDE the window (-Wounded the regeneration tick,
# -Melee a monster's swing and its narration, -Cast a bolt in flight and an open
# spellbook, -Impact bolts launching, striking a FRESH monster, expiring and
# bursting, and a crate the blasts leave alight burning down). Anything that
# allocates is named with a full call stack in dungeon.log, once per unique
# stack.
#
# WHY -Wounded EXISTS, and it is the same trap this project keeps meeting: a
# FRESH party is at full health, and regeneration only runs BELOW maximum - so
# the default run walks straight past the whole resource tick and reports a
# confident PASS for code it never executed (docs/health-and-healing.md). The
# regenerating party is a real steady state (it is what walking away from a
# fight looks like) and it is where the per-frame skill-XP award lives, so it
# needs its own run. Absent and correct report identically; give the check
# something to be wrong about.
#
# It wounds with a SHORT bleed and lets it expire before measuring, so the
# window holds the regeneration tick and nothing else, and it sets the resource
# practices high first - not to make the numbers big, but so a level-up (which
# re-derives the maxima) cannot land mid-window and make the verdict depend on
# how near a practice happened to be to its next level. That keeps the run
# repeatable; it is NOT an excuse. A level-up that allocated would be a bug like
# any other - this run is just not the one that looks for it. At level 20 a
# practice needs 41 more XP, which ten seconds of regeneration cannot reach.
#
# -Melee IS THE SAME TRAP AGAIN. No monster reaches a party standing at the
# start inside the window, so MonsterAttack - its name lookup and its narration
# - went unmeasured, and every swing allocated three times (a concatenated
# "monster." key, loc::Tr's copy, the string local) while every run passed.
# This spawns a weakened monster beside the party HELD (`freeze hold`: it
# neither acts nor notices the party until alloctest's first armed frame lets it
# go), so its first notice, the first formation pass and its first blow all land
# INSIDE the window, and refuses a PASS unless the tally shows the party was
# actually hit there. Swings are events, and since the message path stopped
# allocating (docs/message-allocation.md) events get no exemption. NOR DOES A
# FIRST BLOW (code-review batch 21): this mode used to wait for one to land as
# "warm-up", and that hid TrainDefense building its stat lists on it and the
# formation list growing for the first aware monster - a first time every
# session pays in its first fight, which is not warm-up.
#
# -Rest IS WHERE THE AI THINKS IN A GUARDED FRAME (code-review C62). The bucket
# workers' searches are not checked - they run on their own threads - but REST
# forces lockstep, and lockstep runs those very searches INLINE, on the main
# thread, in frames the guard arms. The case that ends worst is ordinary play:
# resting behind a shut door while a monster that has seen the party waits on
# the other side. Its search fails every frame, and the search allocated a
# std::queue (a deque) every time. This carves a corridor (`arena corridor 11`),
# shuts a door two squares east of the party, stands a skeleton two beyond it
# facing them, and waits until it has NOTICED them (`hudbars`). It then wounds
# the party and puts one member at 0 health (`setpool`): unconscious, they wait
# out the stabilize clock, which a monster in aggro keeps resetting, so this rest
# cannot end by itself - a party merely wounded is healed in about a second at
# 60x, and the window would measure a party standing about.
#   THE REST STARTS INSIDE THE WINDOW, from the HUD's Rest button (`rest
# button` says where), as a player starts one. Typed `rest on`, the first
# inline thinks - the ones that would grow anything left unsized - ran in the
# console's frames, which the guard never arms, and a 120-frame warm-up then
# passed before the window opened; deleting the level-load pre-size of the
# inline brain still read PASS.
#   -RestReach is the other half: no door, and the monster FROZEN (`freeze on`),
# so every think finds a path to the party that it never walks - a search's
# OUTPUT, a plan's path, which a door that always fails never makes. (Unfrozen it
# would walk up, swing and end the rest.)
#   Both refuse a PASS unless the rest outlasted the window, the monster was
# still engaged (and so still searching) at its end, and `lockstep stats` shows
# the inline compute ran - and, for -RestReach, that its thinks found paths.
#
# AND IN EVERY MODE: the AI's snapshot and walkability-grid pools are filled at
# level load to as many buffers as can ever be in use at once (C66). They used to
# grow lazily, which could first land minutes in, in whichever guarded frame the
# thread scheduler chose, so a window caught it only by luck. A pool that grows
# now logs `AI pool grew:` once, and that line fails the run wherever it landed.
# So does any growth of what lockstep's inline compute uses - its brain's search
# scratch, its plan batches and their paths - which is sized at level load too.
#
# -Cast, AND AGAIN (2026-09-28). No run ever cast a spell or opened a book, so a
# bolt copied its payload - four std::string effect ids, which the debug CRT
# allocates for - on every frame of its flight, and an open spellbook rebuilt a
# vector of rune slots twice a frame, while every run passed. This freezes the
# world (timescale 0, so the bolt can neither land nor fizzle), has a caster
# learn fire and cast it, and opens that member's book (`book`), so the window
# holds a projectile in flight AND a book being redrawn. The LAUNCH itself is
# an event in the console's own frame, which the guard never arms; it was
# checked by hand from the hand use menu and the book's Cast button.
#
# -Impact, BECAUSE -Cast CANNOT LAND ANYTHING. -Cast freezes the world, so its
# bolt never arrives: the strike (fx::Deal, the burn a flame leaves on the
# monster, the hit lines, threat), the expiry of a bolt that flies past, the
# impact sparks and an area blast all stayed outside every window. This runs
# the world. It goes to eval_arena (an open room, so no wall of the showcase
# level decides what gets measured), stands the party three squares from a
# FROZEN, toughened monster (`freeze on`: it stands in the line of fire and
# never walks up to swing, which is -Melee's job), and hands the casting to the
# harness (`autocast`): two flame casters in OPPOSITE lanes - one bolt strikes,
# the other flies past the monster and expires at the end of its reach - and a
# Fire Burst, which detonates on contact or where it stops. In an open room
# the blast's force is spent before it reaches back three squares. (A bolt's
# `range` is in METRES, so five squares out, the first try, was out of reach
# of everything and measured nothing but expiries.) A launch now happens in a world
# frame, so the window holds launches AND landings. It waits until a bolt has hit
# and one has expired (first times for the process), then measures, and refuses
# a PASS unless the tally shows a bolt hit, an expiry and a blast INSIDE the
# window. THE BLAST GETS NO WARM-UP: the Fire Burst caster joins the rotation
# only once it is held, so the first detonation of the process lands in the
# window. Counting it as warm-up hid the blast list growing by ~6 KB inside a
# guarded frame (code-review C49) - every session pays that in its first fight.
#
# AND IT MEASURES A FRESH MONSTER. Warm-up may only absorb a first time for the
# PROCESS; a first time for a MONSTER is paid again by every monster in play.
# Warming up and measuring on one target passed while every monster's first
# burn allocated twice (its effects list growing from empty, its flame plume
# made on ignition). So the rotation is held (`autocast hold`), the party
# steps aside to a new, untouched target, and alloctest's first ARMED frame
# releases the barrage and restarts the tally; the verdict frame logs that
# tally, which is what the refusal reads.
#
# -Burst IS THE OTHER SIDE OF -Impact: SHOTS AT THE PARTY (code-review C1, C18).
# -Impact has the party cast at a monster, so nothing ever reaches the party:
# a burst bolt going off on contact with it (ResolveMonsterProjectileHit's area
# branch, then the blast's PARTY branch - every member hurt, narrated, left
# burning), a Wind Ward turning one there, and a gust's repel - weakening a shot,
# flinging one back, or leaving one nothing so it falls and is erased from the
# flight list - all stayed outside every window, and could allocate and pass.
# Nothing in the console's frames is measured, so the harness fires them from a
# world frame: `autocast bolt` launches the skel_magus's own burst bolt at the
# party from the square ahead, as `bolt` does, and an entry naming `repel` meets
# it on the same tick with a repel of an exact power, as `castsvc repel` does -
# exact because only a gust of the shot's own strength leaves it nothing. In an
# emptied eval_arena the rotation (held, so the window's first armed frame
# starts it) is: a plain bolt, one a repel of half its strength weakens, one its
# equal spends and one twice it flings back, a cast every 2 s - sparse so that
# the party lives through the window (Tilo has 24 hp; a burst and its burn take
# about 13 of them). Both members of the bolts' lane carry a Wind Ward of ONE
# charge, so the first bolt to connect is turned whoever it picks and a later
# one goes off. NO WARM-UP, the -Melee rule: a session's first burst on the
# party is paid in its first fight with a magus. The one thing grown before the
# window is the party bar's effect strip, a known defect scheduled elsewhere
# (code-review C219, batch 41 - see the setup). It refuses a PASS unless the
# window's tally counts a burst on the party, a ward turn, and a repel that
# weakened, one that turned and one that spent.
#
# -Swing IS THE PARTY'S OWN SWING (code-review C10). -Melee is a monster
# swinging at the party; no mode made the PARTY swing, so PartyAttack and the
# fumble consequences never ran in a window - and a severe fumble's drop copied
# the held item's id into a fresh std::string (the debug CRT allocates for any)
# in the swing's frame, and lost a torch's charge, while every run passed. This
# empties eval_arena, stands a frozen, toughened skeleton ahead of the party and
# turns the party's own swinging on HELD (`autoattack hold`: alloctest's first
# armed frame releases it, as it does a held autocast), so the session's FIRST
# party swing is inside the window - a first swing is paid by every session, so
# it is not warm-up. The die is LOADED for Sera's lit torch (`fumble severe 1 1`,
# the torch burnt to 300 s first): its swing fumbles severely and the default
# severe table drops it, so the drop - and a part-burnt torch then burning on the
# floor - happen in the window beside the ordinary blows of the other hands. It
# refuses a PASS unless the window's tally counts a swing besides the fumble, a
# severe fumble, and a held item it put on the floor (`severefumbles=`,
# `fumbledrops=`).
#
# -Pause IS THE OTHER HALF OF THE RULE: WHICH FRAMES IT COVERS. The guard judges
# a frame on the state at its top, so the frame Esc is pressed in starts as
# Playing and ends as Paused - and it rebuilds the pause menu (a widget tree, and
# ListSaves parsing every save for the Load entry) inside a frame armed as
# steady. That logged ~5000 allocations on every Esc (2026-09-28). A frame that
# LEAVES the guarded states is a transition, and Game::Update now disarms it; this
# run presses Esc during the window, resumes, and repeats, so a regression of
# that rule lands inside the window. It refuses a PASS unless the verdict line
# counts at least one such transition (`transitions=`), since a swallowed Esc
# would otherwise report exactly like a clean run. What it does NOT cover is the
# frames just after a resume: they fall inside the guard's 120-frame warm-up, so
# a resume path that allocates there passes (code-review C216; a -Cold mode that
# arms them is phase 2's).
#
# -Exit IS -Pause's CASE WITH NO STATE CHANGE, AND A PIT (code-review C210,
# C217). An exit stair asks "Leave?" while the state stays Playing, so the
# transition rule never saw it: the frame that built the prompt, the frames under
# it and the frame that answered No were all armed. A prompt over play is now a
# pause - its opening frame disarmed at the end of Update, no frame armed while it
# is up - and the verdict line counts the openings (`prompts=`). A pit's step
# latched its fall by constructing a fresh transition, a level name included, and
# the plunge after it plays out in Playing; the fall is a KEPT transition now,
# assigned into reserved room. And the log's Help button excused its key-names
# line as "reporting" though the player reads it; the names are read when the
# keys are bound now. NONE of it ran in any window. This goes to crypt1, not
# eval_arena: the harness's ground has no exit stair, and a pit needs a level
# below it, which eval_arena (the last level, a dungeon of one) does not have.
# crypt1's own exit stands one square south of its start; `stairadd pit 7 6`
# puts a pit one square north, over crypt2's 7,6. Inside the window: a click on
# Help, a step back onto the exit, N for No, a step forward to the start and one
# more into the pit - the step, the plunge and (after the load) the rest of the
# window on crypt2. It refuses a PASS unless the verdict counts a prompt
# (`prompts=`), a Help press (`helps=`) and a pit step (`falls=`), and the party
# stands on crypt2 at 7,6 (a Yes would have left the dungeon). The game counts a
# press and a step only in a MEASURED frame - armed to the end of its Update,
# inside the window - because the harness can only time its sends: a click sent
# a few seconds after `alloctest` lands in the guard's warm-up on a slow enough
# frame rate, and a press counted since launch passed it with the Help line
# unchecked. A send that misses its armed frames reads UNMEASURED, never PASS.
#
# -Sheet IS THE CHARACTER SHEET'S TURN (docs/ui-updates-plan.md). The sheet is a
# guarded state, and since ui-updates it does things every frame the pointer
# moves: the status bar names whatever is under it, on every tab. A right-click
# opens the item details dialog - IN an armed frame, which is why that dialog is
# built once rather than per open - and it spins a 3D model every frame it is
# up; a middle-click opens the use menu. None of it ran in any window before.
# This opens the sheet, warms the dialog up once through `itemdetails` (a first
# open bakes its fonts, which is a first time for the process, not a steady
# cost), then during the window hovers two slots, pages through all five tabs,
# right-clicks an item, lets the model turn, Escs, middle-clicks a rune and Escs
# again - three times. It refuses a PASS unless `itemdetails status` counts
# opens made during the window, since a missed click reports exactly like a
# clean run.
#
# -Panels IS THE FLOATING HUD'S TURN (docs/ui-panels-plan.md P3a). Every HUD
# panel moves and resizes under the mouse now, inside armed frames: a drag
# re-places the panel every frame it is held, a corner grip rescales it (a new
# font size the first time, which is a first time, not a steady cost - so the
# warm-up below drags once before the window), and the release SAVES
# settings.ini, which excuses itself (GameSettings::Save). This resets the
# layout, warms up, then during the window drags the Movement dock by its title
# and back and pulls the Hands dock's corner grip - three times - with the
# party inventory WINDOW (P3b) open the whole while. It refuses a PASS unless
# `hudpanel list` afterwards shows all three (the move dock saved off its
# default, the hands dock off scale 1, the inventory shown), since a missed
# drag or a window that never opened reports exactly like a clean run. Every
# drag holds Ctrl (a panel only arranges under it). After the window it also
# checks the arranging rules: a Ctrl+click on a panel's reset button puts every
# panel home, a drag WITHOUT Ctrl moves nothing, and a drag ending 4 px short of
# another panel's edge lands on it - any of them failing is a FAIL.
# THE TRAY (ui-updates Phase 8) rides the same window: the Movement dock is
# minimized by its Ctrl button and restored by its tray button, and the counts
# `hudpanel list` prints must show the trip landed.
# -Items IS MOVING AN ITEM, which no run did (found by accident 2026-09-30, when
# a -Panels click on the ui-panels branch landed on an inventory slot and a later
# one on the floor). Two defects, both logged with call stacks: every pick, put
# and swap COPIED the cursor's id into a fresh std::string (the debug CRT
# allocates for any string it constructs, short or not), and the first time a
# kind of item reached the floor its kind was BUILT - a rune's first drop loaded
# its PBR set, 246 allocations and 2 MB in one guarded frame. The cursor now
# swaps strings with the slot (Game/Inventory.h HeldItem) and every item kind is
# built at load (DungeonWorld::PreloadItemKinds).
# This goes to eval_arena (open floor ahead, like -Impact), freezes it, puts two
# runes in the pack of a member with room, and opens the party inventory window from the
# console (`inventory`). Each cycle: pick the rune out of its slot, click the
# floor (drop), click it again (lift), and put it back - the window is
# non-modal, so it stays open throughout. The floor point is computed from the
# camera (70 degree lens, eye 1.55 m up) to land in the FAR quarter of the
# square ahead, so the lift - which samples the ray at the item's own height,
# nearer the party - still lands in the same quarter. ONE rune runs a warm-up
# cycle first, which also checks the coordinates (`inventory status`); the
# window then moves the OTHER rune, so its FIRST drop is inside the window - a
# first time for a KIND is paid again by every kind a player drops, so it is
# not warm-up (the -Impact lesson). It refuses a PASS unless the window's tally
# counts at least two drops and two lifts, which also puts the put-back between
# them inside the window.
# BEFORE that game, -Items runs tools\EvalScripts\itempose.eval headless
# (Test-ItemPose, code-review C180 / C359 / C258): a rune in a shut wall niche
# throws no floor glow and in an open one glows in the pocket over it, and
# `pickprobe` shoots every click target - floor items across a quarter at their
# drawn box, a niche item, a door's hand-hold, a wall torch - from where it is
# DRAWN, through the real click tests. A failure there is the result PICKS,
# whatever the window said.
#
# -Packs IS -Items' KNOWN-LEFT CASE: equipping a bag with more slots than the
# one it replaces. A pack's slots were a std::vector, so a bigger bag grew it,
# and even growth inside its capacity constructed a std::string per new slot
# (the debug CRT allocates for each). Pack slots are now a fixed-capacity list
# whose strings exist from the start (Game/Inventory.h PackSlots). This puts an
# ammo pouch (8 slots) in an empty pack-row square and a herb pouch (4) on the
# cursor, both through the sheet's own clicks, warms up one swap pair, then
# clicks that square twice per cycle inside the window: herb in (8 -> 4), ammo
# back (4 -> 8). It refuses a PASS unless `sheet status` counts two equips made
# during the window.
#
# -All IS THE PARTY WINDOW (more-ui-updates Phase 5): the sheet's "All" opens a
# card per member on the sheet's tab, each card the sheet's own code. Its four
# cards are built and warmed with the HUD, so opening it - a click in a guarded
# frame - must add nothing. This opens the sheet, reads where "All", the tab
# stones and member 0's portrait are (`sheet status`, `inventory stone`,
# `hudpanel list`), runs one cycle as a warm-up, then cycles inside the window:
# All, every tab with a hover over each card, Esc, the portrait (the sheet
# again). It refuses a PASS unless `inventory status` counts two opens inside.
#
# -OnHitTypo IS A WARNING INSIDE A GUARDED FRAME (code-review C215). log:: did
# not excuse its own formatting, so every reporter had to remember to - and a
# weapon whose on_hit names no effect (a typo in weapons.cat) warned on every
# landed blow and FAILED the guard for it, or aborted under `allocguard strict`.
# The excuse now lives inside log::Write and its templates. This puts a frozen,
# toughened skeleton beside the party, puts clubs in the front rank's hands
# (a club's severe fumble drops nothing - see the setup) and gives the club a
# proc naming 'brun' (`onhit`, in memory - weapons.cat is not touched), lets the
# party swing until the warning shows (a warm-up: the party's FIRST swing is not
# what this mode measures), then HOLDS the swinging until alloctest's first
# armed frame releases it. It refuses a PASS unless the warning was logged
# inside the window.
#
# -Glass IS THE TRANSPARENT QUEUE (transparency Phase 1). A see-through draw is
# not issued but QUEUED, then sorted and drawn after the opaque scene - and no
# other mode ever has glass on screen, so none of that would be measured. This
# places a glass kind (-GlassCategory / -GlassKind, by default a filled flask,
# so the liquid's clip plane and the far-wall / liquid / near-wall ordering are
# measured too) in eval_arena one square ahead of the party and measures with
# it in view. It refuses a PASS unless `glass` counts
# a frame that drew glass for (nearly) every armed frame of the window.
#
# -PartyPage IS THE ONE MODE OFF THE GUARDED STATES (code-review C112). The rule
# covers play, not the menus, which build as a matter of course - but the party
# creation page reformatted all its labels every frame it stood idle, about
# twenty allocations a frame for text nothing had changed, and no guard could
# see it. This starts NO game: on the title screen it opens the page by its dev
# twin (`partypage open`), then throws an AllocTest-ONLY switch, `allocguard
# partypage on` (off by default, never saved), under which Game::GuardedState
# counts the idle page, and measures with nothing touched. An edit on the page
# rebuilds its tree, so the window holds the idle page and nothing else. It
# refuses a PASS unless the page is still open afterwards, and it combines with
# no other mode (they all need a game) - only -SelfTest, whose allocpoke must
# FAIL it like any other.
#
# Every step is driven by what the log actually says rather than by sleeps, so
# a slow cold-cache load stretches the wait instead of failing the run.
#
# ASCII ONLY, deliberately: PowerShell 5.1 reads a BOM-less .ps1 as ANSI, so a
# stray em-dash in a comment is a parse error, not a cosmetic issue.
# ============================================================================
[CmdletBinding()]
param(
	[ValidateSet('debug', 'release')][string]$Config = 'debug',
	[int]$Seconds = 10,
	[int]$LoadTimeoutSec = 240,
	# Measures a WOUNDED party instead of a fresh one. See the note above: a
	# full-health party never runs the regeneration path at all.
	[switch]$Wounded,
	# Measures a party IN MELEE: a monster beside it swinging through the whole
	# window. See the note above - a party nobody attacks never runs the swing.
	[switch]$Melee,
	[string]$MeleeMonster = 'skeleton',
	# Scales the spawned monster's hp AND damage (the `spawn` 5th argument), so
	# it keeps swinging for the whole window without wiping the party.
	[double]$MeleeStrength = 0.3,
	# Measures a party RESTING beside a monster it cannot reach: the AI thinking
	# on the main thread, in guarded frames. See the note above.
	[switch]$Rest,
	# -Rest with no door and the monster frozen, so its every think finds a path
	# (implies -Rest). See the note above.
	[switch]$RestReach,
	[string]$RestMonster = 'skeleton',
	# Measures a bolt IN FLIGHT and an OPEN SPELLBOOK. See the note above.
	[switch]$Cast,
	# The caster: Maren, a rear-rank caster, by default.
	[int]$CastMember = 2,
	# Measures bolts LANDING: impacts, expiries and an area blast, with the
	# world running. See the note above.
	[switch]$Impact,
	# MEDIUM on purpose: it stands in one quarter of its square, so exactly one
	# of the two flame lanes strikes it and the other flies past. A Large body
	# (the plain skeleton) fills the square, both lanes hit, and nothing ever
	# expires - the first run that tried it saw 100 strikes and 0 expiries.
	[string]$ImpactMonster = 'skel_swarm',
	# The target's hp scale (`spawn`'s 5th argument): it must outlive the
	# warm-up AND the window under a bolt every fraction of a second.
	[double]$ImpactStrength = 400,
	# Seconds between casts, round-robin over the rotation below.
	[double]$ImpactEvery = 0.4,
	# Measures SHOTS AT THE PARTY: a burst bolt going off on it, a ward turning
	# one, a gust's repel weakening, turning and spending them. See the note above.
	[switch]$Burst,
	# Measures the PARTY swinging, and a severe fumble dropping a part-burnt torch
	# (code-review C10). See the note above.
	[switch]$Swing,
	# Casts the four hand spells at a wall torch inside the window (spell-updates
	# Phase 8). See the note at the setup.
	[switch]$Hand,
	# Casts the Sowilo LIGHT spells inside the window (lighting-updates Phase 6):
	# a light, another school's, an Ingwaz one and a Hagalaz flare at a mummy.
	[switch]$Light,
	# Pauses (Esc) and resumes inside the window. See the note above.
	[switch]$Pause,
	# Clicks the log's Help button, steps onto crypt1's exit stair and answers
	# its "Leave?" No, then falls down a pit, inside the window. See above.
	[switch]$Exit,
	# Works the character sheet inside the window. See the note above.
	[switch]$Sheet,
	# Opens the PARTY WINDOW from the sheet's "All" and works every tab of it
	# inside the window (more-ui-updates Phase 5). See the note above.
	[switch]$All,
	# Drags and resizes the floating HUD panels inside the window. See above.
	[switch]$Panels,
	# Runs whichever mode under the Minimal HUD layout (one card per member,
	# docs/ui-panels-plan.md P4), and puts Standard back afterwards.
	[switch]$Minimal,
	# Turns the party by KEY inside the window - a full circle each way - so the
	# movement pad presses its cut stones (more-ui-updates: a key move presses
	# the matching stone, via Party::ActCount). Refuses a PASS unless the
	# verdict's moves= counts them.
	[switch]$Walk,
	# Runs any mode under a LIGHT LOAD: 64 test lights over the level
	# (`lightstress`, lighting-updates Phase 3), so the light budget's cull,
	# ranking and fades - and the tile binning - run inside the window. With
	# -Walk the turns sweep lights in and out of view. Refuses a PASS unless
	# the load was placed. After the window it checks the CANDIDATE CEILING (a
	# full list ahead of the fires, survived) and the SHADOW CACHE (see the notes
	# at those steps): a door opened beside a still party re-renders slot 0, and
	# a carried Firelight's cube keeps up with a walk. Before the game it runs
	# floorglow.eval: an enchanted blade's floor glow is its element's colour.
	[switch]$Lights,
	# With -Lights: MUTATES the shadow cache (`shadows ignore both` - the change
	# notes dropped, a wandering light's moves not counted, the rules before
	# code-review C178 / C187) and passes only if BOTH shadow checks then FAIL:
	# the proof each can see a stale cube.
	[switch]$ShadowSelfTest,
	# Moves an item pack -> floor -> pack inside the window; before the game,
	# itempose.eval checks the item pose and the click picks. See the note above.
	[switch]$Items,
	# The warm-up item and the measured one: two different kinds, the second
	# never dropped before the window opens.
	[string]$WarmItem = 'rune_air',
	[string]$MeasureItem = 'rune_water',
	# Swaps a small and a big bag in the pack row inside the window. See above.
	[switch]$Packs,
	# Lifts a rock off the floor and throws it at a wall, round and round,
	# inside the window (ui-updates Phase 10). See the note at the setup.
	[switch]$Throw,
	# What -Throw throws. `torch_lit` (lighting-updates Phase 4) makes the round
	# trip a LIT torch's: carried on the cursor, a light and a flame in flight,
	# and a floor torch burning where it lands - all inside the window.
	[string]$ThrowItem = 'rock',
	# Member 0 WEARS this item for the whole run (lighting-updates Phase 5): an
	# item with a `light` (moonstone_amulet) is then a worn light every frame of
	# the window. Refuses to run if the wear was refused or no worn light shows.
	[string]$Wear = '',
	# Stands the party facing a GLASS decoration for the whole window, so the
	# transparent queue (transparency Phase 1) queues, sorts and flushes in armed
	# frames. See the note above.
	[switch]$Glass,
	[string]$GlassCategory = 'items',
	[string]$GlassKind = 'potion_health_greater', # glass AND a liquid (Phase 3)
	# Swings CLUBS whose on_hit names no effect, inside the window (a club, not a
	# dagger: a blade's severe fumble drops it, which is C212). See the note above.
	[switch]$OnHitTypo,
	# Starts with a CREATED party instead of the default four: a `newparty` spec
	# (party creation, docs/party-creation-plan.md phase 2), e.g.
	# 'premade=0 | premade=1 | premade=2' for three. Any mode runs under it; the
	# member loops below walk only the members it builds.
	[string]$Party = '',
	# Measures the party creation page standing IDLE on the title screen, with
	# no game started. See the note above.
	[switch]$PartyPage,
	# Checks the CHECKER: makes the game allocate ONCE, on the window's first
	# armed frame (`allocpoke once`), and passes only if the run comes back FAIL
	# AND dungeon.log names that allocation's call site - the first armed frame
	# after a disarm captured no stacks until code-review C214.
	[switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
if ($RestReach) { $Rest = $true } # -RestReach is -Rest with the way left open
# -Items spends about four armed seconds a round trip and needs two whole ones
# inside the window, so its default window is longer. So does -Impact since
# batch 21: its burst caster joins only when the window opens (so the first
# detonation is inside it), and a brazier must then break AND go out inside the
# window - in 10 s that happened only on an idle machine (503 frames under load:
# the brazier broke just after the window and the run refused its PASS).
# -Exit's window has to outlast its steps, the prompt's unarmed frames and the
# warm-up after it, and the plunge - then the level load to crypt2 - so it is
# longer too.
if (($Items -or $Throw -or $All -or $Impact -or $Exit) -and -not $PSBoundParameters.ContainsKey('Seconds')) { $Seconds = 20 }
if ($ShadowSelfTest -and -not $Lights) { throw '-ShadowSelfTest mutates the shadow checks, which only -Lights runs' }
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Config\bin"

# Muted for the whole run, restored however it ends (tools\HarnessAudio.ps1).
. (Join-Path $PSScriptRoot 'HarnessAudio.ps1')
if (-not (Test-HarnessMuted $bin)) { exit (Invoke-Muted $bin $PSCommandPath $PSBoundParameters) }
# Launch, input and log waits: the one shared copy (tools\HarnessGame.ps1).
. (Join-Path $PSScriptRoot 'HarnessGame.ps1')
$HarnessCharMs = 40

$exe = Join-Path $bin 'Dungeon.exe'
$log = Join-Path $bin 'dungeon.log'

# How many members the run plays: the default four, or one per `|`-separated
# member of -Party.
$memberCount = if ($Party) { @($Party -split '\|').Count } else { 4 }
if ($memberCount -lt 1 -or $memberCount -gt 4) { throw "-Party names $memberCount members; a party has 1 to 4" }

# -PartyPage starts no game, and every other mode needs one.
if ($PartyPage) {
	$withGame = @('Wounded', 'Melee', 'Cast', 'Impact', 'Hand', 'Light', 'Pause', 'Exit', 'Sheet', 'All',
		'Panels', 'Minimal', 'Walk', 'Lights', 'Items', 'Packs', 'Throw', 'Glass', 'Party', 'Wear') |
		Where-Object { $PSBoundParameters.ContainsKey($_) }
	if ($withGame) { throw "-PartyPage runs on the title screen with no game; it does not combine with -$($withGame -join ', -')" }
}

if (-not (Test-Path $exe)) { throw "no build at $exe - run build.cmd $Config first" }
Assert-ExeCurrent $exe
Assert-NotRunning $exe

# A mouse message at client pixel (x, y): WM_MOUSEMOVE first, so the game's
# pointer is where the button lands, then the down/up pair (none for a hover).
function Send-Mouse([int]$x, [int]$y, [uint32]$down = 0, [uint32]$up = 0, [int]$wparam = 0) {
	$l = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
	[HarnessWin]::PostMessage($hwnd, 0x200, [IntPtr]0, $l) | Out-Null
	Start-Sleep -Milliseconds 150
	if ($down -ne 0) {
		[HarnessWin]::PostMessage($hwnd, $down, [IntPtr]$wparam, $l) | Out-Null
		Start-Sleep -Milliseconds 60
		[HarnessWin]::PostMessage($hwnd, $up, [IntPtr]0, $l) | Out-Null
		Start-Sleep -Milliseconds 250
	}
}

# `itemdetails status`'s open count (needs logecho on and the console open).
# A left-button drag in client pixels: press, a run of moves with the button
# held (wparam MK_LBUTTON), release - what a player's hand sends. With Ctrl
# held through it by default, since a panel only arranges under Ctrl; -NoCtrl
# is the plain drag that must NOT move one.
function Send-Drag([int]$x0, [int]$y0, [int]$x1, [int]$y1, [int]$steps = 10, [switch]$NoCtrl) {
	$at = { param($x, $y) [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF)) }
	if (-not $NoCtrl) {
		[HarnessWin]::PostMessage($hwnd, 0x100, [IntPtr]0x11, [IntPtr]1) | Out-Null
		Start-Sleep -Milliseconds 60
	}
	[HarnessWin]::PostMessage($hwnd, 0x200, [IntPtr]0, (& $at $x0 $y0)) | Out-Null
	Start-Sleep -Milliseconds 150
	[HarnessWin]::PostMessage($hwnd, 0x201, [IntPtr]1, (& $at $x0 $y0)) | Out-Null
	Start-Sleep -Milliseconds 80
	for ($i = 1; $i -le $steps; $i++) {
		$x = [int]($x0 + ($x1 - $x0) * $i / $steps); $y = [int]($y0 + ($y1 - $y0) * $i / $steps)
		[HarnessWin]::PostMessage($hwnd, 0x200, [IntPtr]1, (& $at $x $y)) | Out-Null
		Start-Sleep -Milliseconds 40
	}
	Start-Sleep -Milliseconds 80
	[HarnessWin]::PostMessage($hwnd, 0x202, [IntPtr]0, (& $at $x1 $y1)) | Out-Null
	Start-Sleep -Milliseconds 100
	if (-not $NoCtrl) {
		[HarnessWin]::PostMessage($hwnd, 0x101, [IntPtr]0x11, [IntPtr][int64]0xC0000001) | Out-Null
	}
	Start-Sleep -Milliseconds 200
}

# A Ctrl+click at client pixel (x, y): what presses an arranging panel's reset.
function Send-CtrlClick([int]$x, [int]$y) {
	[HarnessWin]::PostMessage($hwnd, 0x100, [IntPtr]0x11, [IntPtr]1) | Out-Null
	Start-Sleep -Milliseconds 60
	Send-Mouse $x $y 0x201 0x202 1
	[HarnessWin]::PostMessage($hwnd, 0x101, [IntPtr]0x11, [IntPtr][int64]0xC0000001) | Out-Null
	Start-Sleep -Milliseconds 200
}

# One trip through the closed-panels tray (-Panels): Ctrl+click the Movement
# dock's minimize button, then a plain click on its tray button. The waits cover
# a button's push (it fires ~0.12 s after the release, ui::Button).
function Invoke-TrayTrip {
	Send-CtrlClick $script:hideX $script:hideY
	Start-Sleep -Milliseconds 400
	Send-Mouse $script:trayX $script:trayY 0x201 0x202 1
	Start-Sleep -Milliseconds 500
}

# `hudpanel list`'s row for one panel (console open, logecho on around it).
function Get-PanelRow([string]$id) {
	$before = @(Select-String -Path $log -Pattern "console:   $id ").Count
	Send-Key 0xC0
	Start-Sleep -Milliseconds 500
	Send-Text 'logecho on'; Send-Key 0x0D
	Send-Text 'hudpanel list'; Send-Key 0x0D
	$rows = Wait-NewLogLines "console:   $id " $before
	Send-Text 'logecho off'; Send-Key 0x0D
	Send-Key 0xC0
	Start-Sleep -Milliseconds 400
	if ($rows.Count -eq 0) { throw "the console never listed the $id panel" }
	return $rows[-1].Line
}

# The last line matching $pattern after sending $command (console open, logecho
# on), minus the log prefix.
function Get-ConsoleAnswer([string]$command, [string]$pattern) {
	$before = @(Select-String -Path $log -Pattern $pattern -SimpleMatch).Count
	Send-Text $command; Send-Key 0x0D
	$deadline = (Get-Date).AddSeconds(5)
	while ((Get-Date) -lt $deadline) {
		$lines = @(Select-String -Path $log -Pattern $pattern -SimpleMatch)
		if ($lines.Count -gt $before) { return $lines[-1].Line -replace '^.*console: ', '' }
		Start-Sleep -Milliseconds 200
	}
	throw "the console never answered ``$command``"
}

# One -All cycle, starting with the sheet up and the console shut: its "All"
# button opens the party window (on the sheet's tab), every tab stone is
# clicked with a hover over each card after it, Esc closes the window, and a
# click on member 0's portrait brings the sheet back. Ends as it began.
function Invoke-AllCycle {
	Send-Click $script:allX $script:allY
	Start-Sleep -Milliseconds 500
	foreach ($i in 1, 2, 3, 4, 0) {
		Send-Click $script:stones[$i].X $script:stones[$i].Y
		Start-Sleep -Milliseconds 250
		foreach ($p in $script:cardPoints) { Send-Mouse $p.X $p.Y }
	}
	Send-Key 0x1B
	Start-Sleep -Milliseconds 300
	Send-Click $script:portraitX $script:portraitY
	Start-Sleep -Milliseconds 500
}

function Get-DetailOpens {
	$before = @(Select-String -Path $log -Pattern 'item details: .* opens=').Count
	Send-Text 'itemdetails status'; Send-Key 0x0D
	$deadline = (Get-Date).AddSeconds(5)
	while ((Get-Date) -lt $deadline) {
		$lines = @(Select-String -Path $log -Pattern 'item details: .* opens=(\d+)')
		if ($lines.Count -gt $before) { return [int]$lines[-1].Matches[0].Groups[1].Value }
		Start-Sleep -Milliseconds 200
	}
	throw 'the console never answered `itemdetails status`'
}

# Wait-ForLog and Wait-NewLogLines are tools\HarnessGame.ps1's. POLL FOR A
# CONSOLE ANSWER, NEVER SLEEP A FIXED TIME AND READ: 2026-10-02, -Hand read
# `autocast` 800 ms after typing it and found 2 of its 4 rows, the other two
# written just after the read.

# Whether any line matches $pattern, waiting up to $timeoutSec for one to land.
function Wait-LogMatch([string]$pattern, [double]$timeoutSec = 5) {
	return (Wait-NewLogLines $pattern 0 1 $timeoutSec).Count -gt 0
}

# Waits until every command typed so far has answered (console open, logecho
# on), for output whose row count is not known up front (`torch` prints one
# per filled hand, `hudpanel list` one per panel). A bare `logecho` is the end
# mark: commands run in the order typed, so once its answer is in the log, so
# is everything before it.
function Wait-ConsoleDone {
	$mark = 'console: logecho on$'
	$before = @(Select-String -Path $log -Pattern $mark).Count
	Send-Text 'logecho'; Send-Key 0x0D
	if ((Wait-NewLogLines $mark $before).Count -eq 0) { throw 'the console never answered `logecho`' }
}

# Wait-ConsoleDone for a step that may KILL the game, where a death is the
# verdict rather than a harness fault: $true once the console answered, $false
# once the game is gone. A game still inside its crash handler (writing the
# minidump, walking the stack) has not exited yet and answers nothing, so a
# silent console is given 30 s to finish dying before it counts as a fault;
# only a game alive and silent past that throws, as Wait-ConsoleDone does.
function Wait-ConsoleOrDeath {
	$mark = 'console: logecho on$'
	$before = @(Select-String -Path $log -Pattern $mark).Count
	Send-Text 'logecho'; Send-Key 0x0D
	if ((Wait-NewLogLines $mark $before).Count -gt 0) { return $true }
	if (-not $proc.HasExited) { $proc.WaitForExit(30000) | Out-Null }
	if ($proc.HasExited) { return $false }
	throw 'the console never answered `logecho` (and the game did not exit)'
}

# A dead game's own account, for a check to quote: the exit code, the crash
# handler's CRASH line and the faulting frame (the first line under "faulting
# stack:"), read from dungeon.log as the dead process left it. $when finishes
# "the game DIED ...".
function Get-CrashNotes([string]$when) {
	$notes = @("the game DIED $when (exit $($proc.ExitCode))")
	$lines = @(Get-Content $log -Encoding UTF8)
	$crash = @($lines | Where-Object { $_ -match 'CRASH: ' }) | Select-Object -Last 1
	$at = [Array]::FindLastIndex([string[]]$lines, [Predicate[string]] { param($s) $s -match 'faulting stack:' })
	$frame = if ($at -ge 0 -and $at + 1 -lt $lines.Count) { $lines[$at + 1].Trim() -replace '^\[\w+ *\]\s+', '' } else { '' }
	if ($crash) { $notes += ($crash -replace '^.*CRASH: ', 'CRASH: ' -replace '\s+\S\s+the process.*$', '') }
	else { $notes += 'dungeon.log holds no CRASH line' }
	if ($frame) { $notes += "faulting in $frame" }
	return $notes
}

# Asks the console for the encounter tally and returns one numeric field of the
# NEW line it prints (needs logecho on). Counting the lines first is what stops
# it reading the previous answer back.
function Get-TallyField([string]$field) {
	$before = @(Select-String -Path $log -Pattern 'TALLY ').Count
	Send-Text 'tally'; Send-Key 0x0D
	$deadline = (Get-Date).AddSeconds(10)
	while ((Get-Date) -lt $deadline) {
		$lines = @(Select-String -Path $log -Pattern 'TALLY ')
		if ($lines.Count -gt $before) {
			$script:lastTally = $lines[-1].Line -replace '^.*TALLY ', 'TALLY '
			if ($lines[-1].Line -match "\b$field=([0-9.]+)") { return [double]$Matches[1] }
			throw "tally printed no '$field': $($lines[-1].Line)"
		}
		Start-Sleep -Milliseconds 200
	}
	throw 'the console never answered `tally` - is logecho on?'
}

# `glass`'s frames= field: main-scene frames that drew any glass since launch
# (needs logecho on and the console open). Counts lines first, like the tally.
function Get-GlassFrames {
	$pattern = 'console: glass queued='
	$before = @(Select-String -Path $log -Pattern $pattern -SimpleMatch).Count
	Send-Text 'glass'; Send-Key 0x0D
	$deadline = (Get-Date).AddSeconds(10)
	while ((Get-Date) -lt $deadline) {
		$lines = @(Select-String -Path $log -Pattern $pattern -SimpleMatch)
		if ($lines.Count -gt $before) {
			if ($lines[-1].Line -match '\bframes=(\d+)') { return [int]$Matches[1] }
			throw "glass printed no frames=: $($lines[-1].Line)"
		}
		Start-Sleep -Milliseconds 200
	}
	throw 'the console never answered `glass` - is logecho on?'
}

# `shadows status`, parsed (needs logecho on and the console open): one object
# per shadow slot - the light holding it this frame, its re-renders in all and
# by reason, the pass of its last render, and its light's lag (the farthest it
# stood from its cube's pose while the cube was reused, in squares). Counts the
# rows first, like the tally.
function Get-ShadowStatus {
	$pattern = 'console: shadows slot \d+: '
	$before = @(Select-String -Path $log -Pattern $pattern).Count
	Send-Text 'shadows status'; Send-Key 0x0D
	$rows = Wait-NewLogLines $pattern $before 8
	if ($rows.Count -lt 8) { throw "``shadows status`` printed $($rows.Count) slot rows, not 8" }
	$slots = foreach ($r in $rows[0..7]) {
		if ($r.Line -notmatch 'shadows slot (\d+): light=(\S+) last=(\S+) renders=(\d+) new=(\d+) geometry=(\d+) caster=(\d+) moved=(\d+) flicker=(\d+) lastpass=(\d+) lag=([0-9.]+)') {
			throw "unreadable shadow row: $($r.Line)"
		}
		[pscustomobject]@{
			Slot = [int]$Matches[1]; Light = $Matches[2]; Renders = [int64]$Matches[4]
			Caster = [int64]$Matches[7]; Moved = [int64]$Matches[8]
			LastPass = [int64]$Matches[10]
			Lag = [double]::Parse($Matches[11], [Globalization.CultureInfo]::InvariantCulture)
		}
	}
	return ,@($slots)
}

# `shadows door <x> <z>`, parsed: the door's pose and the first shadow pass that
# drew it (Door::posePass - stamped by the leaf's travel itself, not by its
# caster note, so it cannot agree with a note that went missing).
function Get-DoorShadow([int]$x, [int]$z) {
	$pattern = "console: shadows door $x,${z}: "
	$before = @(Select-String -Path $log -Pattern $pattern -SimpleMatch).Count
	Send-Text "shadows door $x $z"; Send-Key 0x0D
	$rows = Wait-NewLogLines ([regex]::Escape($pattern)) $before
	if ($rows.Count -eq 0) { throw "``shadows door $x $z`` printed nothing" }
	if ($rows[-1].Line -notmatch 'open=(\d) openT=([0-9.]+) pull=([0-9.]+) posepass=(\d+)') {
		throw "unreadable door row: $($rows[-1].Line)"
	}
	$inv = [Globalization.CultureInfo]::InvariantCulture
	return [pscustomobject]@{
		Open = $Matches[1] -eq '1'; OpenT = [double]::Parse($Matches[2], $inv)
		Pull = [double]::Parse($Matches[3], $inv); PosePass = [int64]$Matches[4]
	}
}

# THE SHADOW CACHE (code-review C178 / C187), with the console open and logecho
# on. A cube is re-rendered only when something it shows changed, and two kinds
# of change used to be missed: a DOOR (its leaf moved with no map revision, so a
# still party's torch kept the shut door's shadow until it stepped), and a
# WANDERING light that walks - a carried Firelight ignored moves altogether and
# kept up only on the flicker cadence. eval_arena carved to a corridor (nothing
# else in it), a door two squares east of the party, and:
#   DOOR: slot 0 (the held torch) must NOT re-render over a still second - else
#     the check proves nothing - MUST re-render, for a caster, once the door is
#     opened, its LAST render must come at or after the pass that first drew the
#     door's landed pose (`shadows door`: one render as the leaf began to move
#     would count as "a caster" and still keep a nearly shut leaf's shadow), and
#     it must stop again once the door has stopped.
#   FIRELIGHT: Maren casts one; with the flicker cadence off (`shadowrate 0`) its
#     cube must NOT re-render for a move over a still second (a slack tighter than
#     its own wander would re-render a standing light, the waste the slack is
#     there to prevent), MUST re-render for a MOVE while the party walks two
#     squares west, and its LAG - the farthest the light stood from the pose its
#     cube showed - must stay within a tenth of a square. The lag, not a count of
#     re-renders, is the bound on a slack too WIDE: it is read off positions, so
#     it holds at any frame rate, where a count over the walk would move with it.
# Returns @{ Door = PASS|FAIL|UNMEASURED; Fire = ...; Notes = what was seen }.
function Test-ShadowCache {
	$notes = @()
	Send-Text 'lightstress off'; Send-Key 0x0D
	Enter-FrozenArena 'eval_arena'
	Send-Text 'arena corridor 11'; Send-Key 0x0D
	Send-Text 'tp 13 12'; Send-Key 0x0D
	Send-Text 'face east'; Send-Key 0x0D
	Send-Text 'editor place doors wooden_door 15 12'; Send-Key 0x0D
	Send-Text 'mappage close'; Send-Key 0x0D
	# The flicker cadence OFF for both checks, so a re-render means a change -
	# whatever light slot 0 holds (a wandering one would otherwise tick at 25 Hz).
	Send-Text 'shadowrate 0'; Send-Key 0x0D
	if ($ShadowSelfTest) { Send-Text 'shadows ignore both'; Send-Key 0x0D }
	Wait-ConsoleDone
	# The carve and the placement re-render every cube once; let that settle.
	Start-Sleep -Seconds 2
	$a = Get-ShadowStatus
	Start-Sleep -Seconds 1
	$b = Get-ShadowStatus
	$door = 'UNMEASURED'
	if ($b[0].Light -eq 'none') {
		$notes += 'no light holds slot 0 (does anyone carry a lit torch?)'
	} elseif ($b[0].Renders -ne $a[0].Renders) {
		$notes += "slot 0 re-rendered $($b[0].Renders - $a[0].Renders) times over a still second - the door check would prove nothing"
	} else {
		$opened = 'console: door 15,12 -> open'
		$openBefore = @(Select-String -Path $log -Pattern $opened).Count
		Send-Text 'opendoor 15 12'; Send-Key 0x0D
		if ((Wait-NewLogLines $opened $openBefore).Count -eq 0) {
			$notes += 'the door at 15,12 did not open'
		} else {
			Start-Sleep -Seconds 2 # the leaf's 0.8 s and the chain's pull, settled
			# ...or a little longer on a slow frame: the pose that matters is the
			# one it LANDS in.
			$deadline = (Get-Date).AddSeconds(5)
			do {
				$pose = Get-DoorShadow 15 12
				if ($pose.OpenT -ge 1.0 -and $pose.Pull -le 0.0) { break }
				Start-Sleep -Milliseconds 250
			} while ((Get-Date) -lt $deadline)
			$c = Get-ShadowStatus
			if ($pose.OpenT -lt 1.0 -or $pose.Pull -gt 0.0) {
				$notes += "the door at 15,12 never came to rest (openT $($pose.OpenT), pull $($pose.Pull))"
			} elseif ($c[0].Light -ne $b[0].Light) {
				$notes += "slot 0 changed hands as the door opened ($($b[0].Light) -> $($c[0].Light))"
			} else {
				$renders = $c[0].Renders - $b[0].Renders; $caster = $c[0].Caster - $b[0].Caster
				# The cube must show the pose the door LANDED in: its last render at
				# or after the first pass that drew it.
				$landed = $c[0].LastPass -ge $pose.PosePass
				# ...and once the door has stopped, so has the cube: a change noted
				# once must not keep the cache re-rendering (the cache would be gone).
				Start-Sleep -Seconds 1
				$after = (Get-ShadowStatus)[0].Renders - $c[0].Renders
				$notes += "slot 0 ($($b[0].Light)): 0 re-renders over a still second, then $renders ($caster for a caster) as the door opened, the last at pass $($c[0].LastPass) (the landed pose first drawn at pass $($pose.PosePass)), then $after over a still second"
				$door = if ($caster -ge 1 -and $landed -and $after -eq 0) { 'PASS' } else { 'FAIL' }
			}
		}
	}

	# FIRELIGHT. Faced west FIRST: a turn moves a light that hangs ahead of the
	# party, and the move this measures is the walk's. Maren casts it (the last
	# member of a shorter -Party).
	$m = [Math]::Min(2, $memberCount - 1)
	Send-Text 'face west'; Send-Key 0x0D
	Send-Text "learn $m fire"; Send-Key 0x0D
	Send-Text "learn $m light"; Send-Key 0x0D
	Send-Text "setskill $m fire 10"; Send-Key 0x0D
	Send-Text 'heal'; Send-Key 0x0D
	$castLine = 'console: (cast away|no cast)'
	$castBefore = @(Select-String -Path $log -Pattern $castLine).Count
	Send-Text "cast $m fire light"; Send-Key 0x0D
	$cast = Wait-NewLogLines $castLine $castBefore
	Wait-ConsoleDone
	Start-Sleep -Seconds 1
	$fire = 'UNMEASURED'
	$d = Get-ShadowStatus
	$spell = @($d | Where-Object { $_.Light -match '^spell:' })
	if ($cast.Count -eq 0 -or $cast[-1].Line -notmatch 'cast away') {
		$notes += "member $m's Firelight was not cast"
	} elseif ($spell.Count -eq 0) {
		$notes += 'the Firelight holds no shadow slot'
	} else {
		# A STILL second first: a standing Firelight's wander alone must not
		# count as a move.
		Start-Sleep -Seconds 1
		$still = (Get-ShadowStatus)[$spell[0].Slot]
		Send-Text 'forward 2'; Send-Key 0x0D
		Start-Sleep -Seconds 3 # two steps at the party's pace
		$e = Get-ShadowStatus
		$after = $e[$spell[0].Slot]
		if ($still.Light -ne $spell[0].Light -or $after.Light -ne $spell[0].Light) {
			$notes += "the Firelight left slot $($spell[0].Slot) ($($still.Light), then $($after.Light) holds it)"
		} else {
			$stillMoved = $still.Moved - $spell[0].Moved
			$moved = $after.Moved - $still.Moved
			$lagOk = $after.Lag -le 0.1
			$notes += "the Firelight ($($spell[0].Light), slot $($spell[0].Slot)), flicker off: $stillMoved re-renders for a move over a still second, then $moved over a two-square walk, lagging its cube by at most $($after.Lag) of a square"
			$fire = if ($stillMoved -eq 0 -and $moved -ge 3 -and $lagOk) { 'PASS' } else { 'FAIL' }
		}
	}
	Send-Text 'shadowrate 25 2'; Send-Key 0x0D
	Send-Text 'shadows ignore none'; Send-Key 0x0D
	Wait-ConsoleDone
	return [pscustomobject]@{ Door = $door; Fire = $fire; Notes = $notes }
}

# THE CANDIDATE CEILING (code-review C181), with the console open and logecho on,
# on the level the window ran on (crypt1: two sconces and a brazier, all lit).
# PushLight returns null once the frame's candidate list is full (256), and the
# fire loop wrote through that null - latent, since it needs ~240 lit fires.
# `lightstress fill` makes it happen: 256 test lights pushed AHEAD of the fires,
# so every fire's push is refused. The game must live through it, the list must
# read full (`lights`: "of 256 candidates"), and the ceiling line must count a
# FIRE among the refused - else the fire loop never met a full list and nothing
# was tested. Returns @{ Verdict = PASS|FAIL|UNMEASURED; Notes = ... }.
function Test-LightCeiling {
	$notes = @()
	$placed = 'console: lightstress: (\d+) test lights.*\(fill\)'
	$placedBefore = @(Select-String -Path $log -Pattern $placed).Count
	Send-Text 'lightstress fill'; Send-Key 0x0D
	$fill = Wait-NewLogLines $placed $placedBefore
	Start-Sleep -Seconds 1 # frames with the list full
	$ceiling = 'console: lights: ceiling (\d+): refused (\d+)(?: \((.*)\))?$'
	$ceilBefore = @(Select-String -Path $log -Pattern $ceiling).Count
	$headBefore = @(Select-String -Path $log -Pattern 'console: lights: \d+ drawn of').Count
	Send-Text 'lights'; Send-Key 0x0D
	$ceil = Wait-NewLogLines $ceiling $ceilBefore
	$head = @(Select-String -Path $log -Pattern 'console: lights: \d+ drawn of') | Select-Object -Skip $headBefore
	Send-Text 'lightstress off'; Send-Key 0x0D
	# ONE place a death is seen, however late it lands: a game that died in the
	# first frames, one still in its crash handler when `lights` was typed and one
	# that went down later all reach this (the waits above return early on an
	# exit, and typing to a dead window is a no-op), and all quote the crash
	# handler's CRASH line and faulting frame - the fire loop, if that is the bug.
	if (-not (Wait-ConsoleOrDeath)) {
		$notes += Get-CrashNotes 'with the candidate list full'
		return [pscustomobject]@{ Verdict = 'FAIL'; Notes = $notes }
	}
	if ($fill.Count -eq 0) {
		$notes += '`lightstress fill` placed nothing (no answer)'
		return [pscustomobject]@{ Verdict = 'UNMEASURED'; Notes = $notes }
	}
	$read = $ceil.Count -gt 0 -and $head.Count -gt 0 -and ($ceil[-1].Line -match $ceiling)
	if (-not $read) {
		$notes += '`lights` printed no ceiling line'
		return [pscustomobject]@{ Verdict = 'UNMEASURED'; Notes = $notes }
	}
	$limit = [int]$Matches[1]; $refused = [int]$Matches[2]; $bySource = $Matches[3]
	$candidates = if ($head[-1].Line -match 'drawn of (\d+) candidates') { [int]$Matches[1] } else { -1 }
	$fires = if ("$bySource" -match '\bfire (\d+)') { [int]$Matches[1] } else { 0 }
	$notes += "$($fill[-1].Line -replace '^.*console: ', ''); the list held $candidates of $limit, refused $refused ($bySource); the game lived"
	$verdict = if ($candidates -ne $limit -or $fires -lt 1) { 'UNMEASURED' } else { 'PASS' }
	if ($verdict -ne 'PASS') { $notes += 'no lit fire met a full list - the fire loop was not tested' }
	return [pscustomobject]@{ Verdict = $verdict; Notes = $notes }
}

# THE FLOOR GLOWS (code-review C190): tools\EvalScripts\floorglow.eval, run
# headless BEFORE the game this run judges (its own process truncates the one
# dungeon.log, so after would overwrite the window's evidence). It lays the
# flamebrand, then the frostbrand, then a plain khukri two squares ahead and
# prints `lights` for each: an enchanted blade's floor glow must be its
# ELEMENT'S colour (Spells.cpp ElementColor - fire 1.00 0.13 0.08, water 0.18
# 0.42 1.00), where the category's steel grey used to overwrite it, and the plain
# blade must have none. Exactly one glow row per blade section, or the section
# read something else. Killed BY PID if it outlives its timeout.
function Test-FloorGlows {
	$notes = @()
	$script = Join-Path $root 'tools\EvalScripts\floorglow.eval'
	Remove-Item $log -ErrorAction SilentlyContinue
	$p = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru `
		-ArgumentList @('-project', 'dungeon-demo', '-headless', '-eval', $script)
	if (-not $p.WaitForExit(240000)) {
		$p.Kill(); $p.WaitForExit(5000) | Out-Null
		$notes += 'floorglow.eval did not finish in 240 s (killed)'
		return [pscustomobject]@{ Verdict = 'FAIL'; Notes = $notes }
	}
	$lines = if (Test-Path $log) { @(Get-Content $log -Encoding UTF8) } else { @() }
	if (-not ($lines -match 'eval RESULT=PASS script=floorglow\.eval')) {
		$notes += "floorglow.eval did not run clean (exit $($p.ExitCode))"
		return [pscustomobject]@{ Verdict = 'FAIL'; Notes = $notes }
	}
	# Each section's glow rows, as "r g b" strings (the readout's own 2 decimals).
	$sections = @{}
	$current = $null
	foreach ($line in $lines) {
		if ($line -match 'console: --- (\w+) ---') { $current = $Matches[1]; $sections[$current] = @(); continue }
		if ($current -and $line -match 'console:   \[\s*\d+\] glow +\S+ +rgb ([0-9.]+ [0-9.]+ [0-9.]+) ') {
			$sections[$current] += $Matches[1]
		}
	}
	$want = @{ flamebrand = '1.00 0.13 0.08'; frostbrand = '0.18 0.42 1.00' }
	$ok = $true
	foreach ($blade in 'flamebrand', 'frostbrand') {
		$rows = @(if ($sections.ContainsKey($blade)) { $sections[$blade] })
		if ($rows.Count -ne 1) {
			$notes += "${blade}: $($rows.Count) glow rows (want 1)"; $ok = $false; continue
		}
		$match = $rows[0] -eq $want[$blade]
		$notes += "${blade}: glow rgb $($rows[0]) (its element: $($want[$blade])) $(if ($match) { 'ok' } else { 'WRONG' })"
		if (-not $match) { $ok = $false }
	}
	$plain = if ($sections.ContainsKey('plain')) { @($sections['plain']).Count } else { -1 }
	$notes += "plain khukri: $plain glow rows (want 0)"
	if ($plain -ne 0) { $ok = $false }
	return [pscustomobject]@{ Verdict = $(if ($ok) { 'PASS' } else { 'FAIL' }); Notes = $notes }
}

# -Items: a floor item's POSE and the click PICKS (code-review C180 / C359 /
# C258), from tools\EvalScripts\itempose.eval run headless BEFORE the window's
# game, like Test-FloorGlows. A rune in an open wall niche must glow (one glow
# row) IN THE POCKET, over the rune where it is drawn, and in a SHUT one must not
# (none - it glowed at the foot of the wall either way); then every `pickprobe`
# section must end RESULT=PASS with exactly the targets it set up - a torch and
# a rune shot at five points across a quarter centred on their drawn boxes at
# their drawn height, an upright potion and a model-less key likewise, the niche
# rune, the wall torch, a chain and a pad at their drawn middles, each with a
# point that must miss. A section that probed nothing (RESULT=NONE, or a kind missing)
# set nothing up, which is a FAIL here, not a pass. Killed BY PID past its
# timeout.
function Test-ItemPose {
	$notes = @()
	$script = Join-Path $root 'tools\EvalScripts\itempose.eval'
	Remove-Item $log -ErrorAction SilentlyContinue
	$p = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru `
		-ArgumentList @('-project', 'dungeon-demo', '-headless', '-eval', $script)
	if (-not $p.WaitForExit(240000)) {
		$p.Kill(); $p.WaitForExit(5000) | Out-Null
		$notes += 'itempose.eval did not finish in 240 s (killed)'
		return [pscustomobject]@{ Verdict = 'FAIL'; Notes = $notes }
	}
	$lines = if (Test-Path $log) { @(Get-Content $log -Encoding UTF8) } else { @() }
	if (-not ($lines -match 'eval RESULT=PASS script=itempose\.eval')) {
		$notes += "itempose.eval did not run clean (exit $($p.ExitCode))"
		return [pscustomobject]@{ Verdict = 'FAIL'; Notes = $notes }
	}
	# Per section: its glow rows (each one's position, from the `(light at x y z)`
	# line `lights` prints just before it), its probe lines and its verdict line.
	$glows = @{}; $probes = @{}; $verdicts = @{}
	$current = $null
	$lightAt = $null
	foreach ($line in $lines) {
		if ($line -match 'console: --- ([\w ]+) ---') {
			$current = $Matches[1]; $glows[$current] = @(); $probes[$current] = @(); continue
		}
		if (-not $current) { continue }
		if ($line -match '\(light at (-?[0-9.]+) (-?[0-9.]+) (-?[0-9.]+)\)') {
			$lightAt = @([double]$Matches[1], [double]$Matches[2], [double]$Matches[3])
		}
		elseif ($line -match 'console:   \[\s*\d+\] glow ') { $glows[$current] += , $lightAt }
		elseif ($line -match 'console: (pickprobe: .*)$') { $probes[$current] += $Matches[1] }
		elseif ($line -match 'console: (pickprobe RESULT=.*)$') { $verdicts[$current] = $Matches[1] }
	}
	$ok = $true
	$wantGlows = [ordered]@{ 'niche open' = 1; 'niche shut' = 0 }
	foreach ($section in $wantGlows.Keys) {
		$got = if ($glows.ContainsKey($section)) { @($glows[$section]).Count } else { -1 }
		$good = $got -eq $wantGlows[$section]
		$notes += "${section}: $got glow rows (want $($wantGlows[$section])) $(if ($good) { 'ok' } else { 'WRONG' })"
		if (-not $good) { $ok = $false }
	}
	# WHERE the open niche's glow is (C180's other half: it glows IN the pocket).
	# It must stand over the rune where the rune is DRAWN - within 5 cm across, and
	# above its drawn middle by no more than a metre - read off the `niche pick`
	# section's probe line (the same rune in the same niche, reopened). The old
	# glow stood at the item's floor quarter, 0.40 m up: well clear across, and
	# below the pocket floor.
	$drawn = $null
	foreach ($probe in @(if ($probes.ContainsKey('niche pick')) { $probes['niche pick'] })) {
		if ($probe -match '^pickprobe: niche .* drawn about (-?[0-9.]+) (-?[0-9.]+) (-?[0-9.]+):') {
			$drawn = @([double]$Matches[1], [double]$Matches[2], [double]$Matches[3])
		}
	}
	$openGlows = @(if ($glows.ContainsKey('niche open')) { $glows['niche open'] })
	if ($openGlows.Count -ne 1 -or -not $openGlows[0] -or -not $drawn) {
		$notes += 'niche open: the glow''s place was not read (no single glow row with a position, or no niche probe line)'
		$ok = $false
	} else {
		$g = $openGlows[0]
		$across = [math]::Sqrt(($g[0] - $drawn[0]) * ($g[0] - $drawn[0]) + ($g[2] - $drawn[2]) * ($g[2] - $drawn[2]))
		$rise = $g[1] - $drawn[1]
		$good = $across -le 0.05 -and $rise -gt 0 -and $rise -le 1.0
		$notes += ('niche open: glow at {0:F2} {1:F2} {2:F2}, the rune drawn about {3:F2} {4:F2} {5:F2} - {6:F2} m across, {7:F2} m above (want <= 0.05 across, 0..1 above) {8}' -f `
			$g[0], $g[1], $g[2], $drawn[0], $drawn[1], $drawn[2], $across, $rise, $(if ($good) { 'ok' } else { 'WRONG' }))
		if (-not $good) { $ok = $false }
	}
	# Each probe section: the exact counts it set up, and a PASS.
	$expect = [ordered]@{
		'niche pick'   = 'targets=1 wrong=0 floor=0 niche=1 opener=0 sconce=0'
		'floor pick'   = 'targets=2 wrong=0 floor=2 niche=0 opener=0 sconce=0'
		'upright pick' = 'targets=2 wrong=0 floor=2 niche=0 opener=0 sconce=0'
		'sconce pick'  = 'targets=1 wrong=0 floor=0 niche=0 opener=0 sconce=1'
		'chain pick'   = 'targets=1 wrong=0 floor=0 niche=0 opener=1 sconce=0'
		'pad pick'     = 'targets=1 wrong=0 floor=0 niche=0 opener=1 sconce=0'
	}
	foreach ($section in $expect.Keys) {
		foreach ($probe in @(if ($probes.ContainsKey($section)) { $probes[$section] })) { $notes += "  $probe" }
		$verdict = if ($verdicts.ContainsKey($section)) { $verdicts[$section] } else { '(no verdict line)' }
		$good = $verdict -eq "pickprobe RESULT=PASS $($expect[$section])"
		$notes += "${section}: $verdict $(if ($good) { 'ok' } else { "WRONG (want RESULT=PASS $($expect[$section]))" })"
		if (-not $good) { $ok = $false }
	}
	return [pscustomobject]@{ Verdict = $(if ($ok) { 'PASS' } else { 'FAIL' }); Notes = $notes }
}

# Throws unless the party stands on x,z facing north (asks `pos`; needs logecho
# on). -Impact's whole geometry hangs on it: a `tp` or `face` swallowed by a
# busy console leaves the party firing somewhere else, and the barrage then
# measures bolts expiring into a far wall.
function Assert-PartyAt([int]$x, [int]$z) {
	$want = "console: $x,$z facing north"
	$before = @(Select-String -Path $log -Pattern $want -SimpleMatch).Count
	Send-Text 'pos'; Send-Key 0x0D
	$deadline = (Get-Date).AddSeconds(5)
	while ((Get-Date) -lt $deadline) {
		if (@(Select-String -Path $log -Pattern $want -SimpleMatch).Count -gt $before) { return }
		Start-Sleep -Milliseconds 200
	}
	$got = Select-String -Path $log -Pattern 'console: \d+,\d+ facing ' | Select-Object -Last 1
	throw "the party is not at $x,$z facing north (pos: $(if ($got) { $got.Line } else { 'no answer' }))"
}

# Goes to eval_arena, freezes its monsters and heals the party (needs logecho on
# and the console open). -Impact and -Items both want its open floor; -Hand
# names another level, for its wall torch.
function Enter-FrozenArena([string]$Stem = 'eval_arena') {
	# The console refuses commands while the level loads; a NEW "Level
	# ready" line is the moment it will take them again. NEW, counted from
	# before the goto: when the landing page Continues an eval save, the
	# game has ALREADY printed one for eval_arena, the wait matched it at
	# once, and `tp` and `face` were typed into the reload and refused - the
	# party then fired the whole barrage the wrong way.
	$readyPattern = "^\[info \] Level ready: $Stem"
	$readyBefore = @(Select-String -Path $log -Pattern $readyPattern).Count
	Send-Text "goto $Stem"; Send-Key 0x0D
	$deadline = (Get-Date).AddSeconds($LoadTimeoutSec)
	while (@(Select-String -Path $log -Pattern $readyPattern).Count -le $readyBefore) {
		if ($proc.HasExited) { throw "the game exited during the arena load (code $($proc.ExitCode))" }
		if ((Get-Date) -gt $deadline) { throw 'timed out waiting for the arena load' }
		Start-Sleep -Milliseconds 500
	}
	# FREEZE FIRST, THEN HEAL. A new game arrives on eval_arena's start
	# square among the arena's own monsters, which got two seconds to act
	# while the script typed: a fresh run found Sera down at 0 hp, and every
	# one of her 54 casts refused. (A Continue into an eval save happened
	# to arrive somewhere quieter, which is why it passed.)
	Send-Text 'freeze on'; Send-Key 0x0D
	Send-Text 'heal'; Send-Key 0x0D
	Start-Sleep -Milliseconds 800
}

# `inventory status`, minus the log prefix (needs logecho on and the console
# open): "inventory: open|closed held=<id|none> | 0: <slot> <slot> ... | 1: ...".
function Get-InventoryStatus {
	$pattern = 'console: inventory: '
	$before = @(Select-String -Path $log -Pattern $pattern -SimpleMatch).Count
	Send-Text 'inventory status'; Send-Key 0x0D
	$deadline = (Get-Date).AddSeconds(5)
	while ((Get-Date) -lt $deadline) {
		$lines = @(Select-String -Path $log -Pattern $pattern -SimpleMatch)
		if ($lines.Count -gt $before) { return $lines[-1].Line -replace '^.*console: ', '' }
		Start-Sleep -Milliseconds 200
	}
	throw 'the console never answered `inventory status`'
}

# Member $m's pack slots ('-' = free), from an `inventory status` line.
function Get-PackSlots([string]$status, [int]$m) {
	if ($status -notmatch "\| ${m}:((?: [^|\s]+)*)") { throw "no member $m in: $status" }
	return @($Matches[1].Trim() -split ' ')
}

# Where item $id sits in member $m's pack.
function Get-PackSlot([string]$status, [int]$m, [string]$id) {
	$i = [array]::IndexOf((Get-PackSlots $status $m), $id)
	if ($i -lt 0) { throw "$id is not in member $m's pack: $status" }
	return $i
}

# The centre of member $m's pack slot $i in the party window, as the game
# reports it (`inventory slot`; needs logecho on, the console open and the
# window open on its Inventory tab). It used to be worked out here from the
# old window's fractions; the party window (more-ui-updates Phase 5) lays
# itself out in em, which a harness cannot see.
function Get-InventorySlotPoint([int]$m, [int]$i) {
	$pattern = "console: inventory slot $m ${i}: "
	$before = @(Select-String -Path $log -Pattern $pattern -SimpleMatch).Count
	Send-Text "inventory slot $m $i"; Send-Key 0x0D
	$deadline = (Get-Date).AddSeconds(5)
	while ((Get-Date) -lt $deadline) {
		$lines = @(Select-String -Path $log -Pattern $pattern -SimpleMatch)
		if ($lines.Count -gt $before) {
			if ($lines[-1].Line -notmatch ': (\d+),(\d+) \d+x\d+$') { throw "unreadable: $($lines[-1].Line)" }
			return [pscustomobject]@{ X = [int]$Matches[1]; Y = [int]$Matches[2] }
		}
		Start-Sleep -Milliseconds 200
	}
	throw "the console never answered ``inventory slot $m $i`` (is the window open?)"
}

# A left click at client pixel (x, y).
function Send-Click([int]$x, [int]$y) { Send-Mouse $x $y 0x201 0x202 1 }

# One round trip for the item in member 0's pack slot $slot, starting with the
# party inventory window OPEN and the console shut: out of the slot onto the
# cursor; a floor click, the drop; a second, the lift; then the item put back.
# The window is a NON-MODAL floating window (ui-panels P3b), so it stays open
# throughout and the floor below it takes the clicks directly. Ends as it began.
function Invoke-ItemRoundTrip($slot) {
	Send-Click $slot.X $slot.Y
	Send-Click $script:floorX $script:floorY
	Send-Click $script:floorX $script:floorY
	Send-Click $slot.X $slot.Y
}

# `sheet status`'s pack-row line (needs logecho on, the console open and the
# sheet up): the row (one entry per pack-row square, '-' = none), the selected
# square, its slot count and the equips counted so far.
function Get-SheetPacks {
	$pattern = 'console: sheet packs: '
	$before = @(Select-String -Path $log -Pattern $pattern -SimpleMatch).Count
	Send-Text 'sheet status'; Send-Key 0x0D
	$deadline = (Get-Date).AddSeconds(5)
	while ((Get-Date) -lt $deadline) {
		$lines = @(Select-String -Path $log -Pattern $pattern -SimpleMatch)
		if ($lines.Count -gt $before) {
			$line = $lines[-1].Line -replace '^.*console: sheet packs: ', ''
			if ($line -notmatch '^(.*) selected=(\d+) slots=(\d+) equips=(\d+)$') {
				throw "unreadable sheet packs line: $line"
			}
			return [pscustomobject]@{
				Row = @($Matches[1].Trim() -split ' ')
				Selected = [int]$Matches[2]; Slots = [int]$Matches[3]
				Equips = [int]$Matches[4]; Line = $line
			}
		}
		Start-Sleep -Milliseconds 200
	}
	throw 'the console never answered `sheet status`'
}

# Centres on the sheet's Inventory tab, as window fractions measured from the
# -Sheet run's calibration (backpack slots 3 and 4 at 0.6675 / 0.72 of the
# width, 0.5033 of the height): a slot step of 0.0525 of the width, and the
# pack row 0.161 of the sheet BODY above the grid (CharacterSheetLayout.h:
# kPackY - kPackRowY), the body being 0.62 of the window's height (kBodyH).
function Get-SheetGridPoint([int]$i) {
	return [pscustomobject]@{
		X = [int]($script:clientW * (0.51 + ($i % 6) * 0.0525))
		Y = [int]($script:clientH * (0.5033 + [math]::Floor($i / 6) * 0.147 * 0.62))
	}
}
function Get-SheetPackRowPoint([int]$i) {
	return [pscustomobject]@{
		X = [int]($script:clientW * (0.51 + $i * 0.0525))
		Y = [int]($script:clientH * (0.5033 - 0.161 * 0.62))
	}
}

# One numeric field of the tally line Get-TallyField last read.
function Get-LastTallyField([string]$field) {
	if ($script:lastTally -match "\b$field=([0-9.]+)") { return [double]$Matches[1] }
	throw "tally printed no '$field': $script:lastTally"
}

# What -Impact counts (hits, expiries, blasts), from one fresh `tally`.
function Get-ImpactCounts {
	Get-TallyField 'bolthits' | Out-Null
	return [pscustomobject]@{
		Hits = Get-LastTallyField 'bolthits'
		Expired = Get-LastTallyField 'expired'
		Blasts = Get-LastTallyField 'blasts'
	}
}

$proc = $null
$hwnd = [IntPtr]::Zero
$code = 1
$shadowSelfTestFailed = $false

# -Lights: the floor glows FIRST, in a headless eval of their own
# (Test-FloorGlows) - its game writes the same dungeon.log, which the game below
# then truncates, so the window's evidence is the log that survives the run.
$glow = $null
if ($Lights -and (-not $SelfTest -or $ShadowSelfTest)) {
	Write-Host 'checking the floor glows: floorglow.eval, headless'
	$glow = Test-FloorGlows
	foreach ($n in $glow.Notes) { Write-Host "  $n" }
	Write-Host "  floor glows: $($glow.Verdict)"
}
# -Items: the item pose and the click picks, likewise before the game
# (Test-ItemPose). Not under the guard's own -SelfTest, which is about the guard.
$pose = $null
if ($Items -and -not $SelfTest) {
	Write-Host 'checking the item pose and the picks: itempose.eval, headless'
	$pose = Test-ItemPose
	foreach ($n in $pose.Notes) { Write-Host "  $n" }
	Write-Host "  item pose and picks: $($pose.Verdict)"
}

try {
	Start-HarnessGame $exe $bin $log $LoadTimeoutSec
	if ($PartyPage) {
		# NO GAME: the page on the title screen, opened by its dev twin once the
		# console answers there, then the AllocTest-only switch that lets the
		# guard arm on it. Each waits for its own answer, counted from before.
		Write-Host 'opening the party creation page on the title screen'
		Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 500
		if (-not (Wait-ConsoleReady)) { throw 'the console never accepted a command on the title screen' }
		$openLine = 'console: party page: open$'
		$before = Get-LogMatchCount $openLine
		Send-Text 'partypage open'; Send-Key $VK_RETURN
		Wait-ForNewLog $openLine 60 'the party page to open' $before | Out-Null
		$guardLine = 'console: party page guarded: on'
		$before = Get-LogMatchCount $guardLine
		Send-Text 'allocguard partypage on'; Send-Key $VK_RETURN
		Wait-ForNewLog $guardLine 10 'the party page switch' $before | Out-Null
	} else {
		# Through the console's `newgame` (or `newparty`), never the landing page -
		# Enter there is Continue on the newest shared save - waited out to the
		# LEVEL, not 'Game loaded:' (a load task's line, while commands are still
		# refused), and then until the console really answers (tools\HarnessGame.ps1).
		if ($Party) { Write-Host "  with a created party of $memberCount" }
		Start-NewGame $LoadTimeoutSec -PartySpec $Party | Out-Null
		# A REFUSED `newparty` still ends in a game - the default four's - so the run
		# would measure the wrong party and PASS. The command's own line is the proof.
		if ($Party -and -not (Select-String -Path $log -Pattern "console: new game with a party of $memberCount\b" -EA SilentlyContinue)) {
			throw "newparty did not build the party of $memberCount (see dungeon.log)"
		}
		Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 500
	}
	# logecho off, and the console closed: each path below opens it for itself.
	Send-Text 'logecho off'; Send-Key $VK_RETURN
	Start-Sleep -Milliseconds 300
	Send-Key $VK_CONSOLE
	Start-Sleep -Milliseconds 400

	# -Minimal: the whole run under the Minimal HUD layout (the party cards).
	# FIRST, before any mode sets its scene up: the switch REBUILDS the HUD, which
	# would close a spellbook -Cast had opened. A first time, out here before the
	# window; the verdict below refuses a PASS unless the cards were actually up.
	if ($Minimal) {
		Write-Host 'switching the HUD to the Minimal layout'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'hudpanel layout minimal'; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Milliseconds 600
	}

	# -Wear: member 0 puts the item on before anything else, so its light is
	# there in every measured frame. The proof it took is the game's own lines:
	# the wear, and a `worn` row in the light readout.
	if ($Wear) {
		Write-Host "member 0 wears $Wear"
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Send-Text "wear $Wear 0"; Send-Key 0x0D
		# Closed a moment so the world runs a few frames with it on: the light
		# readout is the LAST frame's lights, and an open console's frames may
		# not have refreshed them.
		Send-Key 0xC0
		Start-Sleep -Milliseconds 600
		Send-Key 0xC0
		Start-Sleep -Milliseconds 400
		Send-Text 'lights'; Send-Key 0x0D
		Start-Sleep -Milliseconds 600
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Milliseconds 400
		if (-not (Select-String -Path $log -Pattern "wears $Wear" -SimpleMatch -Quiet)) {
			throw "wear $Wear was refused (see dungeon.log)"
		}
		if (-not (Select-String -Path $log -Pattern '\] worn ' -Quiet)) {
			Select-String -Path $log -Pattern 'console: ' | Select-Object -Last 12 |
				ForEach-Object { Write-Host "    $($_.Line)" }
			throw "$Wear is worn but gives no light (no 'worn' row in the light readout)"
		}
	}

	if ($Lights) {
		Write-Host 'scattering 64 test lights over the level'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Send-Text 'lightstress 64'; Send-Key 0x0D
		Start-Sleep -Milliseconds 600
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Milliseconds 400
		$placed = Select-String -Path $log -Pattern 'console: lightstress: (\d+) test lights' |
			Select-Object -Last 1
		$script:stressLights = if ($placed -and $placed.Line -match 'lightstress: (\d+)') { [int]$Matches[1] } else { 0 }
		Write-Host "  $($script:stressLights) test lights placed"
	}

	if ($Wounded) {
		Write-Host 'wounding the party so the regeneration path actually runs'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		# Echo to the log, because a PASS here is only worth anything if the
		# wounding actually happened - and a swallowed keystroke (0xC0 toggles,
		# so one stray press eats every command after it) would leave a run that
		# looks exactly like a clean one. The `party` line below is the evidence.
		Send-Text 'logecho on'; Send-Key 0x0D
		foreach ($m in 0..($memberCount - 1)) {
			# Level the practices first, so the window measures the regeneration
			# tick alone and not a level-up landing in it. (Not because an event
			# may allocate - none may, since the event exemption went; C216.)
			foreach ($s in 'constitution', 'conditioning', 'attunement') {
				Send-Text "setskill $m $s 20"; Send-Key 0x0D
			}
			Send-Text "effect bleed $m 6 2"; Send-Key 0x0D
		}
		# Let the bleed run out, so only the recovery is inside the window.
		Start-Sleep -Seconds 4
		Send-Text 'party'; Send-Key 0x0D
		# Refuse to report on a party that is not actually hurt. Without this the
		# switch could silently degrade into the plain run it exists to replace.
		# Polled, not read after a fixed sleep: `party` answers when the game
		# gets to it (see Wait-NewLogLines).
		$deadline = (Get-Date).AddSeconds(5)
		do {
			$hurt = Select-String -Path $log -Pattern 'hp \d+\.\d+/\d+\.\d+' |
				Where-Object { $_.Line -match 'hp (\d+\.\d+)/(\d+\.\d+)' -and
							   [double]$Matches[1] -lt [double]$Matches[2] }
			if (-not $hurt) { Start-Sleep -Milliseconds 200 }
		} while (-not $hurt -and (Get-Date) -lt $deadline)
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
		if (-not $hurt) { throw 'the party is at full health - the wounding did not land' }
		Write-Host "  wounded: $((($hurt | Select-Object -Last 4).Line -replace '^.*console: ', '') -join '; ')"
	}

	if ($Melee) {
		# ASK THE GAME where the party stands (`pos`), never parse it off the load
		# line: which line a new game ends on depends on the path it took. The
		# console `newgame` (c8c28aa) logs 'New game started in <dungeon> (<level>
		# at X,Z)' and never 'Level ready:', so a regex on $ready found no cell and
		# -Melee died in setup on every run. `pos` answers on every path.
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		$posPattern = 'console: (\d+),(\d+) facing (north|east|south|west)$'
		$posBefore = @(Select-String -Path $log -Pattern $posPattern -EA SilentlyContinue).Count
		Send-Text 'pos'; Send-Key 0x0D
		$posLine = Wait-NewLogLines $posPattern $posBefore
		if ($posLine.Count -eq 0 -or $posLine[-1].Line -notmatch $posPattern) {
			throw 'the console never answered `pos` - there is no party cell to fight beside'
		}
		$px = [int]$Matches[1]; $pz = [int]$Matches[2]
		Write-Host "putting a HELD $MeleeMonster (x$MeleeStrength) beside the party at $px,$pz"
		# HELD FIRST (`freeze hold`): no monster acts, or even notices the party,
		# until alloctest's first ARMED frame lets them go - so the first notice,
		# the first formation pass and the first blow of the process all land
		# inside the window. There is no warm-up: a first blow is a first time
		# every fight pays, and the warm-up this mode used to wait out hid the
		# stat lists TrainDefense built on it and the formation list growing
		# (code-review C35, C71).
		Send-Text 'freeze hold'; Send-Key 0x0D
		if (-not (Wait-LogMatch 'console: freeze held until an alloctest window opens')) {
			throw 'the freeze was not held'
		}
		# The first orthogonal neighbour `spawn` accepts (it refuses a wall or a
		# taken cell, and says so) - no cell of any one level is hardcoded. It is
		# spawned FACING the party (+z is south), as arena.eval does, and `up`, so
		# it is not still rising off the floor when the window opens.
		$spawnedAt = $null
		foreach ($d in @(@(1, 0, 'w'), @(-1, 0, 'e'), @(0, 1, 'n'), @(0, -1, 's'))) {
			$x = $px + $d[0]; $z = $pz + $d[1]
			Send-Text "spawn $MeleeMonster $x $z $($d[2]) $MeleeStrength up"; Send-Key 0x0D
			# Either answer ends the wait; a refusal moves on to the next cell.
			$answer = Wait-NewLogLines "(spawned |spawn: refused ')$MeleeMonster'? at $x,$z\b" 0
			if ($answer.Count -gt 0 -and $answer[-1].Line -match "spawned $MeleeMonster at $x,$z") {
				$spawnedAt = "$x,$z"; break
			}
		}
		if (-not $spawnedAt) { throw "no cell beside $px,$pz would take a $MeleeMonster" }
		# Refuse unless nothing has noticed the party yet: a monster already aware
		# would have run its first notice, and maybe its first blow, outside the
		# window. A few seconds first, so a plan that was going to latch has had
		# the chance (the slowest AI bucket thinks every two).
		Start-Sleep -Seconds 3
		$noticePattern = 'console: hudbars: .* \| noticed (yes|no) \|'
		$noticeBefore = @(Select-String -Path $log -Pattern $noticePattern -EA SilentlyContinue).Count
		Send-Text 'hudbars'; Send-Key 0x0D
		$notice = Wait-NewLogLines $noticePattern $noticeBefore
		if ($notice.Count -eq 0) { throw 'the console never answered `hudbars`' }
		if ($notice[-1].Line -notmatch '\| noticed no \|') {
			throw 'a monster noticed the party before the window opened - the first notice would go unmeasured'
		}
		# NO `tally reset` before this read: the tally has counted since this
		# script's own game began, so `taken` is every blow the party has had,
		# and a reset here would zero the very count being checked. The window's
		# first armed frame restarts the tally itself (Game::UpdateAllocTest).
		$taken = Get-TallyField 'taken'
		if ($taken -gt 0) { throw "the party was struck before the window opened (taken=$taken)" }
		Write-Host "  spawned at $spawnedAt, held: nothing has noticed the party, nothing has struck it"
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	if ($Rest) {
		if ($RestReach) {
			Write-Host "a corridor, a frozen $RestMonster down it with a way through; the party wounded, one member down"
		} else {
			Write-Host "a corridor, a shut door, a $RestMonster behind it; the party wounded, one member down"
		}
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		# The arena's cells are printed, never assumed (it is centred on whatever
		# map is loaded): the party stands on the centre, the corridor runs east-west.
		# The `1` is the height the size check reads: left out it defaults to the
		# length, and an 11-tall box does not fit crypt1's 14x10.
		$arenaPattern = 'console: arena corridor \d+x1  floor \d+,\d+\.\.\d+,\d+  centre (\d+),(\d+)'
		$before = @(Select-String -Path $log -Pattern $arenaPattern -EA SilentlyContinue).Count
		Send-Text 'arena corridor 11 1'; Send-Key 0x0D
		$got = Wait-NewLogLines $arenaPattern $before
		if ($got.Count -eq 0 -or $got[-1].Line -notmatch $arenaPattern) { throw 'the corridor arena was refused (see dungeon.log)' }
		$cx = [int]$Matches[1]; $cz = [int]$Matches[2]
		$doorX = $cx + 2; $monX = $cx + 4
		if ($RestReach) {
			# No door: the way is open, and the monster is FROZEN before it can
			# take it, so it thinks - and finds a path - every time, and never
			# walks up to swing (a landed blow would end the rest).
			$frozen = Get-ConsoleAnswer 'freeze on' 'console: freeze ' # not the echo, `> freeze on`
			if ($frozen -notmatch '^freeze on') { throw "the monsters were not frozen: $frozen" }
		} else {
			$placed = Get-ConsoleAnswer "editor place doors wooden_door $doorX $cz" 'editor place: '
			if ($placed -notmatch "wooden_door at $doorX,$cz") { throw "the door was not placed at $doorX,${cz}: $placed" }
			Send-Text 'mappage close'; Send-Key 0x0D
		}
		# Facing the party (west) and standing at once (`up`), so it can see them
		# down the corridor - through the door's square, which is floor to sight.
		$spawned = Get-ConsoleAnswer "spawn $RestMonster $monX $cz w 1 up" " at $monX,$cz "
		if ($spawned -notmatch "spawned $RestMonster at $monX,$cz") { throw "the $RestMonster was not spawned at $monX,${cz}: $spawned" }
		# Noticed = a monster AWARE of the party and acting on it, which is what
		# makes it search for a way round every think.
		$deadline = (Get-Date).AddSeconds(30)
		do {
			$bars = Get-ConsoleAnswer 'hudbars' 'hudbars: '
			if ($bars -match 'noticed yes') { break }
			Start-Sleep -Milliseconds 500
		} while ((Get-Date) -lt $deadline)
		if ($bars -notmatch 'noticed yes') { throw "the $RestMonster never noticed the party: $bars" }
		Send-Text 'setpool all health 5'; Send-Key 0x0D
		$down = Get-ConsoleAnswer 'setpool 1 health 0' 'health = '
		if ($down -notmatch 'health = 0\.0') { throw "member 1 was not put down: $down" }
		# NOT `rest on`: the rest starts INSIDE the window, from the HUD's button
		# (see the note above). Where it is, read off the game.
		$button = Get-ConsoleAnswer 'rest button' 'rest button: '
		if ($button -notmatch 'rest button: (\d+),(\d+)') { throw "unreadable: $button" }
		$script:restX = [int]$Matches[1]; $script:restY = [int]$Matches[2]
		if ($script:restX -le 0 -or $script:restY -le 0) { throw "the HUD has no Rest button: $button" }
		# (`(world x` and not `rest `: the console's echo of the command itself,
		# `> rest`, would match that.)
		$awake = Get-ConsoleAnswer 'rest' '(world x'
		if ($awake -notmatch '^rest off \(world x') { throw "the party is resting already: $awake" }
		$where = if ($RestReach) { 'no door' } else { "door at $doorX,$cz" }
		Write-Host "  $where, the $RestMonster at $monX,$cz; Rest button at $($script:restX),$($script:restY)"
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	if ($OnHitTypo) {
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		# Where the party stands, from the game (`pos`), as -Melee asks it.
		$posPattern = 'console: (\d+),(\d+) facing (north|east|south|west)$'
		$posBefore = Get-LogMatchCount $posPattern
		Send-Text 'pos'; Send-Key 0x0D
		$posLine = Wait-NewLogLines $posPattern $posBefore
		if ($posLine.Count -eq 0 -or $posLine[-1].Line -notmatch $posPattern) {
			throw 'the console never answered `pos` - there is no party cell to fight beside'
		}
		$px = [int]$Matches[1]; $pz = [int]$Matches[2]
		# FROZEN (`freeze on`), not held: the skeleton never swings back, so the
		# window holds the party's swings and nothing of -Melee's. x400 hp outlasts
		# the warm-up and the window, and `up` skips its rise.
		Send-Text 'freeze on'; Send-Key 0x0D
		$spawnedAt = $null
		foreach ($d in @(@(1, 0, 'w'), @(-1, 0, 'e'), @(0, 1, 'n'), @(0, -1, 's'))) {
			$x = $px + $d[0]; $z = $pz + $d[1]
			Send-Text "spawn skeleton $x $z $($d[2]) 400 up"; Send-Key 0x0D
			$answer = Wait-NewLogLines "(spawned |spawn: refused ')skeleton'? at $x,$z\b" 0
			if ($answer.Count -gt 0 -and $answer[-1].Line -match "spawned skeleton at $x,$z") {
				$spawnedAt = "$x,$z"; break
			}
		}
		if (-not $spawnedAt) { throw "no cell beside $px,$pz would take a skeleton" }
		# CLUBS in the front rank's armed hands, not the starting daggers and torch:
		# a severe fumble DROPS a blade or a torch (balance.cat's default severe
		# table), and that drop still copies the item's id - code-review C212, a
		# later batch's fix, which would fail this mode at random for a reason it
		# does not measure. A club's severe fumble is `wild`; it drops nothing.
		Send-Text 'equip club 0 1'; Send-Key 0x0D
		Send-Text 'equip club 1 0'; Send-Key 0x0D
		Send-Text 'equip club 1 1'; Send-Key 0x0D
		# The typo: `burn` misspelt.
		$typoId = 'brun'
		Send-Text "onhit club $typoId 3 6"; Send-Key 0x0D
		if (-not (Wait-LogMatch "console: onhit club: $typoId 3 6")) { throw 'the club did not take the typo''d on_hit' }
		# WARM-UP until the warning shows: proof a landed club blow rolls the
		# proc, and the party's first swing kept out of the window - that first
		# time is batch 27's -Swing to measure, not this mode.
		$typoPattern = "on-hit proc names effect '$typoId'"
		Send-Text 'autoattack on'; Send-Key 0x0D
		if ((Wait-NewLogLines $typoPattern 0 1 20).Count -eq 0) {
			throw "the party swung for 20 s and no club blow warned about '$typoId'"
		}
		Send-Text 'autoattack hold'; Send-Key 0x0D
		if (-not (Wait-LogMatch 'console: autoattack held until an alloctest window opens')) {
			throw 'the autoattack was not held'
		}
		# Every warning past this count is the window's: the swinging is off until
		# its first armed frame, and the verdict line ends it.
		$script:typoBefore = Get-LogMatchCount $typoPattern
		Write-Host "  a skeleton at $spawnedAt, the club's on_hit '$typoId 3 6'; swinging held for the window"
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	if ($Cast) {
		Write-Host "freezing the world, casting a bolt, and opening member $CastMember's book"
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Send-Text 'timescale 0'; Send-Key 0x0D
		Send-Text "learn $CastMember fire"; Send-Key 0x0D
		Send-Text "learn $CastMember project"; Send-Key 0x0D
		# Enough fire skill that a two-rune cast cannot fumble (Magic.cpp: 35% a rune
		# past the first, less 10% a level) - the run measures a bolt, not luck.
		Send-Text "setskill $CastMember fire 5"; Send-Key 0x0D
		# Kenaz Tiwaz: Kenaz alone is a hand spell now, with nothing in flight.
		Send-Text "cast $CastMember 0 fire project"; Send-Key 0x0D
		Send-Text "book $CastMember"; Send-Key 0x0D
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
		# Refuse to measure unless both actually happened: a swallowed key or a
		# fizzled cast would otherwise leave a run that looks exactly like a
		# clean one. (A book refusal is printed as a Refuse, not 'book open'.)
		if (-not (Wait-LogMatch 'console: cast away')) {
			throw 'the cast did not go off - no bolt is in flight to measure'
		}
		if (-not (Wait-LogMatch 'console: book open: ')) {
			throw 'the spellbook did not open'
		}
	}

	if ($Impact) {
		Write-Host 'going to eval_arena for an open field of fire'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Enter-FrozenArena
		# The room is open from 1,1 to 26,22 (28x24 with a solid border). The
		# monster stands THREE squares north of the party: a bolt's `range` is
		# in METRES (flame's 8 m is 3.2 squares), so at five every bolt went
		# out in open air short of it. In an open room a Fire Burst's force is
		# spent a square or two out, so three is also past its reach back.
		$tx = 14; $tz = 14; $px = 14; $pz = 17
		Send-Text "tp $px $pz"; Send-Key 0x0D
		Send-Text 'face n'; Send-Key 0x0D
		Assert-PartyAt $px $pz
		Send-Text "spawn $ImpactMonster $tx $tz s $ImpactStrength"; Send-Key 0x0D
		if (-not (Wait-LogMatch "spawned $ImpactMonster at $tx,$tz")) {
			throw "the arena would not take a $ImpactMonster at $tx,$tz"
		}
		# Members 0 and 1 cast down OPPOSITE lanes (front-left, front-right),
		# so whichever lane the monster's slot is not in flies past and
		# expires; member 2 throws the blast - but only once the rotation is
		# held below, so the process's first detonation is inside the window.
		# Single-target bolts in the two lanes (Puff of Flame is a hand spell now).
		Send-Text "autocast 0 waterbolt $ImpactEvery"; Send-Key 0x0D
		Send-Text 'autocast 1 waterbolt'; Send-Key 0x0D
		Send-Text 'tally reset'; Send-Key 0x0D
		Write-Host "  casting at a $ImpactMonster (x$ImpactStrength) from $px,$pz; waiting for a hit and an expiry (warm-up)"
		$deadline = (Get-Date).AddSeconds(60)
		while ($true) {
			$c = Get-ImpactCounts
			if ($c.Hits -gt 0 -and $c.Expired -gt 0) { break }
			if ((Get-Date) -gt $deadline) {
				# Into the log: where everything stands, and what each caster's
				# attempts came to (a rotation entry that only fails says so).
				Send-Text 'monsters'; Send-Key 0x0D
				Send-Text 'autocast'; Send-Key 0x0D
				Send-Text 'party'; Send-Key 0x0D
				Start-Sleep -Milliseconds 500
				throw "the warm-up never saw both (last: $script:lastTally)"
			}
			Start-Sleep -Seconds 1
		}
		# EVERY CASTER MUST HAVE CAST. The counts above can both arrive with one
		# entry of the rotation refused throughout (a downed member), and then
		# the lane pattern the window depends on is not the one this header
		# describes. Failures alone prove nothing - a fumble is one.
		$castPattern = 'console:   member \d+ casts \S+: \d+ cast, \d+ failed'
		$castBefore = @(Select-String -Path $log -Pattern $castPattern).Count
		Send-Text 'autocast'; Send-Key 0x0D
		$castRows = Wait-NewLogLines $castPattern $castBefore 2
		if ($castRows.Count -ne 2) { throw "``autocast`` listed $($castRows.Count) entries, not 2" }
		foreach ($r in $castRows) {
			if ($r.Line -match ': 0 cast,') {
				Send-Text 'party'; Send-Key 0x0D
				throw "a caster never cast: $($r.Line -replace '^.*console:\s+', '') (party state is in dungeon.log)"
			}
		}
		# A few more rounds, so each outcome's first time in the PROCESS (a
		# miss line, a sound's first voice) is warm-up rather than window. NOT
		# the first detonation: nothing has burst yet, and nothing will until the
		# window opens (C49).
		Start-Sleep -Seconds 4
		# THEN A FRESH TARGET. What the warm-up absorbs must be a first time
		# for the process, never a first time for a MONSTER - every monster
		# in play is new once, so its first burn (an effects list growing from
		# empty) or first threat entry is a steady cost of casting, not
		# warm-up. Warming up and measuring on one target hid exactly that. So
		# the party steps four squares west, out of the line of the worn-in
		# target, and a new one is spawned three squares ahead of it, never
		# touched by anything before the window opens.
		#
		# AND NOTHING FIRES AT IT UNTIL THE WINDOW OPENS, so the rotation is
		# HELD FIRST. The guard skips a 120-frame warm-up after the console
		# shuts, and at a cast every 0.4 s the fresh target's first hits landed
		# in that gap - or, with the hold typed after the move, in the half
		# second the script spent typing it. Both passed a run that should have
		# failed. A held rotation is released by alloctest's first ARMED frame,
		# which also restarts the tally, so the count read afterwards is the
		# window's. The pause lets bolts already in flight land on the old one.
		Send-Text 'autocast hold'; Send-Key 0x0D
		Start-Sleep -Seconds 1
		# NOW the Fire Burst joins, into the HELD rotation, so its first cast and
		# the process's first detonation come with the window. Refuse if anything
		# has burst already: that first time would be outside it again.
		Send-Text 'autocast 2 firebolt_burst'; Send-Key 0x0D
		# (Not `$burst`: PowerShell names ignore case, and that is the -Burst switch.)
		$blastsSoFar = Get-TallyField 'blasts'
		if ($blastsSoFar -gt 0) { throw "a blast went off before the window ($blastsSoFar) - the first detonation would go unmeasured" }
		$px -= 4; $tx -= 4
		Send-Text "tp $px $pz"; Send-Key 0x0D
		Assert-PartyAt $px $pz
		Send-Text "spawn $ImpactMonster $tx $tz s $ImpactStrength"; Send-Key 0x0D
		if (-not (Wait-LogMatch "spawned $ImpactMonster at $tx,$tz")) {
			throw "the arena would not take a fresh $ImpactMonster at $tx,$tz"
		}
		# Refuse unless it really is untouched: `monsters` lists a live effect
		# in brackets after the hp, and a burn caught early is exactly the
		# thing this step exists to keep out of the warm-up.
		Start-Sleep -Milliseconds 500
		$before = @(Select-String -Path $log -Pattern "console:   $ImpactMonster @ $tx,$tz ").Count
		Send-Text 'monsters'; Send-Key 0x0D
		$row = Wait-NewLogLines "console:   $ImpactMonster @ $tx,$tz " $before
		if ($row.Count -eq 0) { throw "``monsters`` did not list the fresh $ImpactMonster" }
		# (Past the "[info ]" the log line opens with, which is a bracket too.)
		$listed = $row[-1].Line -replace '^.*console: ', ''
		if ($listed -match '\[') { throw "the fresh target was touched before the window: $listed" }
		Write-Host "  fresh $ImpactMonster at $tx,$tz, party at $px,$pz, untouched"
		# AND A FRESH CRATE beside it, in the Fire Burst's ring: a blast leaves a
		# piece of dungeon alight, and its burn ticks every frame until it breaks
		# (TickBreakables). Fresh for the same reason as the target - a crate's
		# first burn and its break are a steady cost of fighting beside one. It is
		# off the bolts' column, so it never stands between a bolt and its mark.
		$cx = $tx + 1
		Send-Text "editor place decorations crate $cx $tz"; Send-Key 0x0D
		Send-Text 'mappage close'; Send-Key 0x0D
		if (-not (Wait-LogMatch "editor place: crate at $cx,$tz")) {
			throw "the arena would not take a crate at $cx,$tz"
		}
		Send-Text "breakables $cx $tz"; Send-Key 0x0D
		$crate = Wait-NewLogLines "console:   decoration crate @ $cx,$tz " 0
		if ($crate.Count -eq 0) { throw "the crate at $cx,$tz is not breakable" }
		Write-Host "  fresh $($crate[-1].Line -replace '^.*console:\s+', '')"
		# AND A LIT BRAZIER THAT BREAKS INSIDE THE WINDOW: a wrecked fixture puts
		# its fire out and thins the haze it fed (DouseFixture) - the haze texture
		# is rewritten in place, which used to be a whole new texture built
		# mid-frame. Braziers shrug off fire, so it gets a POISON (earth) that eats
		# its 30 hp in about five seconds: applied as the very last thing before
		# the console shuts, so the break lands a few seconds into the window.
		$bx = $tx - 3
		Send-Text "editor place fixtures brazier $bx $tz"; Send-Key 0x0D
		Send-Text 'mappage close'; Send-Key 0x0D
		if (-not (Wait-LogMatch "editor place: brazier at $bx,$tz")) {
			throw "the arena would not take a brazier at $bx,$tz"
		}
		Send-Text "breakables $bx $tz poison 6 30"; Send-Key 0x0D
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	# -Burst: shots at the party from a world frame (see the note at the top).
	if ($Burst) {
		Write-Host 'going to an emptied eval_arena: burst bolts at the party'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Enter-FrozenArena
		# Emptied, so nothing but the party is there to be hit, and a flung-back
		# bolt flies home into open air. The centre is read back, not assumed.
		$arenaBefore = @(Select-String -Path $log -Pattern 'console: arena open ').Count
		Send-Text 'arena open 9 9'; Send-Key 0x0D
		$arena = Wait-NewLogLines 'console: arena open ' $arenaBefore 1 30
		if ($arena.Count -eq 0 -or $arena[-1].Line -notmatch 'centre (\d+),(\d+)') {
			throw 'the arena was not carved (no `arena open` line with a centre)'
		}
		$bpx = [int]$Matches[1]; $bpz = [int]$Matches[2]
		$bfz = $bpz - 1 # the square ahead, facing north: where every bolt flies from
		Send-Text "tp $bpx $bpz"; Send-Key 0x0D
		Send-Text 'face n'; Send-Key 0x0D
		Assert-PartyAt $bpx $bpz
		# THE PARTY BAR'S EFFECT STRIP IS PRE-GROWN, and that is a KNOWN DEFECT
		# stood aside, not a warm-up: the strip builds an icon widget the first
		# time a member shows N effects (code-review C219), so the window's first
		# burn on Sera and Tilo allocated there, in the HUD, whatever brought the
		# effect. C219 is batch 41's (Warm(fx::kMaxEffects), judged by its own
		# -Effects mode), so until it lands every member shows two effects for a
		# moment here - the most any shows in the window (a ward and a burn) - and
		# `heal` clears them. DROP THIS once C219 is in.
		foreach ($m in 0..3) {
			Send-Text "effect burn $m 0.01 60"; Send-Key 0x0D
			Send-Text "effect bleed $m 0.01 60"; Send-Key 0x0D
		}
		Start-Sleep -Milliseconds 800 # frames, so the HUD lays the icons out
		Send-Text 'heal'; Send-Key 0x0D
		# One charge each on the two members of slot 0's lane (front-left and
		# rear-left): the first bolt to connect is turned whichever it picks, and
		# of the next two at most one more can be.
		Send-Text 'effect windward 0 1 60'; Send-Key 0x0D
		Send-Text 'effect windward 2 1 60'; Send-Key 0x0D
		# HELD before the first entry goes in: the world runs with the console
		# open, and an entry fires as soon as it is added.
		Send-Text 'autocast off'; Send-Key 0x0D
		Send-Text 'autocast hold'; Send-Key 0x0D
		# The magus's burst bolt is 14 strong: 7 weakens it to half, 14 spends it,
		# 21 flings it back with half.
		Send-Text "autocast bolt firebolt_burst $bpx $bfz 0 2"; Send-Key 0x0D
		Send-Text "autocast bolt firebolt_burst $bpx $bfz 0 repel 7 1"; Send-Key 0x0D
		Send-Text "autocast bolt firebolt_burst $bpx $bfz 0 repel 14 1"; Send-Key 0x0D
		Send-Text "autocast bolt firebolt_burst $bpx $bfz 0 repel 21 1"; Send-Key 0x0D
		$boltPattern = 'console:   bolt firebolt_burst from \d+,\d+ in slot 0'
		$boltBefore = @(Select-String -Path $log -Pattern $boltPattern).Count
		Send-Text 'autocast'; Send-Key 0x0D
		$boltRows = Wait-NewLogLines $boltPattern $boltBefore 4
		if ($boltRows.Count -ne 4) { throw "``autocast`` listed $($boltRows.Count) bolt entries, not 4" }
		foreach ($r in $boltRows) {
			if ($r.Line -notmatch ': 0 shot, 0 failed') {
				throw "an entry fired before the window: $($r.Line -replace '^.*console:\s+', '')"
			}
		}
		Write-Host "  party at $bpx,$bpz facing north, four bolt entries from $bpx,$bfz, held"
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	# -Swing: the party's own swing and a severe fumble (see the note at the top).
	if ($Swing) {
		Write-Host 'going to an emptied eval_arena: the party swings, and a loaded die drops a torch'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Enter-FrozenArena
		# Emptied, so the skeleton below is the only thing in reach. The centre is
		# read back, not assumed.
		$arenaBefore = @(Select-String -Path $log -Pattern 'console: arena open ').Count
		Send-Text 'arena open 9 9'; Send-Key 0x0D
		$arena = Wait-NewLogLines 'console: arena open ' $arenaBefore 1 30
		if ($arena.Count -eq 0 -or $arena[-1].Line -notmatch 'centre (\d+),(\d+)') {
			throw 'the arena was not carved (no `arena open` line with a centre)'
		}
		$spx = [int]$Matches[1]; $spz = [int]$Matches[2]
		$sfz = $spz - 1 # the square ahead, facing north
		Send-Text "tp $spx $spz"; Send-Key 0x0D
		Send-Text 'face n'; Send-Key 0x0D
		Assert-PartyAt $spx $spz
		# FROZEN (Enter-FrozenArena's `freeze on`): it takes the blows and never
		# swings back, which is -Melee's job. Toughened, so it outlives the window.
		Send-Text "spawn skeleton $spx $sfz s 50 up"; Send-Key 0x0D
		if (-not (Wait-LogMatch "spawned skeleton at $spx,$sfz")) {
			throw "the arena would not take a skeleton at $spx,$sfz"
		}
		# Sera's lit torch (member 1, hand 1), part-burnt, with the die loaded for
		# its swing: a severe fumble, and the default severe table is `drop`.
		Send-Text 'torch charge 1 1 300'; Send-Key 0x0D
		Send-Text 'fumble severe 1 1'; Send-Key 0x0D
		if (-not (Wait-LogMatch 'console: fumble: the next swing by member 1 hand 1 fumbles severely')) {
			throw 'the die was not loaded for Sera''s torch'
		}
		# HELD: nobody swings until alloctest's first armed frame.
		Send-Text 'autoattack hold'; Send-Key 0x0D
		if (-not (Wait-LogMatch 'console: autoattack held until an alloctest window opens')) {
			throw 'the party''s swinging was not held'
		}
		# Refuse unless the party has not swung at all this session: a first swing
		# outside the window would be a first time gone unmeasured. (The tally has
		# counted since this script's own game began; the window restarts it.)
		$swungAlready = Get-TallyField 'swings'
		if ($swungAlready -gt 0) { throw "the party swung before the window opened (swings=$swungAlready)" }
		Write-Host "  party at $spx,$spz facing north, a frozen skeleton at $spx,$sfz, Sera's torch loaded to fumble, swinging held"
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	# -Exit: the log's Help button, crypt1's exit stair and a pit (see the note at
	# the top). crypt1 frozen: its sleepers stay asleep, and crypt2's too once the
	# party lands there (the freeze is the world's, not the level's).
	if ($Exit) {
		Write-Host "crypt1: its exit stair south of the start, a pit north of it, the log's Help button"
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Enter-FrozenArena 'crypt1'
		Send-Text 'tp 7 7'; Send-Key 0x0D
		Send-Text 'face n'; Send-Key 0x0D
		Assert-PartyAt 7 7
		# Over crypt2's 7,6: AddStairAt authors the ceiling hole there too.
		$pit = Get-ConsoleAnswer 'stairadd pit 7 6' 'stair pit '
		if ($pit -notmatch 'stair pit placed on crypt1 at 7,6') { throw "the pit was not placed at 7,6: $pit" }
		$help = Get-ConsoleAnswer 'messages help' 'messages help: '
		if ($help -notmatch 'messages help: (\d+),(\d+) presses=(\d+)') { throw "unreadable: $help" }
		$script:helpX = [int]$Matches[1]; $script:helpY = [int]$Matches[2]
		$script:helpBefore = [int]$Matches[3]
		# Arrivals on crypt2 counted from here: the fall's is the only one to come.
		$script:crypt2Before = Get-LogMatchCount '^\[info \] Level ready: crypt2 at 7,6$'
		Write-Host "  party at 7,7 facing north, the exit at 7,8, the pit at 7,6; Help at $($script:helpX),$($script:helpY)"
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	# -Hand: THE HAND SPELLS (spell-updates Phase 8). A tier-1 spell changes the
	# WORLD AHEAD and the ITEMS IN HAND, not a monster: Kenaz lights a held torch
	# (an item renamed in its slot) and the wall torch ahead (a fire's state, the
	# turbidity grid refreshed in place on the GPU), Laguz fills a held waterskin
	# a step at a time and then douses that torch (a smoke effect on the fire),
	# Ansuz flares it, and Berkano conjures a pebble into an empty hand and then
	# at the feet (a floor drop). None of that is a bolt, so -Cast and -Impact
	# never reach it. crypt1's torch at 4,3 is the target, the monsters frozen;
	# `autocast` casts each from a world frame, so the window holds the casts.
	#
	# The ITEM paths happen once each before a slot is used up (a full skin
	# stops filling, a lit torch stays lit), so the warm-up runs the whole
	# rotation, then the rotation is HELD and FRESH items go back in hand - an
	# unlit torch, an empty skin, an empty hand - and the window's first armed
	# frame releases it. So the first light, both fills and a hand landing fall
	# inside the window, and every cast after them is a fire change or a drop.
	if ($Hand) {
		Write-Host "going to crypt1's wall torch for the hand spells"
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Enter-FrozenArena 'crypt1'
		Send-Text 'tp 4 3'; Send-Key 0x0D
		Send-Text 'face n'; Send-Key 0x0D
		Assert-PartyAt 4 3
		$handKit = {
			Send-Text 'equip torch 2 1'; Send-Key 0x0D
			Send-Text 'equip waterskin_empty 3 1'; Send-Key 0x0D
			Send-Text 'equip none 0 0'; Send-Key 0x0D
		}
		& $handKit
		Send-Text 'learn 0 earth'; Send-Key 0x0D
		Send-Text 'learn 1 air'; Send-Key 0x0D
		Send-Text 'learn 2 fire'; Send-Key 0x0D
		Send-Text 'learn 3 water'; Send-Key 0x0D
		Send-Text 'autocast 2 flame 0.3'; Send-Key 0x0D
		Send-Text 'autocast 3 splash'; Send-Key 0x0D
		Send-Text 'autocast 1 gust'; Send-Key 0x0D
		Send-Text 'autocast 0 rock'; Send-Key 0x0D
		Write-Host '  warming the rotation up'
		Start-Sleep -Seconds 6
		$castPattern = 'console:   member \d+ casts \S+: \d+ cast, \d+ failed'
		$castBefore = @(Select-String -Path $log -Pattern $castPattern).Count
		Send-Text 'autocast hold'; Send-Key 0x0D
		Send-Text 'autocast'; Send-Key 0x0D
		$script:handRows = Wait-NewLogLines $castPattern $castBefore 4
		if ($script:handRows.Count -ne 4) { throw "``autocast`` listed $($script:handRows.Count) entries, not 4" }
		foreach ($r in $script:handRows) {
			if ($r.Line -match ': 0 cast,') { throw "a hand spell never cast in the warm-up: $($r.Line -replace '^.*console:\s+', '')" }
		}
		Start-Sleep -Milliseconds 500
		& $handKit
		Start-Sleep -Milliseconds 300
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	# -Light: THE SOWILO LIGHTS (lighting-updates Phase 6). Each member casts a
	# different one from a world frame (`autocast`, the harness pays the mana):
	# a plain Firelight and Tidelight (a light effect landing, refreshed each
	# time - the per-school stacking), an Ingwaz Skylight (one bigger light), and
	# a Hagalaz Firelight FLARE at a mummy beside the party (the flash, the dazzle
	# landing on a monster, Firelight's scorch and the flammable mummy catching),
	# and a Stonelight (6f: the stone SET DOWN, replacing the last in its square,
	# and the squares it reaches mapped; 6g: the monster tracks in its reach
	# shown - three planted up the doorway, since a frozen world walks none).
	# The world is frozen, so the mummy only stands there.
	if ($Light) {
		Write-Host 'casting the light spells in crypt1, a mummy beside the party'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Enter-FrozenArena 'crypt1'
		Send-Text 'tp 7 7'; Send-Key 0x0D
		Send-Text 'face n'; Send-Key 0x0D
		Assert-PartyAt 7 7
		# Beside the party, so Firelight's scorch (and the mummy catching fire) lands in
		# the window too; tough enough (x400) to outlive it.
		Send-Text 'spawn mummy 7 6 s 400'; Send-Key 0x0D
		Send-Text 'tracks clear'; Send-Key 0x0D
		foreach ($z in 4, 5, 6) { Send-Text "tracks add 7 $z s"; Send-Key 0x0D }
		foreach ($m in 0, 1, 2, 3) {
			foreach ($s in 'fire', 'water', 'air', 'earth', 'light', 'multiple', 'explode') {
				Send-Text "learn $m $s"; Send-Key 0x0D
			}
		}
		Send-Text 'setskill 0 fire 10'; Send-Key 0x0D
		Send-Text 'setskill 1 water 10'; Send-Key 0x0D
		Send-Text 'setskill 2 air 10'; Send-Key 0x0D
		Send-Text 'setskill 3 fire 10'; Send-Key 0x0D
		Send-Text 'setskill 3 earth 10'; Send-Key 0x0D
		Send-Text 'autocast 0 firelight 0.5'; Send-Key 0x0D
		Send-Text 'autocast 1 tidelight'; Send-Key 0x0D
		Send-Text 'autocast 2 skylight_bright'; Send-Key 0x0D
		Send-Text 'autocast 3 firelight_flare'; Send-Key 0x0D
		Send-Text 'autocast 3 stonelight'; Send-Key 0x0D
		Write-Host '  warming the rotation up'
		Start-Sleep -Seconds 6
		$castPattern = 'console:   member \d+ casts \S+: \d+ cast, \d+ failed'
		$castBefore = @(Select-String -Path $log -Pattern $castPattern).Count
		Send-Text 'autocast hold'; Send-Key 0x0D
		Send-Text 'autocast'; Send-Key 0x0D
		$script:lightRows = Wait-NewLogLines $castPattern $castBefore 5
		if ($script:lightRows.Count -ne 5) { throw "``autocast`` listed $($script:lightRows.Count) entries, not 5" }
		foreach ($r in $script:lightRows) {
			if ($r.Line -match ': 0 cast,') { throw "a light spell never cast in the warm-up: $($r.Line -replace '^.*console:\s+', '')" }
		}
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	if ($Sheet) {
		Write-Host 'opening the sheet with a rune and a blade in the pack'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		# The sheet is a floating window now (ui-panels P3b): the clicks below aim
		# at its DEFAULT spot and size, so put it back there first.
		Send-Text 'hudpanel reset'; Send-Key 0x0D
		# A new party's pack holds three pieces of armour (slots 0-2), so these
		# land in slots 3 and 4 - the cells the clicks below aim at.
		Send-Text 'give rune_fire 0'; Send-Key 0x0D
		Send-Text 'give flamebrand 0'; Send-Key 0x0D
		# Skills part-way to their next level, so the Skills tab the cycle passes
		# through draws its framed progress bars (ui-bars-updates) - a new party
		# has trained nothing, and an empty tab measured nothing there.
		Send-Text 'setskill 0 blade 1.5'; Send-Key 0x0D
		Send-Text 'setskill 0 fire 2.3'; Send-Key 0x0D
		Send-Text 'setskill 0 conditioning 0.6'; Send-Key 0x0D
		Send-Text 'sheet 0'; Send-Key 0x0D
		# WARM-UP: one open of the dialog, and a moment for it to draw, bakes its
		# fonts and glyphs - a first time for the process, outside the window.
		Send-Text 'itemdetails flamebrand 1.4'; Send-Key 0x0D
		Start-Sleep -Seconds 1
		Send-Text 'itemdetails off'; Send-Key 0x0D
		$script:opensBefore = Get-DetailOpens
		if ($script:opensBefore -le 0) { throw 'the warm-up never opened the item details dialog' }
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
		if (-not (Wait-LogMatch 'console: sheet open: ')) {
			throw 'the sheet did not open'
		}
		# Where the two cells are, from the window's own size (the sheet lays out
		# in fractions of it): backpack slots 3 and 4 of the default layout.
		$rc = New-Object HarnessWin+RECT
		[HarnessWin]::GetClientRect($hwnd, [ref]$rc) | Out-Null
		$script:runeX = [int]($rc.Right * 0.6675); $script:bladeX = [int]($rc.Right * 0.72)
		$script:slotY = [int]($rc.Bottom * 0.5033)
	}

	if ($All) {
		Write-Host 'opening the sheet, and the party window from its All button'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		# Every click below is read off the game, at the default layout.
		Send-Text 'hudpanel reset'; Send-Key 0x0D
		Send-Text 'give rune_fire 0'; Send-Key 0x0D # an item for the Inventory cards to draw
		Send-Text 'sheet 0'; Send-Key 0x0D
		Start-Sleep -Milliseconds 500
		$line = Get-ConsoleAnswer 'sheet status' 'console: sheet all: '
		if ($line -notmatch 'sheet all: (\d+),(\d+)') { throw "unreadable: $line" }
		$script:allX = [int]$Matches[1]; $script:allY = [int]$Matches[2]
		# Member 0's portrait: the left end of the party bar, as tall as the bar.
		$before = @(Select-String -Path $log -Pattern 'console:   party ').Count
		Send-Text 'hudpanel list'; Send-Key 0x0D
		$rows = Wait-NewLogLines 'console:   party ' $before
		if ($rows.Count -eq 0 -or $rows[-1].Line -notmatch 'px (-?\d+),(-?\d+) (\d+)x(\d+)') {
			throw 'no party bar rect from `hudpanel list`'
		}
		$script:portraitX = [int]$Matches[1] + [int]([int]$Matches[4] * 0.45)
		$script:portraitY = [int]$Matches[2] + [int]([int]$Matches[4] * 0.5)
		Send-Key 0xC0
		Start-Sleep -Milliseconds 400
		# The window, open, says where its stones and its cards are.
		Send-Click $script:allX $script:allY
		Start-Sleep -Milliseconds 800
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		$script:stones = @()
		for ($i = 0; $i -lt 5; $i++) {
			$line = Get-ConsoleAnswer "inventory stone $i" "console: inventory stone ${i}: "
			if ($line -notmatch ': (\d+),(\d+)$') { throw "unreadable: $line" }
			$script:stones += [pscustomobject]@{ X = [int]$Matches[1]; Y = [int]$Matches[2] }
		}
		$script:cardPoints = @()
		for ($m = 0; $m -lt $memberCount; $m++) { $script:cardPoints += Get-InventorySlotPoint $m 0 }
		Send-Text 'inventory off'; Send-Key 0x0D
		Send-Text 'sheet 0'; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		# WARM-UP: one whole cycle, which also checks that every click landed -
		# the window opened, closed, and the portrait brought the sheet back.
		Invoke-AllCycle
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		$status = Get-InventoryStatus
		$sheetLine = Get-ConsoleAnswer 'sheet status' 'console: sheet: '
		if ($status -notmatch 'opens=(\d+)' -or [int]$Matches[1] -lt 2 -or
			$status -notmatch '^inventory: closed' -or $sheetLine -notmatch '^sheet: open') {
			throw "the warm-up cycle went wrong ($status; $sheetLine) - All $($script:allX),$($script:allY), " +
				"portrait $($script:portraitX),$($script:portraitY)"
		}
		$status -match 'opens=(\d+)' | Out-Null
		$script:allOpensBefore = [int]$Matches[1]
		Write-Host "  warm-up cycle ok (All at $($script:allX),$($script:allY))"
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	if ($Panels) {
		Write-Host 'resetting the HUD layout and warming the panel drags up'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'hudpanel reset'; Send-Key 0x0D
		Send-Text 'hudpanel lock off'; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Milliseconds 600
		# Where the grabs are, read off the docks' OWN rects (`hudpanel list`), so
		# a change to the default layout - the party bar grew taller once and the
		# Movement dock slid down under a fixed grab point - cannot make the drags
		# miss: a point in the Movement dock's title row, and just inside the
		# Hands dock's bottom-right corner (the resize wedge).
		$rc = New-Object HarnessWin+RECT
		[HarnessWin]::GetClientRect($hwnd, [ref]$rc) | Out-Null
		$moveRect = Get-PanelRow 'move'
		$handsRect = Get-PanelRow 'hands'
		if ($moveRect -notmatch 'px (-?\d+),(-?\d+) (\d+)x(\d+)') { throw "no move dock rect: $moveRect" }
		$script:moveX = [int]$Matches[1] + [int]([int]$Matches[3] * 0.25)
		$script:moveY = [int]$Matches[2] + 12
		if ($handsRect -notmatch 'px (-?\d+),(-?\d+) (\d+)x(\d+)') { throw "no hands dock rect: $handsRect" }
		$script:gripX = [int]$Matches[1] + [int]$Matches[3] - 4
		$script:gripY = [int]$Matches[2] + [int]$Matches[4] - 4
		$script:pullX = $script:gripX - [int]($rc.Right * 0.0235)
		$script:pullY = $script:gripY - [int]($rc.Bottom * 0.0275)
		# Below the inventory window's default rect (0.23..0.77 down) and above the
		# log footer: a grab landing ON the window would act on its slots instead.
		$script:awayX = [int]($rc.Right * 0.30); $script:awayY = [int]($rc.Bottom * 0.80)
		# THE TRAY (ui-updates Phase 8): the Movement dock's MINIMIZE is the
		# top-right Ctrl button, with RESET one button to its left; the tray's
		# button for it then heads the column, right edges level, in the strip
		# the dock's default spot starts under (GameUI::DockColumnTop: padding 0.3 em,
		# a 1.4 em button - HudTray.h - and a 0.5 em gap, so the button's centre
		# is 1 em in from the dock's right and 1.5 em above its top). The button
		# side and so the em come from `hudpanel list`.
		$gripRow = @(Select-String -Path $log -Pattern 'console: hud layout .*grip (\d+)px')[-1].Line
		if ($gripRow -notmatch 'grip (\d+)px') { throw "no grip size: $gripRow" }
		$script:grip = [int]$Matches[1]
		$em = $script:grip / 1.2
		if ($moveRect -notmatch 'px (-?\d+),(-?\d+) (\d+)x(\d+)') { throw "no move dock rect: $moveRect" }
		$mLeft = [int]$Matches[1]; $mTop = [int]$Matches[2]; $mRight = $mLeft + [int]$Matches[3]
		$script:hideX = $mRight - 6; $script:hideY = $mTop + 6
		$script:resetX = $mRight - $script:grip - 7; $script:resetY = $mTop + 6
		$script:trayX = [int]($mRight - 1.0 * $em); $script:trayY = [int]($mTop - 1.5 * $em)
		# WARM-UP: one drag and one pull outside the window - the pull's new scale
		# bakes a font size, a first time for the process - and one trip through
		# the tray. Then back to default.
		Send-Drag $script:moveX $script:moveY $script:awayX $script:awayY
		Send-Drag $script:awayX $script:awayY $script:moveX $script:moveY
		Send-Drag $script:gripX $script:gripY $script:pullX $script:pullY
		Invoke-TrayTrip
		Start-Sleep -Milliseconds 400
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'hudpanel reset'; Send-Key 0x0D
		# The party window stays open through the window, so its draw (every
		# card, every frame) is measured too. Parked small at the top-left, clear
		# of both grabs above and of the away point (its default size covers it).
		Send-Text 'hudpanel inventory 0.05 0.02 0.6'; Send-Key 0x0D
		Send-Text 'inventory'; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Milliseconds 400
	}

	if ($Items) {
		Write-Host "going to eval_arena: $WarmItem warms up, $MeasureItem is measured"
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Enter-FrozenArena
		# Facing north across open floor at 14,17 (the room is open from 1,1 to
		# 26,22), so the square ahead takes a drop. WALKED onto, not teleported:
		# `tp` reveals nothing, and a drop only lands on a square the party has
		# SEEN - an unseen one falls back to its feet, out of view, and the first
		# run of this lifted nothing for exactly that reason. A step reveals the
		# eight squares round where it lands.
		Send-Text 'tp 14 18'; Send-Key 0x0D
		Send-Text 'face n'; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Milliseconds 400
		Send-Key 0x57 # W: one step forward
		Start-Sleep -Seconds 1
		Send-Key 0xC0
		Start-Sleep -Milliseconds 400
		Assert-PartyAt 14 17
		# Whoever has two free pack slots carries them: a Continued eval save can
		# leave any member's pack full.
		$status = Get-InventoryStatus
		$member = -1
		for ($m = 0; $m -lt $memberCount -and $member -lt 0; $m++) {
			if (@(Get-PackSlots $status $m | Where-Object { $_ -eq '-' }).Count -ge 2) { $member = $m }
		}
		if ($member -lt 0) { throw "no member has two free pack slots: $status" }
		Send-Text "give $WarmItem $member"; Send-Key 0x0D
		Send-Text "give $MeasureItem $member"; Send-Key 0x0D
		Start-Sleep -Milliseconds 300
		$status = Get-InventoryStatus
		$warmSlot = Get-PackSlot $status $member $WarmItem
		$measureSlot = Get-PackSlot $status $member $MeasureItem
		Write-Host "  member $member carries $WarmItem in slot $warmSlot, $MeasureItem in slot $measureSlot"
		$rc = New-Object HarnessWin+RECT
		[HarnessWin]::GetClientRect($hwnd, [ref]$rc) | Out-Null
		$script:clientW = [double]$rc.Right; $script:clientH = [double]$rc.Bottom
		# The window opens (on its Inventory tab) and lays itself out before its
		# slots can be asked where they are. PARKED small at the top-left: at its
		# default size it covers the floor point below.
		Send-Text 'hudpanel reset'; Send-Key 0x0D
		Send-Text 'hudpanel inventory 0.05 0.02 0.6'; Send-Key 0x0D
		Send-Text 'inventory'; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Milliseconds 600
		Send-Key 0xC0
		Start-Sleep -Milliseconds 400
		$script:warmPoint = Get-InventorySlotPoint $member $warmSlot
		$script:measurePoint = Get-InventorySlotPoint $member $measureSlot
		# THE FLOOR POINT, from the camera: a 70 degree vertical lens with the eye
		# 1.55 m up (kEyeHeight), level. A pixel ndc units below the centre sees
		# the floor at 1.55 / (ndc * tan 35) metres. 3.3 m is the FAR quarter of
		# the square ahead (2.5 to 3.75), far enough that the lift - which
		# samples the ray at the item's own drawn middle, a rune's 0.016 m (it
		# was 0.23 m before code-review C359), so about 3.27 m out - is still in
		# it. Left of centre puts both in the west half.
		$ndc = (1.55 / 3.3) / [math]::Tan(35 * [math]::PI / 180)
		$script:floorX = [int]($script:clientW * 0.40)
		$script:floorY = [int]($script:clientH * (0.5 + $ndc / 2))
		Send-Text 'tally reset'; Send-Key 0x0D
		Send-Text 'inventory'; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Seconds 1
		# WARM-UP, and the check that every click lands where it is aimed: a
		# first drop's sound voice and the like are first times for the PROCESS.
		Invoke-ItemRoundTrip $script:warmPoint
		Start-Sleep -Milliseconds 500
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		$drops = Get-TallyField 'drops'
		$lifts = Get-LastTallyField 'lifts'
		$status = Get-InventoryStatus
		if ($drops -ne 1 -or $lifts -ne 1 -or $status -notmatch 'held=none' -or
			(Get-PackSlot $status $member $WarmItem) -ne $warmSlot) {
			throw "the warm-up round trip went wrong (drops=$drops lifts=$lifts; $status) - " +
				"slot $($script:warmPoint.X),$($script:warmPoint.Y), floor $($script:floorX),$($script:floorY)"
		}
		Write-Host "  warm-up round trip ok (floor point $($script:floorX),$($script:floorY))"
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	# -Throw (ui-updates Phase 10): THROWING, by clicks - a lift off the floor,
	# a click above the floor's horizon that throws it, the flight, the wall it
	# hits and the landing. The party stands one square back from eval_arena's
	# north wall (14,2 facing north), so every throw hits the wall and comes
	# down in the square ahead - in its FIRST free quarter, slot 0, which facing
	# north is the far-left one: exactly -Items' floor point. So the loop needs
	# no feedback: lift there, throw high, wait out throw_interval, again.
	if ($Throw) {
		Write-Host "going to eval_arena's north wall with a $ThrowItem"
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Enter-FrozenArena
		Send-Text 'tp 14 3'; Send-Key 0x0D
		Send-Text 'face n'; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Milliseconds 400
		Send-Key 0x57 # W: one step forward, which reveals the squares round it
		Start-Sleep -Seconds 1
		Send-Key 0xC0
		Start-Sleep -Milliseconds 400
		Assert-PartyAt 14 2
		$rc = New-Object HarnessWin+RECT
		[HarnessWin]::GetClientRect($hwnd, [ref]$rc) | Out-Null
		$script:clientW = [double]$rc.Right; $script:clientH = [double]$rc.Bottom
		# The floor point as -Items works it out (see there), and a point well
		# above the horizon: the ray never meets the floor, so it is a throw.
		$ndc = (1.55 / 3.3) / [math]::Tan(35 * [math]::PI / 180)
		$script:floorX = [int]($script:clientW * 0.40)
		$script:floorY = [int]($script:clientH * (0.5 + $ndc / 2))
		$script:skyX = [int]($script:clientW * 0.50)
		$script:skyY = [int]($script:clientH * 0.30)
		# The first rock comes from nowhere: the leader throws one, and it lands
		# where every later one will.
		Send-Text 'tally reset'; Send-Key 0x0D
		Send-Text "throw $ThrowItem"; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Milliseconds 1500
		# WARM-UP, and the check that the loop's clicks land: one whole cycle by
		# hand. A first throw's sound voice and the like are first times for the
		# PROCESS, not steady costs.
		Send-Click $script:floorX $script:floorY
		Start-Sleep -Milliseconds 300
		Send-Click $script:skyX $script:skyY
		Start-Sleep -Milliseconds 1500
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		$throws = Get-TallyField 'throws'
		$lifts = Get-LastTallyField 'lifts'
		$landed = (Get-LastTallyField 'throwlandings') + (Get-LastTallyField 'throwstrikes')
		if ($throws -ne 2 -or $lifts -ne 1 -or $landed -ne 2) {
			throw "the warm-up throw went wrong (throws=$throws lifts=$lifts landed=$landed) - " +
				"floor $($script:floorX),$($script:floorY), sky $($script:skyX),$($script:skyY)"
		}
		Write-Host "  warm-up throw ok (floor point $($script:floorX),$($script:floorY))"
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	# -Glass: eval_arena, the party one square back from the north wall, and the
	# glass kind on the square between them, in full view.
	if ($Glass) {
		Write-Host "standing in front of a $GlassKind in eval_arena"
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Enter-FrozenArena
		Send-Text 'tp 14 3'; Send-Key 0x0D
		Send-Text 'face n'; Send-Key 0x0D
		Start-Sleep -Milliseconds 400
		Assert-PartyAt 14 3
		Send-Text "editor place $GlassCategory $GlassKind 14 2"; Send-Key 0x0D
		Start-Sleep -Milliseconds 600
		$placed = Select-String -Path $log -Pattern "console: editor place: $GlassKind at 14,2" -SimpleMatch -Quiet
		if (-not $placed) { throw "the $GlassKind was not placed at 14,2 (see the message log)" }
		Send-Text 'editor off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console
		Start-Sleep -Milliseconds 400
		Send-Key 0x4D # M closes the map overlay the editor left open
		Start-Sleep -Seconds 1
		Send-Key 0xC0
		Start-Sleep -Milliseconds 400
		$script:glassBefore = Get-GlassFrames
		Start-Sleep -Seconds 1
		$warm = (Get-GlassFrames) - $script:glassBefore
		if ($warm -le 0) { throw "no frame drew glass with the $GlassKind in view - is it marked transparent?" }
		$script:glassBefore = Get-GlassFrames
		Write-Host "  glass in view ($warm frames drew it in the warm-up second)"
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	if ($Packs) {
		Write-Host 'putting an ammo pouch in the pack row and a herb pouch on the cursor'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		$rc = New-Object HarnessWin+RECT
		[HarnessWin]::GetClientRect($hwnd, [ref]$rc) | Out-Null
		$script:clientW = [double]$rc.Right; $script:clientH = [double]$rc.Bottom
		# Whoever has two free slots in the pack on show takes both bags (a
		# Continued eval save can leave any member's pack full).
		$status = Get-InventoryStatus
		$member = -1
		for ($m = 0; $m -lt $memberCount -and $member -lt 0; $m++) {
			if (@(Get-PackSlots $status $m | Where-Object { $_ -eq '-' }).Count -ge 2) { $member = $m }
		}
		if ($member -lt 0) { throw "no member has two free pack slots: $status" }
		Send-Text "give ammo_pouch $member"; Send-Key 0x0D
		Send-Text "give herb_pouch $member"; Send-Key 0x0D
		Start-Sleep -Milliseconds 300
		$status = Get-InventoryStatus
		$ammoAt = Get-SheetGridPoint (Get-PackSlot $status $member 'ammo_pouch')
		$herbAt = Get-SheetGridPoint (Get-PackSlot $status $member 'herb_pouch')
		Send-Text "sheet $member"; Send-Key 0x0D
		Start-Sleep -Milliseconds 500
		$p = Get-SheetPacks
		$shown = $p.Selected
		$free = [array]::IndexOf($p.Row, '-')
		if ($free -lt 0) { throw "member $member has no empty pack-row square: $($p.Line)" }
		$script:bagAt = Get-SheetPackRowPoint $free
		$shownAt = Get-SheetPackRowPoint $shown
		Write-Host "  member $member, bags into pack-row square $free (the pack on show is $shown)"
		Send-Key 0xC0
		Start-Sleep -Milliseconds 600
		Send-Click $ammoAt.X $ammoAt.Y                 # the ammo pouch onto the cursor
		Send-Click $script:bagAt.X $script:bagAt.Y     # into the empty square (0 -> 8)
		Send-Click $shownAt.X $shownAt.Y                 # back to the pack holding the herb pouch
		Send-Click $herbAt.X $herbAt.Y                 # the herb pouch onto the cursor
		# WARM-UP: one swap pair, first times for the process.
		Send-Click $script:bagAt.X $script:bagAt.Y
		Send-Click $script:bagAt.X $script:bagAt.Y
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		$p = Get-SheetPacks
		$status = Get-InventoryStatus
		if ($p.Equips -ne 3 -or $p.Row[$free] -ne 'ammo_pouch' -or $p.Slots -ne 8 -or
			$status -notmatch 'held=herb_pouch ') {
			throw "the pack setup went wrong ($($p.Line); $status) - grid $($ammoAt.X),$($ammoAt.Y), " +
				"row $($script:bagAt.X),$($script:bagAt.Y)"
		}
		$script:equipsBefore = $p.Equips
		Write-Host '  setup and a warm-up swap pair ok'
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	# THE SELF-TEST POKES ONCE, on the window's first armed frame - the first
	# armed frame after a disarm, which captured no stacks until code-review C214,
	# so a violation there was counted and its call site never logged. An
	# every-frame poke could not show that: its second frame captured and logged
	# the site. The console stays open from the poke to `alloctest`, so no armed
	# frame can fall between them (at 240 Hz the warm-up is half a second).
	$consoleOpen = $false
	if ($SelfTest) {
		Write-Host 'self-test: one allocation on the first armed frame, expecting FAIL and its call site'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'allocpoke once'
		Send-Key 0x0D
		Start-Sleep -Milliseconds 300
		$consoleOpen = $true
	}

	Write-Host "measuring ${Seconds}s of steady frames"
	if (-not $consoleOpen) {
		Send-Key 0xC0 # `~` opens the console
		Start-Sleep -Milliseconds 500
	}
	Send-Text "alloctest $Seconds"
	Send-Key 0x0D

	# -Pause: Esc into the pause menu and Esc back out, a few times, while the
	# window runs. Each wait clears the console close / resume plus the guard's
	# 120-frame warm-up, so the Esc lands in an ARMED frame; paused frames and
	# the warm-up after a resume are not armed and cost the window nothing.
	# -Walk: turn by key, a full circle right and one left, twice, while the
	# window runs. TURNS, not steps, so the party ends where it began and nothing
	# it might walk into (a wall bump, a stair) muddies what is measured: a turn
	# is an Act like any move, and it presses a pad stone the same way.
	if ($Walk) {
		for ($cycle = 1; $cycle -le 2; $cycle++) {
			Start-Sleep -Seconds 3
			if (Select-String -Path $log -Pattern 'alloctest RESULT=' -Quiet) { break }
			for ($t = 0; $t -lt 4; $t++) { Send-Key 0x45; Start-Sleep -Milliseconds 350 } # E
			for ($t = 0; $t -lt 4; $t++) { Send-Key 0x51; Start-Sleep -Milliseconds 350 } # Q
		}
	}

	# -Rest: lie down from the HUD's Rest button, once, inside the window. The
	# wait clears the console close plus the guard's 120-frame warm-up, so the
	# click lands in an ARMED frame, and so does lockstep's first inline think.
	if ($Rest) {
		Start-Sleep -Seconds 3
		Send-Click $script:restX $script:restY
	}

	if ($Pause) {
		for ($cycle = 1; $cycle -le 3; $cycle++) {
			Start-Sleep -Seconds 3
			if (Select-String -Path $log -Pattern 'alloctest RESULT=' -Quiet) { break }
			Send-Key 0x1B # pause
			Start-Sleep -Seconds 1
			Send-Key 0x1B # and resume
		}
	}

	# -Exit: Help, the exit stair answered No, the pit. Each wait of 4 s is meant
	# to clear a warm-up (the console's close, then the prompt's: 120 frames, 4 s
	# at 30 fps), so the click, the step onto the exit and the steps to the pit
	# land in ARMED frames. Only the game can say whether they did: the verdict
	# counts each in a measured frame, and the checks after it refuse on those
	# counts. The bound keys are the defaults (S back, W forward); N is the
	# prompt's No, as Esc is.
	if ($Exit) {
		Start-Sleep -Seconds 4
		if (-not (Select-String -Path $log -Pattern 'alloctest RESULT=' -Quiet)) {
			Send-Click $script:helpX $script:helpY # the movement-keys line
			Start-Sleep -Seconds 1
			Send-Key 0x53                          # S: back, onto the exit stair - "Leave?"
			Start-Sleep -Milliseconds 1500         # the world waits under the question
			Send-Key 0x4E                          # N: No
			Start-Sleep -Seconds 4
			Send-Key 0x57                          # W: forward, to the start
			Start-Sleep -Milliseconds 1200
			Send-Key 0x57                          # W: into the pit, and down
		}
	}

	# -Sheet: work the sheet while the window runs. The first wait clears the
	# console close plus the guard's 120-frame warm-up, so the clicks land in
	# ARMED frames. Esc closes whichever popup is up (the dialog, the menu) -
	# never the sheet, which only closes on an Esc with nothing open.
	if ($Sheet) {
		for ($cycle = 1; $cycle -le 3; $cycle++) {
			Start-Sleep -Seconds 3
			if (Select-String -Path $log -Pattern 'alloctest RESULT=' -Quiet) { break }
			Send-Mouse $script:runeX $script:slotY  # hover: the status bar names it
			Send-Mouse $script:bladeX $script:slotY
			for ($t = 0; $t -lt 5; $t++) { Send-Key 0x09 } # all five tabs, round to Inventory
			Send-Mouse $script:bladeX $script:slotY 0x204 0x205 2 # right: details
			Start-Sleep -Seconds 2                                 # the model turns
			Send-Key 0x1B
			Send-Mouse $script:runeX $script:slotY 0x207 0x208 0x10 # middle: use menu
			Start-Sleep -Milliseconds 500
			Send-Key 0x1B
		}
	}

	# -All: All, every tab, Esc, the portrait - a cycle every few seconds while
	# the window runs. The first wait clears the console close plus the guard's
	# warm-up, so the first click on All is armed.
	if ($All) {
		for ($cycle = 1; $cycle -le 3; $cycle++) {
			Start-Sleep -Seconds 3
			if (Select-String -Path $log -Pattern 'alloctest RESULT=' -Quiet) { break }
			Invoke-AllCycle
		}
	}

	# -Panels: drag the Movement dock away by its title and home again, then
	# pull the Hands dock's corner grip, while the window runs. The first wait
	# clears the console close plus the guard's warm-up, so the drags land in
	# ARMED frames; each release saves settings.ini.
	if ($Panels) {
		for ($cycle = 1; $cycle -le 3; $cycle++) {
			Start-Sleep -Seconds 3
			if (Select-String -Path $log -Pattern 'alloctest RESULT=' -Quiet) { break }
			# First, while the dock is still on its default spot: into the tray
			# and back out.
			if ($cycle -eq 1) { Invoke-TrayTrip }
			Send-Drag $script:moveX $script:moveY $script:awayX $script:awayY
			Send-Drag $script:awayX $script:awayY $script:moveX $script:moveY
			if ($cycle -eq 1) { Send-Drag $script:gripX $script:gripY $script:pullX $script:pullY }
		}
	}

	# -Packs: two clicks on the bag square a cycle - the herb pouch in (8 -> 4)
	# and the ammo pouch back (4 -> 8), the growth this mode exists for.
	if ($Packs) {
		for ($cycle = 1; $cycle -le 3; $cycle++) {
			Start-Sleep -Seconds 3
			if (Select-String -Path $log -Pattern 'alloctest RESULT=' -Quiet) { break }
			Send-Click $script:bagAt.X $script:bagAt.Y
			Send-Click $script:bagAt.X $script:bagAt.Y
		}
	}

	# -Items: round trips with the measured item while the window runs. The
	# window is still open from the warm-up; the first wait clears the console
	# close plus the guard's 120-frame warm-up, so the first pick is armed.
	if ($Items) {
		for ($cycle = 1; $cycle -le 3; $cycle++) {
			Start-Sleep -Seconds 3
			if (Select-String -Path $log -Pattern 'alloctest RESULT=' -Quiet) { break }
			Invoke-ItemRoundTrip $script:measurePoint
		}
	}

	# -Throw: lift the rock from the square ahead and throw it at the wall, a
	# round every ~2 s (throw_interval is 1 s, and the flight and landing take
	# well under one). The first wait clears the console close plus the guard's
	# warm-up, so the first lift is armed.
	if ($Throw) {
		Start-Sleep -Seconds 3
		for ($cycle = 1; $cycle -le 6; $cycle++) {
			if (Select-String -Path $log -Pattern 'alloctest RESULT=' -Quiet) { break }
			Send-Click $script:floorX $script:floorY
			Start-Sleep -Milliseconds 300
			Send-Click $script:skyX $script:skyY
			Start-Sleep -Milliseconds 1700
		}
	}

	# The command closes the console itself, then spends its budget on armed
	# frames only; its own deadline guarantees a line either way.
	$line = Wait-ForLog 'alloctest RESULT=' ($Seconds * 4 + 60) 'the alloctest result'
	$result = if ($line -match 'RESULT=(\w+)') { $Matches[1] } else { 'UNKNOWN' }
	Write-Host ''
	Write-Host $line.Substring($line.IndexOf('alloctest'))

	# WHAT HAPPENED INSIDE THE WINDOW, from the game itself: the verdict frame
	# logs the harness tally, which the window's first ARMED frame restarted.
	# (Asking `tally` afterwards used to count the console's frames, the
	# guard's warm-up and whatever landed while the question was being typed.)
	if ($Melee -or $Impact -or $Burst -or $Swing -or $Items -or $Throw -or $OnHitTypo -or $Exit) {
		$script:lastTally = (Wait-ForLog 'alloctest window TALLY ' 10 'the window tally') -replace '^.*TALLY ', 'TALLY '
		Write-Host "  in the window: $script:lastTally"
	}

	# -Burst: every shot-at-the-party path it exists to measure, counted by the
	# window's own tally. A bolt entry that failed, a lane nobody stood in or a
	# repel that met nothing would otherwise report exactly like a clean run.
	if ($Burst) {
		$missing = @()
		if ((Get-LastTallyField 'partybursts') -le 0) { $missing += 'no burst went off on the party' }
		if ((Get-LastTallyField 'wardturns') -le 0) { $missing += 'no ward turned a bolt' }
		if ((Get-LastTallyField 'repelweakened') -le 0) { $missing += 'no repel weakened a bolt' }
		if ((Get-LastTallyField 'repelturned') -le 0) { $missing += 'no repel flung a bolt back' }
		if ((Get-LastTallyField 'repelspent') -le 0) { $missing += 'no repel spent a bolt' }
		if ($missing.Count -gt 0 -and $result -eq 'PASS') {
			Write-Host "$($missing -join ', ') inside the window - the shots at the party were not measured" -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# -OnHitTypo counts only if a typo'd proc WARNED inside the window: the
	# swinging was held until its first armed frame, so every warning between the
	# hold and the verdict line was written in a window frame.
	if ($OnHitTypo) {
		$verdictAt = @(Select-String -Path $log -Pattern 'alloctest RESULT=')[-1].LineNumber
		$inside = @(Select-String -Path $log -Pattern "on-hit proc names effect 'brun'" |
			Where-Object { $_.LineNumber -lt $verdictAt }).Count - $script:typoBefore
		Write-Host "  typo'd on-hit warnings inside the window: $inside"
		if ($inside -lt 1 -and $result -eq 'PASS') {
			Write-Host 'no club blow warned inside the window - the warning was not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# -Swing: the party swung, a swing besides the loaded one, and the loaded one
	# fumbled severely and put the torch on the floor - all inside the window. A
	# swing that met nothing (the skeleton gone), a die spent before the window or
	# a fumble that dropped nothing would otherwise report exactly like a clean run.
	if ($Swing) {
		$missing = @()
		if ((Get-LastTallyField 'swings') -lt 2) { $missing += 'fewer than two swings reached the skeleton' }
		if ((Get-LastTallyField 'severefumbles') -le 0) { $missing += 'no swing fumbled severely' }
		if ((Get-LastTallyField 'fumbledrops') -le 0) { $missing += 'no fumble dropped a held item' }
		if ($missing.Count -gt 0 -and $result -eq 'PASS') {
			Write-Host "$($missing -join ', ') inside the window - the party's swing was not measured" -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# A melee PASS counts only if the swing path actually ran inside the window.
	# Without this, a monster that wandered off, or a party knocked out before
	# the window opened, would report exactly like a clean fight.
	if ($Melee) {
		$taken = Get-LastTallyField 'taken'
		if ($taken -le 0 -and $result -eq 'PASS') {
			Write-Host 'the monster landed no blow inside the window - the swing path was not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -Rest: the window measured a REST only if the party was still
	# resting at its end (the click in the window started it), and the AI's inline
	# search only if the monster was still engaged (noticed) - a monster that lost
	# interest would sit idle and search for nothing, and a rest that ended would
	# leave a party standing about. `lockstep stats` (counted since the click
	# turned lockstep on) must show the inline compute ran; and for -RestReach,
	# that its thinks found paths, or the search's output was never measured.
	if ($Rest) {
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		$restLine = Get-ConsoleAnswer 'rest' '(world x'
		$bars = Get-ConsoleAnswer 'hudbars' 'hudbars: '
		$monLine = (Get-ConsoleAnswer 'monsters' "$RestMonster @ ").Trim()
		$stats = Get-ConsoleAnswer 'lockstep stats' 'lockstep stats: '
		Send-Text 'rest off'; Send-Key 0x0D
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0
		Write-Host "  after the window: $restLine; $($bars -replace '\s*\|\s*demo.*$', ''); $monLine"
		Write-Host "  $stats"
		$ticks = 0; $paths = 0
		if ($stats -match 'ticks=(\d+) plans=(\d+) paths=(\d+)') {
			$ticks = [int]$Matches[1]; $paths = [int]$Matches[3]
		}
		$short = @()
		if ($restLine -notmatch '^rest on ') { $short += 'the party was not resting (the click missed, or the rest ended)' }
		if ($bars -notmatch 'noticed yes') { $short += 'the monster was no longer engaged' }
		if ($ticks -le 0) { $short += 'the AI never thought on the main thread' }
		if ($RestReach -and $paths -le 0) { $short += 'no inline think found a path' }
		if ($short.Count -gt 0 -and $result -eq 'PASS') {
			Write-Host "$($short -join ', ') - the resting AI was not measured" -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# The same refusal for -Impact, over all three things it exists to see: a
	# monster that died in the warm-up, or a rotation that stopped, would
	# otherwise report exactly like a clean barrage.
	if ($Impact) {
		$missing = @()
		if ((Get-LastTallyField 'bolthits') -le 0) { $missing += 'no bolt hit' }
		if ((Get-LastTallyField 'expired') -le 0) { $missing += 'no bolt expired' }
		if ((Get-LastTallyField 'blasts') -le 0) { $missing += 'no blast went off' }
		if ((Get-LastTallyField 'sceneryticks') -le 0) { $missing += 'no burning crate ticked' }
		if ((Get-LastTallyField 'doused') -le 0) { $missing += 'no fixture broke and went out' }
		# What the crate came to, for the reader: `broken` means its burn finished
		# it inside the window, so the break was measured as well as the ticks.
		# The console is shut again afterwards - the quit below reopens it.
		Send-Key 0xC0; Start-Sleep -Milliseconds 400
		Send-Text 'logecho on'; Send-Key 0x0D
		Send-Text "breakables $cx $tz"; Send-Key 0x0D
		Send-Text "breakables $bx $tz"; Send-Key 0x0D
		Wait-ConsoleDone
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0; Start-Sleep -Milliseconds 300
		foreach ($what in @("decoration crate @ $cx,$tz ", "fixture brazier @ $bx,$tz ")) {
			$row = @(Select-String -Path $log -Pattern "console:   $what")
			if ($row.Count -gt 0) {
				Write-Host "  after the window: $($row[-1].Line -replace '^.*console:\s+', '')"
			}
		}
		if ($missing.Count -gt 0 -and $result -eq 'PASS') {
			Write-Host "$($missing -join ', ') inside the window - the impact path was not measured" -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -Hand: every entry of the rotation cast again (several times - the
	# counts run on past the window until the hold, so one is not enough to say
	# it was inside), and the fresh items were USED: the torch lit, the skin
	# filled, the empty hand holding a pebble.
	if ($Hand) {
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Send-Text 'autocast hold'; Send-Key 0x0D
		$castPattern = 'console:   member \d+ casts \S+: \d+ cast, \d+ failed'
		$castBefore = @(Select-String -Path $log -Pattern $castPattern).Count
		$torchBefore = @(Select-String -Path $log -Pattern 'console:   \[\d\] \w+ hand \d: ').Count
		Send-Text 'autocast'; Send-Key 0x0D
		Send-Text 'torch'; Send-Key 0x0D
		Wait-ConsoleDone
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0
		$after = @(Select-String -Path $log -Pattern $castPattern) | Select-Object -Skip $castBefore
		$hands = (@(Select-String -Path $log -Pattern 'console:   \[\d\] \w+ hand \d: ') |
			Select-Object -Skip $torchBefore | ForEach-Object { $_.Line -replace '^.*console:\s+', '' }) -join '; '
		$short = @()
		for ($i = 0; $i -lt $after.Count -and $i -lt $script:handRows.Count; $i++) {
			$was = if ($script:handRows[$i].Line -match ': (\d+) cast,') { [int]$Matches[1] } else { 0 }
			$now = if ($after[$i].Line -match ': (\d+) cast,') { [int]$Matches[1] } else { 0 }
			Write-Host "  $($after[$i].Line -replace '^.*console:\s+', '') ($($now - $was) since the warm-up)"
			if ($now - $was -lt 3) { $short += ($after[$i].Line -replace '^.*casts (\S+):.*$', '$1') }
		}
		Write-Host "  hands: $hands"
		$used = @()
		if ($hands -notmatch '\[2\] Maren hand 1: torch_lit ') { $used += 'the torch was not lit' }
		if ($hands -notmatch '\[3\] Tilo hand 1: waterskin ') { $used += 'the skin was not filled' }
		if ($hands -notmatch '\[0\] Brand hand 0: pebble ') { $used += 'no pebble landed in hand' }
		if (($short.Count -gt 0 -or $used.Count -gt 0 -or $after.Count -ne 4) -and $result -eq 'PASS') {
			if ($short.Count -gt 0) { Write-Host "too few casts after the warm-up: $($short -join ', ')" -ForegroundColor Yellow }
			if ($used.Count -gt 0) { Write-Host "$($used -join ', ') - the item paths were not measured" -ForegroundColor Yellow }
			$result = 'UNMEASURED'
		}
	}

	# And for -Light: every light in the rotation cast again inside the window,
	# and the flare's dazzle is on the mummy.
	if ($Light) {
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Send-Text 'autocast hold'; Send-Key 0x0D
		$castPattern = 'console:   member \d+ casts \S+: \d+ cast, \d+ failed'
		$castBefore = @(Select-String -Path $log -Pattern $castPattern).Count
		$monBefore = @(Select-String -Path $log -Pattern 'console:   mummy @').Count
		$tracksBefore = @(Select-String -Path $log -Pattern 'console: tracks: \d+ on').Count
		$trailsBefore = @(Select-String -Path $log -Pattern 'console: trails: ').Count
		Send-Text 'autocast'; Send-Key 0x0D
		Send-Text 'tracks'; Send-Key 0x0D
		# Nothing flies in this mode, so the spark pool's TRAIL motes are the
		# tracks the stone is showing.
		Send-Text 'trails'; Send-Key 0x0D
		Send-Text 'monsters'; Send-Key 0x0D
		Wait-ConsoleDone
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0
		$after = @(Select-String -Path $log -Pattern $castPattern) | Select-Object -Skip $castBefore
		$mummy = (@(Select-String -Path $log -Pattern 'console:   mummy @') | Select-Object -Skip $monBefore |
			ForEach-Object { $_.Line -replace '^.*console:\s+', '' }) -join '; '
		$short = @()
		for ($i = 0; $i -lt $after.Count -and $i -lt $script:lightRows.Count; $i++) {
			$was = if ($script:lightRows[$i].Line -match ': (\d+) cast,') { [int]$Matches[1] } else { 0 }
			$now = if ($after[$i].Line -match ': (\d+) cast,') { [int]$Matches[1] } else { 0 }
			Write-Host "  $($after[$i].Line -replace '^.*console:\s+', '') ($($now - $was) since the warm-up)"
			if ($now - $was -lt 3) { $short += ($after[$i].Line -replace '^.*casts (\S+):.*$', '$1') }
		}
		Write-Host "  the mummy: $mummy"
		$trackLine = @(Select-String -Path $log -Pattern 'console: tracks: \d+ on') | Select-Object -Skip $tracksBefore -First 1
		$trackCount = if ($trackLine -and $trackLine.Line -match 'tracks: (\d+) on') { [int]$Matches[1] } else { 0 }
		$trailLine = @(Select-String -Path $log -Pattern 'console: trails: ') | Select-Object -Skip $trailsBefore -First 1
		$trackMotes = if ($trailLine -and $trailLine.Line -match '(\d+) of them trail') { [int]$Matches[1] } else { 0 }
		Write-Host "  monster tracks in the stone's reach: $trackCount, $trackMotes motes showing them"
		if (($short.Count -gt 0 -or $after.Count -ne 5 -or $mummy -notmatch '\[dazzle ' -or $trackCount -lt 3 -or $trackMotes -lt 1) -and $result -eq 'PASS') {
			if ($trackCount -lt 3 -or $trackMotes -lt 1) { Write-Host 'the stone showed no tracks - ShowTracks was not measured' -ForegroundColor Yellow }
			if ($short.Count -gt 0) { Write-Host "too few casts after the warm-up: $($short -join ', ')" -ForegroundColor Yellow }
			if ($mummy -notmatch '\[dazzle ') { Write-Host 'the flare dazzled nothing - its monster path was not measured' -ForegroundColor Yellow }
			$result = 'UNMEASURED'
		}
	}

	# And for -Walk: no move counted means no key landed in an armed frame.
	if ($Walk) {
		$moves = if ($line -match '\bmoves=(\d+)') { [int]$Matches[1] } else { 0 }
		Write-Host "  key moves inside the window: $moves"
		if ($moves -lt 4 -and $result -eq 'PASS') {
			Write-Host 'fewer than four key moves landed - the pad presses were not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -Lights: no load placed means the budget ran on the level's own
	# handful of lights, which is not what the mode exists to measure.
	if ($Lights -and $script:stressLights -lt 32 -and $result -eq 'PASS') {
		Write-Host "only $($script:stressLights) test lights were placed - the light load was not measured" -ForegroundColor Yellow
		$result = 'UNMEASURED'
	}

	# And for -Pause: no transition counted means no Esc landed in an armed
	# frame, and the run measured nothing it exists to measure.
	if ($Pause) {
		$transitions = if ($line -match '\btransitions=(\d+)') { [int]$Matches[1] } else { 0 }
		Write-Host "  transitions inside the window: $transitions"
		if ($transitions -le 0 -and $result -eq 'PASS') {
			Write-Host 'no Esc landed in an armed frame - the pause transition was not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -Exit, every count from the VERDICT LINE, so each happened in a frame
	# the guard measured: the prompt opened (prompts=), the Help click pressed the
	# button (helps=) and the step into the pit began the fall (falls=). Then the
	# party came down on crypt2 at 7,6 - a Yes would have left the dungeon, a
	# swallowed N left the world frozen under the question. The presses since the
	# set-up (`messages help`) are only printed: beside helps= they tell a click
	# that missed the button from one that landed in an unarmed frame.
	if ($Exit) {
		$prompts = if ($line -match '\bprompts=(\d+)') { [int]$Matches[1] } else { 0 }
		$helps = if ($line -match '\bhelps=(\d+)') { [int]$Matches[1] } else { 0 }
		$falls = if ($line -match '\bfalls=(\d+)') { [int]$Matches[1] } else { 0 }
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		$help = Get-ConsoleAnswer 'messages help' 'messages help: '
		$where = Get-ConsoleAnswer 'pos' ' facing '
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0
		$presses = if ($help -match 'presses=(\d+)') { [int]$Matches[1] - $script:helpBefore } else { 0 }
		$landed = (Get-LogMatchCount '^\[info \] Level ready: crypt2 at 7,6$') -gt $script:crypt2Before
		Write-Host "  in measured frames - prompts / Help presses / pit steps: $prompts / $helps / $falls"
		Write-Host "  Help presses since the set-up, armed or not: $presses"
		Write-Host "  after the window: $(if ($landed) { 'on crypt2' } else { 'never reached crypt2' }), $where"
		$short = @()
		if ($prompts -lt 1) { $short += 'the exit stair asked nothing' }
		if ($helps -lt 1) { $short += 'no Help press in a measured frame' }
		if ($falls -lt 1) { $short += 'no pit step in a measured frame' }
		if (-not $landed -or $where -notmatch '^7,6 facing ') { $short += 'the party did not come down on crypt2 at 7,6' }
		if ($short.Count -gt 0 -and $result -eq 'PASS') {
			Write-Host "$($short -join ', ') inside the window - the exit, the fall and the Help line were not measured" -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -Packs: two equips made during the window (one each way), or the
	# clicks missed and the growth was not measured.
	if ($Packs) {
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		$equips = (Get-SheetPacks).Equips - $script:equipsBefore
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0
		Write-Host "  bags equipped by a click during the window: $equips"
		if ($equips -lt 2 -and $result -eq 'PASS') {
			Write-Host 'fewer than two equips - the pack growth was not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -Items: two drops and two lifts inside the window, which puts the
	# first round trip's put-back between them inside it too. Fewer means a
	# click missed or the window closed early, and the moves were not measured.
	if ($Items) {
		# The party window was parked for the run; put the layout back.
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'hudpanel reset'; Send-Key 0x0D
		Send-Key 0xC0
		$drops = Get-LastTallyField 'drops'
		$lifts = Get-LastTallyField 'lifts'
		Write-Host "  floor drops / lifts inside the window: $drops / $lifts"
		if (($drops -lt 2 -or $lifts -lt 2) -and $result -eq 'PASS') {
			Write-Host 'fewer than two round trips inside the window - the item moves were not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -Throw: two lifts, two throws and two flights that came down,
	# inside the window - or a click missed and the throw was not measured.
	if ($Throw) {
		$throws = Get-LastTallyField 'throws'
		$lifts = Get-LastTallyField 'lifts'
		$landed = (Get-LastTallyField 'throwlandings') + (Get-LastTallyField 'throwstrikes')
		Write-Host "  lifts / throws / came down inside the window: $lifts / $throws / $landed"
		if (($lifts -lt 2 -or $throws -lt 2 -or $landed -lt 2) -and $result -eq 'PASS') {
			Write-Host 'fewer than two whole throws inside the window - throwing was not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -Glass: the glass must have been drawn through the window - a frame
	# count that did not move means it left the view and nothing was measured.
	if ($Glass) {
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		$glassFrames = (Get-GlassFrames) - $script:glassBefore
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0
		$windowFrames = if ($line -match '\bframes=(\d+)') { [int]$Matches[1] } else { 0 }
		Write-Host "  frames that drew glass: $glassFrames (window: $windowFrames armed frames)"
		if ($glassFrames -lt $windowFrames -and $result -eq 'PASS') {
			Write-Host 'glass was not in view for the whole window - the transparent queue was not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -Sheet: no new open of the dialog means the right-click missed (or
	# landed outside the window), and the open path was not measured.
	if ($Sheet) {
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		$opens = (Get-DetailOpens) - $script:opensBefore
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0
		Write-Host "  item details opened by a right-click: $opens"
		if ($opens -le 0 -and $result -eq 'PASS') {
			Write-Host 'no right-click opened the dialog - the open path was not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -All: the window must have opened inside the window, twice, or a
	# click missed and the open path was not measured.
	if ($All) {
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		$status = Get-InventoryStatus
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0
		$opens = if ($status -match 'opens=(\d+)') { [int]$Matches[1] - $script:allOpensBefore } else { 0 }
		Write-Host "  party window opens inside the window: $opens"
		if ($opens -lt 2 -and $result -eq 'PASS') {
			Write-Host 'fewer than two opens of the party window - it was not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -Panels: the drags must have LANDED - the move dock saved off its
	# default spot and the hands dock off scale 1 - or nothing was measured.
	if ($Panels) {
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Send-Text 'hudpanel list'; Send-Key 0x0D
		Wait-ConsoleDone
		# Closed for the checks below: the reset button puts it home too, and at
		# its default size the party window covers the Movement dock's drags.
		Send-Text 'inventory off'; Send-Key 0x0D
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0
		$moveRow = @(Select-String -Path $log -Pattern 'console:   move ')[-1].Line
		$handsRow = @(Select-String -Path $log -Pattern 'console:   hands ')[-1].Line
		Write-Host "  $($moveRow -replace '^.*console:   ', '')"
		Write-Host "  $($handsRow -replace '^.*console:   ', '')"
		$invRow = @(Select-String -Path $log -Pattern 'console:   inventory ')[-1].Line
		Write-Host "  $($invRow -replace '^.*console:   ', '')"
		$moved = $moveRow -notmatch 'saved default'
		$scaled = $handsRow -notmatch 'scale 1\.00'
		$invShown = $invRow -match 'inventory shown'
		# The tray: the counts are since launch and the warm-up made one trip, so
		# the window's trip shows as a second of each - and the dock is back.
		$hudRow = @(Select-String -Path $log -Pattern 'console: hud layout ')[-1].Line
		$trips = if ($hudRow -match 'minimizes (\d+), restores (\d+)') { [Math]::Min([int]$Matches[1], [int]$Matches[2]) } else { 0 }
		Write-Host "  tray trips (warm-up included): $trips; move dock $(if ($moveRow -match 'minimized') { 'still minimized' } else { 'restored' })"
		$tripped = $trips -ge 2 -and $moveRow -notmatch 'minimized'
		if ((-not $moved -or -not $scaled -or -not $invShown -or -not $tripped) -and $result -eq 'PASS') {
			Write-Host 'a drag or the tray trip did not land, or the inventory was not open, inside the window - the panel path was not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}

		# The arranging rules, after the window: a Ctrl+click on the moved dock's
		# RESET button (beside its minimize in the top-right corner, read off the
		# dock's own rect) puts every panel home, and a drag WITHOUT Ctrl then
		# moves nothing.
		if ($moveRow -match 'px (-?\d+),(-?\d+) (\d+)x(\d+)') {
			$rx = [int]$Matches[1] + [int]$Matches[3] - $script:grip - 7; $ry = [int]$Matches[2] + 6
			Send-Mouse $rx $ry
			Send-CtrlClick $rx $ry
			$resetMove = Get-PanelRow 'move'
			$resetHands = Get-PanelRow 'hands'
			Send-Drag $script:moveX $script:moveY $script:awayX $script:awayY -NoCtrl
			$plainMove = Get-PanelRow 'move'
			Write-Host "  after reset: $($resetMove -replace '^.*console:   ', '')"
			Write-Host "  after a plain drag: $($plainMove -replace '^.*console:   ', '')"
			$wasReset = $resetMove -match 'saved default' -and $resetHands -match 'saved default.*scale 1\.00'
			$stayed = $plainMove -match 'saved default'
			# SNAPPING: drag the move dock left until its right edge is 4 px short
			# of the hands dock's left edge (they share a column, so the hands
			# dock sits right below it); it must land butted up against it.
			$snapped = $false; $snapNote = 'rects unreadable'
			if ($plainMove -match 'px (-?\d+),(-?\d+) (\d+)x(\d+)') {
				$mLeft = [int]$Matches[1]; $mW = [int]$Matches[3]
				if ($resetHands -match 'px (-?\d+),') {
					$hLeft = [int]$Matches[1]
					$dx = ($hLeft - 4) - ($mLeft + $mW)
					Send-Drag $script:moveX $script:moveY ($script:moveX + $dx) $script:moveY
					$snapMove = Get-PanelRow 'move'
					Write-Host "  after a snapping drag: $($snapMove -replace '^.*console:   ', '')"
					$want = $hLeft - $mW
					$snapNote = "wanted the left edge at $want"
					if ($snapMove -match 'px (-?\d+),') { $snapped = [Math]::Abs([int]$Matches[1] - $want) -le 1 }
					Send-Key 0xC0
					Start-Sleep -Milliseconds 500
					Send-Text 'hudpanel reset'; Send-Key 0x0D
					Send-Key 0xC0
				}
			}
			if (-not $wasReset -or -not $stayed -or -not $snapped) {
				Write-Host $(if (-not $wasReset) { 'the reset button did not put the panels home' }
							 elseif (-not $stayed) { 'a drag without Ctrl moved a panel' }
							 else { "a drag 4 px from an edge did not snap to it ($snapNote)" }) -ForegroundColor Red
				if ($result -eq 'PASS') { $result = 'FAIL' }
			}
		} else {
			Write-Host 'could not read the move dock''s rect - the reset button was not checked' -ForegroundColor Yellow
			if ($result -eq 'PASS') { $result = 'UNMEASURED' }
		}
	}

	# And for -Minimal: the cards must have been up - else the run measured the
	# Standard HUD and says nothing about the Minimal one. Then Standard goes
	# back, so the next harness on this build starts where it expects.
	if ($Minimal) {
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Send-Text 'hudpanel list'; Send-Key 0x0D
		Wait-ConsoleDone
		Send-Text 'hudpanel layout standard'; Send-Key 0x0D
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0
		$cardsRow = @(Select-String -Path $log -Pattern 'console:   cards ')[-1].Line
		Write-Host "  $($cardsRow -replace '^.*console:   ', '')"
		if ($cardsRow -notmatch 'cards +shown' -and $result -eq 'PASS') {
			Write-Host 'the party cards were not up - the Minimal layout was not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -Lights: the CANDIDATE CEILING (Test-LightCeiling) on the window's
	# level, and the floor glows run before the game (Test-FloorGlows). LIGHTS
	# when either failed, UNMEASURED when the ceiling met no fire. Not under the
	# guard's own -SelfTest, which is about the guard; under -ShadowSelfTest they
	# run as normal (the mutation is the cache's alone).
	$ceilingDied = $false
	if ($Lights -and (-not $SelfTest -or $ShadowSelfTest)) {
		Write-Host 'checking the candidate ceiling: `lightstress fill` ahead of the fires'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		$ceiling = Test-LightCeiling
		$ceilingDied = $proc.HasExited
		if (-not $ceilingDied) {
			Send-Text 'logecho off'; Send-Key 0x0D
			Send-Key 0xC0
		}
		foreach ($n in $ceiling.Notes) { Write-Host "  $n" }
		Write-Host "  candidate ceiling: $($ceiling.Verdict); floor glows: $($glow.Verdict)"
		if ($ceiling.Verdict -eq 'FAIL' -or $glow.Verdict -ne 'PASS') {
			if ($result -eq 'PASS' -or $result -eq 'UNMEASURED') { $result = 'LIGHTS' }
		} elseif ($ceiling.Verdict -ne 'PASS') {
			Write-Host 'the ceiling check could not be set up - the fire loop was not measured' -ForegroundColor Yellow
			if ($result -eq 'PASS') { $result = 'UNMEASURED' }
		}
	}

	# And for -Lights: the SHADOW CACHE, after the window (Test-ShadowCache). Not
	# under the guard's own -SelfTest, which is about the guard. Its verdict is
	# its own: STALE when a cube missed a change it shows, UNMEASURED when the
	# scene could not be set up to tell; -ShadowSelfTest mutates the cache and
	# wants BOTH checks to fail. Not on a game the ceiling check killed.
	if ($Lights -and (-not $SelfTest -or $ShadowSelfTest) -and -not $ceilingDied) {
		Write-Host 'checking the shadow cache: a door beside a still party, a walking Firelight'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		$shadow = Test-ShadowCache
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0
		foreach ($n in $shadow.Notes) { Write-Host "  $n" }
		Write-Host "  shadow cache: door $($shadow.Door), Firelight $($shadow.Fire)"
		if ($ShadowSelfTest) {
			if ($shadow.Door -eq 'FAIL' -and $shadow.Fire -eq 'FAIL') {
				Write-Host 'SHADOW SELF-TEST PASSED - both checks caught the mutated cache' -ForegroundColor Green
			} else {
				Write-Host 'SHADOW SELF-TEST FAILED - a check passed (or could not run) with the cache mutated' -ForegroundColor Red
				$shadowSelfTestFailed = $true
				if ($result -eq 'PASS') { $result = 'STALE' }
			}
		} elseif ($shadow.Door -eq 'FAIL' -or $shadow.Fire -eq 'FAIL') {
			if ($result -eq 'PASS') { $result = 'STALE' }
		} elseif ($shadow.Door -ne 'PASS' -or $shadow.Fire -ne 'PASS') {
			Write-Host 'a shadow check could not be set up - the cache was not measured' -ForegroundColor Yellow
			if ($result -eq 'PASS') { $result = 'UNMEASURED' }
		}
	}

	# And for -Items: the item pose and the picks, run before the game
	# (Test-ItemPose). PICKS when they failed, whatever the window said.
	if ($pose -and $pose.Verdict -ne 'PASS') {
		if ($result -eq 'PASS' -or $result -eq 'UNMEASURED') { $result = 'PICKS' }
	}

	# And for -PartyPage: the page must still be up. A window that never armed
	# reports SKIP by itself; this catches one that armed and lost the page, after
	# which the switch would have guarded nothing.
	if ($PartyPage) {
		Send-Key $VK_CONSOLE
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key $VK_RETURN
		$pagePattern = 'console: (member 0|party page: closed)'
		$before = Get-LogMatchCount $pagePattern
		Send-Text 'partypage'; Send-Key $VK_RETURN
		$rows = Wait-NewLogLines $pagePattern $before
		Send-Text 'logecho off'; Send-Key $VK_RETURN
		Send-Key $VK_CONSOLE
		$row = if ($rows.Count) { $rows[-1].Line -replace '^.*console: ', '' } else { 'no answer from `partypage`' }
		Write-Host "  after the window: $row"
		if ($row -notmatch '^member 0' -and $result -eq 'PASS') {
			Write-Host 'the party page was not open - the idle page was not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# EVERY MODE: an AI pool that GREW in play fails the run, wherever it grew -
	# inside the window, in the warm-up or in a console frame. The pools are
	# filled at level load to as many buffers as can be in use at once (C66), so
	# growth means a fill that fell short or a reader that never gave its mark
	# back; and it used to land in whichever guarded frame the threads chose,
	# which a window caught only by luck. The game logs it once per pool. Read
	# LAST, so the frames -Lights' shadow checks run are covered too.
	$grew = @(Select-String -Path $log -Pattern 'AI pool grew:' -SimpleMatch -EA SilentlyContinue)
	if ($grew.Count -gt 0) {
		foreach ($g in $grew) { Write-Host "  $($g.Line)" -ForegroundColor Red }
		if ($result -eq 'PASS') {
			Write-Host 'an AI pool grew in play (see above)' -ForegroundColor Red
			$result = 'FAIL'
		}
	}

	# A self-test INVERTS the verdict: the guard is working only if the run it
	# was asked to break comes back FAIL - and only if dungeon.log then NAMES the
	# poke's call site, since "call sites are in dungeon.log" is what a FAIL tells
	# whoever reads it. A count with no site is the C214 defect.
	$want = if ($SelfTest) { 'FAIL' } else { 'PASS' }
	if ($SelfTest -and $result -eq 'FAIL') {
		$site = @(Select-String -Path $log -Pattern '^\[warn \]\s+.*Game::AllocPokeOnce\b')
		if ($site.Count -eq 0) {
			Write-Host 'SELF-TEST FAILED - the guard counted the poke but logged no call site for it' -ForegroundColor Red
			Select-String -Path $log -Pattern 'steady-state frame allocated' | ForEach-Object { Write-Host "  $($_.Line)" }
			$result = 'NO-STACK'
		} else {
			Write-Host "  the poke's call site: $($site[0].Line.Trim())"
		}
	}
	switch ($result) {
		'PASS' {
			if ($SelfTest) {
				Write-Host 'SELF-TEST FAILED - allocpoke allocated and the guard missed it' -ForegroundColor Red
			} else {
				Write-Host 'PASS - no steady-state frame allocated' -ForegroundColor Green
			}
		}
		'NO-STACK' { } # already explained above
		'FAIL' {
			if ($SelfTest) {
				Write-Host 'SELF-TEST PASSED - the guard caught the deliberate allocation and named its site' -ForegroundColor Green
			} else {
				Write-Host 'FAIL - call sites follow (also in dungeon.log)' -ForegroundColor Red
				Select-String -Path $log -Pattern '^\[warn' | ForEach-Object { Write-Host "  $($_.Line)" }
			}
		}
		'UNMEASURED' { } # already explained above
		'STALE' {
			Write-Host 'FAIL - a shadow cube kept a change it should have shown (see above)' -ForegroundColor Red
		}
		'LIGHTS' {
			Write-Host 'FAIL - a light check failed: the candidate ceiling or a floor glow (see above)' -ForegroundColor Red
		}
		'PICKS' {
			Write-Host 'FAIL - an item pose or click pick check failed: a niche glow shut or out of its pocket, or a target missed where it is drawn (see above)' -ForegroundColor Red
		}
		default {
			Write-Host "$result - the game never reached a steady frame" -ForegroundColor Yellow
		}
	}
	$code = if ($result -eq $want) { 0 } else { 1 }
	if ($shadowSelfTestFailed) { $code = 1 } # whatever the guard's own self-test said
} finally {
	# Quit through the console so shutdown runs (it logs whole-run heap totals).
	Stop-HarnessGame 5000 -OpenConsole
}
exit $code
