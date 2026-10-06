# code-review - plan

The plan for acting on the review: docs/code-review-findings.md (the report)
and docs/code-review-issues.md (every issue as C0..C482). It follows the
report's order of work, reshaped by Michael's eleven answers (recorded in
docs/code-review-notes.md). Every one of the 483 issues has a place in it:
the index at the end says which phase or batch closes each one.

## How the work runs

- **One phase at a time, each judged before the next.** A phase lands only
  when the checks named for it pass. Each commit fixes the CLAUDE.md or docs
  paragraph its change makes wrong, in the same commit (the review found that
  old paragraphs left standing are how the handoff drifts).
- **Phase 0 goes first, all of it** (answer 1). Its 96 batches are each one
  commit (a few are two), in the order given. The harness and judge batches
  (0a) come first so every later batch is judged by checks that can fail.
- **A batch names its judge.** "ADD" means a check that must be written in
  that batch before its fix is trusted. A batch marked "Look: yes" changes
  something visible: close any test instance and hand Michael a freshly
  launched game for it, rather than judging it from a script.
- **Harnesses run the DEBUG build**; rebuild debug before trusting a PASS.
- **Merge to main at phase boundaries**, and inside phase 0 after each
  sub-group (0a+judge infrastructure, 0b, 0c, 0d, 0e, 0f/0g, 0h) - other
  sessions keep landing on main, and a long-lived branch is how the `sound`
  branch got stranded. Survey unmerged branches before each merge.
- **Nothing here changes balance numbers.** Where a fix moves an eval number
  (a passage rule, stance on monster shots), the plan says so, and the number
  is reported, not tuned.

## The phases

| Phase | What | Issues | Size | Risk |
|---|---|---|---|---|
| 0 | Standalone fixes: judges first, then docs, combat/AI, platform/UI, rendering, lifetimes, allocation, data/editor | 233 | L (96 batches, mostly S) | low |
| 1 | Split Game into six CMake targets (answer 5) | 13 | M (12 commits) | low |
| 2 | Shared helpers; allocation-guard coverage, workers judged, M map guarded | 23 | M | low |
| 3 | One reset: LevelRuntime + WorldSession | 3 (+ the reset bugs phase 0 fixes in place) | M | medium |
| 4 | One passage rule (Game/Passage.h) | 5 | M | medium - eval numbers move |
| 5 | The one hit pipeline (5a-5g) | 32 | L | medium-high |
| 6 | AI behaviours as classes | 9 | L | medium |
| 7 | Dialogs on one base, a modal stack | 18 | L | low-medium |
| 8 | Graphics seams (8a-8e) | 25 | L | medium |
| 9 | Data integrity: schema as source, TryParse, records for monsters and decorations | 24 | L | medium |
| 10 | Extractions from DungeonWorld and Game | 9 | L, ongoing | medium |
| 11 | Dead code, stale docs, a doc-name check | 86 | M | low |
| - | Dropped, with reasons | 3 | - | - |

Dependencies: 0 before everything. 1 is independent of 2-11 and makes their
new pure headers land behind a build-enforced wall. 3 before 5 (the reset
struct holds the blasts 5e moves). 4 before 5c and 6 (both read the passage
grid). 5 before 6 (monsters attack and cast through 5's builders). 7 and 8
are independent of 3-6 and can run in their own worktree. 9 before 10's
LevelStore. 11 last.

## Phase 0 - standalone fixes, judges first

Goal: stop every live bug and rule-break that needs no refactor (answer 1), with the judges made honest first. Each batch is a commit. Paths are under src/Game/ unless shown.

There are 96 batches (89 numbered in order, then 5a and 90-96 added for the tools issues assigned late). Of the 213 issues first batched, 212 are placed below; the additions place 20 more. C46 is moved out, and C34 and C227 are in batches but held for Michael's answer. The batches run in this order:

1. 0a (the judges and harnesses).
2. Two pieces of judge infrastructure: guard evidence, and a hidden headless window.
3. 0b (docs).
4. 0c to 0h, grouped by the files they touch.

Paths are under `src/Game/` unless shown otherwise. "Judge" names the existing checks, and "ADD" names a check that must be written in that batch before its fix is trusted. "Look" says whether Michael should see the change in game.

**0a - judges and harnesses (first)**

**1. Shared PowerShell harness module** - C401 (PowerShell half), C428, C429
- Files: tools/HarnessGame.ps1 (new), AllocTest.ps1, InGameTest.ps1, HealthTest.ps1, ProfileTest.ps1, TypingTest.ps1.
- C401: one dot-sourced module that the five harnesses share instead of their own copies. It covers:
  - starting its own game (lookup by PID plus window class, 30 s retry, as in docs/drive.ps1);
  - `newgame` through the console, so TypingTest stops pressing Enter on the title;
  - log waits;
  - stopping the game by PID.
- C428: the console-ready wait counts only new log lines.
- C429: HealthTest waits on `Level ready:|New game started`, then a console echo.
- Judge: /check-selftest (every PowerShell harness still passes and its -SelfTest still fails); HealthTest with shadercache\ deleted. ADD: TypingTest leaves Documents\DungeonSaves untouched.
- Look: no.

**2. Shared Python harness module; a run counts only if it finished** - C401 (Python half), C430, C433
- Files: tools/harness_game.py (new), harness_audio.py, HarnessAudio.ps1, EditorTest.py, WorldTest.py, SpellTest.py, Eval.ps1, CheckAll.ps1, ProfileTest.ps1, src/Main/Main.cpp.
- C401: run() returns the exit code and handles TimeoutExpired.
- C430: refuse when this worktree's exe is already running; require exit 0 or the BATCH verdict; skip AllocConsole under -headless.
- C433: the mute flag is set per bin directory, and SpellTest imports harness_audio.
- Judge: Eval.ps1 -SelfTest, SpellTest --selftest, EditorTest, WorldTest. ADD:
  - a second Eval.ps1 in the same worktree is refused;
  - a run killed before its BATCH line fails;
  - each harness logs its mute state;
  - CheckAll -Full runs ProfileTest and SpellTest muted.
- Look: no.

**3. Stale exe** - C426
- Files: CheckAll.ps1, HarnessGame.ps1, harness_game.py, SpellTest.py, EditorTest.py.
- C426:
  - `-Only` runs its own build row first (build-profile for `profile`).
  - Standalone harnesses ask the build system (`ninja -n`) through the shared helper, and refuse with an exit code distinct from FAIL.
- Judge: ADD: touch a .cpp, then run AllocTest standalone and `CheckAll -Only alloc`. Each must build first or exit with the stale code.
- Look: no.

**4. Judges on scratch worlds; checks that cannot fail** - C431, C432
- Files: EditorTest.py, WorldTest.py, LevelBuildTest.py.
- C431: EditorTest and WorldTest run on `-project` scratch copies (LevelBuildTest's scratch()). They never delete an existing backup, and they name their saves per worktree.
- C432:
  - make WorldTest's `or True` control real;
  - refuse an unknown LevelBuildTest phase, and refuse a PASS with zero checks;
  - create phase 16's settings.ini control when the file is absent.
- Judge: all three pass. ADD:
  - kill EditorTest mid-phase, rerun, and require `git status assets/projects assets/library` to be clean;
  - `LevelBuildTest 9` is refused.
- Look: no.

**5. RollTest's self-test names its fault** - C419 (RollTest part), C424
- Files: tools/RollTest/Main.cpp, Combat.cpp.
- C419: ResolveAttack takes the caller's RollRules, so the dice fault reaches the strike sections. --self-test lists exactly the checks it expects to fail (the SpellTest.py:356 rule).
- C424: size-guard the three `[0]` indexings, and delete the `sealed ? fire : fire` no-op.
- Judge: RollTest passes, and --self-test fails exactly its named checks. ADD mutation: an empty parser result prints FAIL instead of a CRT dialog.
- Look: no.

**6. ThreadStress walks real paths** - C418, C419 (ThreadStress part), C454 (its ThreadStress half)
- Files: tools/ThreadStress/Main.cpp.
- C418:
  - target the party's cell;
  - put monsters in `occ`, not `blocked` (this is also C454's ThreadStress item);
  - take the bucket thresholds from Scheduler::BucketForIq;
  - assert that reachable phases return paths and unreachable ones return none.
- C419: the self-test names its expected failures.
- Judge: /check-threads passes, and the self-test fails exactly the named checks.
- Look: no.

**7. Bc7Test's baseline gate** - C417, C422, C419 (Bc7Test part)
- Files: tools/Bc7Test/Main.cpp, Bc7Decode.h, Bc7Test.ps1, bc7-baseline.txt, tools/AssetBaker/Bc7Tables.h, Bc7Encoder.h.
- C417: a getline loader that skips '#' and blank lines. It prints each unmatched image, and fails on zero matches or when no syn.* image matches.
- C422: build the "default" rows from Bc7Options{}, with only `threads` changed. Fix the stale GPU-proof comments, and add mode 3 to the list in Bc7Encoder.h.
- Judge: /check-bc7 --audit. ADD: a self-test that raises the loaded baseline by about 1 dB and requires `regressed > 0`.
- Look: no.

**8. Game-driving harness self-tests and coverage** - C419 (HealthTest, InGameTest, ProfileTest parts), C427
- Files: HealthTest.ps1, InGameTest.ps1, ProfileTest.ps1, Game_DevDiagnostics.cpp, and the generator-tab and new-world-dialog dev commands.
- C419:
  - each -SelfTest names the cases it expects to fail;
  - a harness error fails the self-test;
  - InGameTest's self-test keeps the real labels;
  - ProfileTest's header says what it skips.
- C427:
  - match `uioverlap \[<label>\] ---` exactly;
  - every open step logs a status line that the verdict requires;
  - the run ends by requiring `state` to be playing or worldmap.
- Judge: /check-selftest. ADD: an InGameTest self-test case where one open step is refused must fail.
- Look: no.

**9. DiagTest log checks** - C420, C389 (0f)
- Files: tools/DiagTest/Main.cpp, src/Core/Diagnostics.cpp, Log.cpp, Log.h.
- C420:
  - expose log::FilePath(), and treat an unreadable log as FAIL;
  - flush pending suppressions when a thread unregisters;
  - split the repeat counter from the rate-limit counter;
  - replace the vacuous `>= 0` check.
- C389: ResetEntry also clears windowStartNs, windowLogged and suppressed.
- Judge: /check-diag. ADD:
  - a burst, a wait past the window, then one more event gives exactly one summary line with the right count;
  - with 33 distinct thread names, a slot's new owner starts with a clean throttle window.
- Look: no.

**10. Eval declines are counted** - C442, C446
- Files: Game_Eval.cpp, Game_DevCommands.cpp, Game_DevWorld.cpp, Game_DevParty.cpp, Game_DevEval.cpp, dungeondelete.eval, worldprops.eval, dungeonrefuse.eval.
- C442, in two commits:
  - First: add `expect-refuse <line>` and move the three probing scripts onto it.
  - Then: turn the Print-declines into Refuse; make `editor move`/`erase` report the truth; make unknown `editor`/`profile` verbs refuse; route arity errors through devargs::Need.
- C446: setskill refuses ids that are not in TrainableSkills.
- Judge: the Eval suites and Eval.ps1 -SelfTest. ADD:
  - a self-test script whose declined setup line must fail;
  - `expect-refuse setskill 0 fyre 2`.
- Any suite that was silently declining shows up here and is fixed in the same commit.
- Look: no.

**11. Eval stop reasons and batch counts** - C443, C444, C445, C447
- Files: Game_Eval.cpp, Game_DevEval.cpp, Game_DevParty.cpp.
- C443: StepStop::PartyWiped, and `step` stops ticking at the wipe.
- C444: `rest until` refuses on NotPlaying, names every stop and uses kStepTicksPerSecond. It shares one stop-reason helper with `step`.
- C445: the batch runner keeps trying pending scripts in the same frame.
- C447: the console `throw` passes the held item's charge.
- Judge: expedition.eval (rung 4's `rest until 900` becomes `expect-refuse`) and Eval.ps1 -SelfTest. ADD:
  - `step` across a wipe prints PartyWiped, and the tally stops there;
  - a batch with an unreadable middle script reports exact `scripts=` and `failed=`;
  - a console-thrown, part-burnt torch keeps its charge.
- Look: no.

**12. Eval reset uses the eval level; reset readouts** - C300
- Files: DungeonWorld_Save.cpp, Game_Eval.cpp, Game_DevEval.cpp, tools/Eval.ps1.
- C300: reset reloads evalLevel, clears the stashes and the undo history, and refuses a `~` stem with a log line.
- ADD (this is the judge for batches 77-79): a `transients` readout, printed in both -SelfTest baselines. It reports:
  - active blasts and monster effects;
  - broken fixtures, decorations and doors;
  - the pending fall and the cursor item;
  - undo depth, resting and lockstep;
  - the throw cooldown and the kindle clock.
  Nothing is injected yet, so it passes today.
- Judge: Eval.ps1 -SelfTest with a NEW batched levelplay + resettest pair that must match its solo run. Recheck worldpersist.eval's "over another level" case.
- Look: no.

**13. levelcheck resolves models the way the loaders do** - C441
- Files: Game_DevWorld.cpp, AssetUtil.cpp, DungeonWorld_Load.cpp.
- C441: one model resolver, used by levelcheck and by the loaders, with no behaviour change. It covers today's per-category extension, the id fallback, empty_model / part2_model, and worn_<set>_<tier>.
- Judge: /check-ingame levelcheck. ADD mutation cases that must each fail:
  - a missing empty_model;
  - a missing part2_model;
  - a .glb named where the category loads .gltf;
  - a missing worn tier.
- See the C441 -> C301 order below.
- Look: no.

**14. CheckAll tiers and the check-\* skill files** - C213 (tier part only), C216, C481
- Files: tools/CheckAll.ps1, .claude/commands/check*.md, AllocTest.ps1 header, src/Core/ThreadManager.cpp comment.
- C213: RollTest joins the quick tier. EditorTest, WorldTest, LevelBuildTest and Eval.ps1 -SelfTest join the full tier.
- C216: the -Pause header and check-alloc.md stop claiming post-resume coverage. Fix the stale comments at AllocTest.ps1:896 and ThreadManager.cpp:160.
- C481: regenerate the check*.md files from `-List`.
- Judge: /check-selftest (AllocTest -Pause -SelfTest still fails). ADD: a CheckAll check that each check*.md lists exactly what `CheckAll.ps1 -List` and `Eval.ps1 -List` print.
- Look: no.

**Judge infrastructure, right after 0a**

**15. Guard evidence** - C214, C226, C215 (0g)
- Files: src/Core/AllocTrack.cpp/.h, StackTrace.cpp/.h, Log.h/.cpp, Game.cpp, DungeonWorld_Ledger.cpp, Effect/Effect.cpp, DevConsole_Snapshots.cpp.
- C214: stack capture starts in ArmFrame, keyed on the warm-up counter. A violation that printed nothing logs "N allocations, no stacks captured".
- C226: a full SeenSet makes FirstSighting return false and prints one "N further sites" line.
- C215: alloc::Excused goes inside log::Write and its templates. LedgerSubjectName keeps its own scope.
- Judge: AllocTest default and -SelfTest. ADD:
  - an -SelfTest case that does `allocpoke` on the first armed frame after a disarm and requires a stack line;
  - a DiagTest case that fills a SeenSet past 64;
  - an AllocTest step that swings a weapon with a typo'd on_hit inside the window and must still pass.
- Why here: every 0g judge reads these stacks.
- Look: no.

**16. Headless stays hidden; a load can be quit** - C391, C392 (0d)
- Files: src/Platform/Window.cpp/.h, Game.cpp, src/Main/Main.cpp, DevConsole.cpp, tools/Eval.ps1, CLAUDE.md.
- C391: the Window keeps a hidden flag. While it is set, Game's constructor skips ApplyDisplaySettings, and the window gets no SWP_SHOWWINDOW and no WS_VISIBLE.
- C392:
  - forward only VK_F4 to DefWindowProcW;
  - exempt `quit`/`exit` from the loading gate;
  - correct CLAUDE.md's "ways out" sentence.
- Judge: ADD:
  - Eval.ps1 -SelfTest requires IsWindowVisible to stay false through a headless run, with Borderless and Exclusive saved in a scratch settings.ini;
  - a harness case that types `quit` during a load, and another that posts Alt+F4 in Borderless; each must end the process cleanly.
- Why here: the harness shares settings.ini with Michael's own play.
- Look: yes, Alt+F4 in Borderless.

**0b - docs that teach the wrong thing**

**17. CLAUDE.md facts, each pinned by a check** - C473, C480, C460, C462, C463
- Files: CLAUDE.md, docs/combat.md, Combat.h, the tools/BuildFloor*.py comments, tools/AssetBaker/TextureBaker.cpp comment.
- C473: facing +1 is clockwise (on-screen right).
- C480: rewrite the debug vector-move paragraph.
- C460: there are eight damage types; name damagetypes.cat, drop the count in Combat.h, and natureResists is no longer "future".
- C462: name the real feature catalogs, including ceiling features.
- C463: list the full bake as tools/AssetBaker/Main.cpp runs it. Fix the TextureBaker comment unless C411 has already deleted that file.
- Judge: /check-build. ADD:
  - a RollTest case pinning facing +1 = TurnRight/StrafeRight;
  - a `static_assert(std::is_nothrow_move_constructible_v<...>)` beside the clip-vector reserve.
  The rest is checked by hand until P11's doc-name check.
- Look: no.

**18. CLAUDE.md UI paragraphs** - C455, C456, C457
- Files: CLAUDE.md, DialogLayout.cpp/.h, src/UI/Controls.h, TypeEditorDialog.cpp, InstanceInspector.h, GameUI.h/.cpp, GameUI_Items.cpp, docs/ui-hierarchy.md, assets/lang/*.lang.
- C455: replace both dialog-chrome paragraphs with a BuildDialogChrome / EditableTitle note, and restore EditableTitle's shrink-then-ellipsis.
- C456: rewrite the settings section around SettingsTab and the kSet* constants.
- C457: fix the HUD, UIContext-count and Player-map key paragraphs, and drop log.hands_empty from all five lang files.
- Judge: uioverlap on a dialog with a long id title (no TextOverrun, no escape); uioverlap on settings unchanged; InGameTest.
- The dead helpers wait for P7 (C92).
- Look: optional. Long dialog titles now trim.

**19. Effects and AI docs** - C451, C454
- Files: docs/effects.md, health-and-healing.md, "magic system.md", ai.md, CLAUDE.md, Effect/*.h, Spell/Stoneskin.h, DungeonWorld.h, DungeonWorld_Combat.cpp (comments), Spells.h, Magic.h, MonsterAI.h, CatalogSchema.cpp.
- C451:
  - status lines and headers describe the system as built;
  - recount kMaxEffects (4 wards + 3 DoTs + 2 supply + 4 sights + 3 lights = 16), raise it, and static_assert it against the counted worst case.
- C454:
  - ai.md's "Current implementation" describes four modes and six archetypes, and lists call-for-help as not built;
  - fix the MonsterAI.h and CLAUDE.md threading lines;
  - fix the `offense` and `faces` help strings.
- Judge: /check-build (the static_assert); SpellTest, PipelineTest and ThreadStress unchanged.
- Look: no.

**20. Game.h comments; script usage lines** - C466, C482
- Files: Game.h, Game.cpp, Game_Wiring.cpp, CLAUDE.md, tools/FetchTextures.ps1, FetchModels.ps1, SortTextureDownloads.ps1.
- C466:
  - rewrite the banner from AppState: Esc never quits, the sheet is not a pause, assets load from the repo, world switches happen in-process, and the roster can be resized;
  - move or delete the orphaned comments;
  - drop RestartApp's dead extraArgs and the `-newgame` comment;
  - fix CLAUDE.md's "Game.cpp is just..." line.
- C482: rewrite the usage lines in the -Command form, with real model names.
- Judge: /check-build. ADD: dry-run each rewritten usage line (-WhatIf where the script has it) and require it to select the named sets or models.
- Look: no.

**0c - combat**

**21. First-fight allocations** - C35, C49 (0g), C71
- Files: DungeonWorld_Combat.cpp, DungeonWorld.h/.cpp, DungeonWorld_Load.cpp, tools/AllocTest.ps1.
- C35: kDexStat and kStrStat move to namespace scope, and kNone becomes an empty span.
- C49 + C71: m_activeBlasts becomes a fixed array like m_pendingBolts, and a live blast is never evicted.
- C71: MakeMonster reserves the formation scratch.
- Judge: AllocTest -Melee with its first-blow warm-up removed (AllocTest.ps1:70-72), and -Impact with its first-detonation exemption removed (:1061). Both must pass.
- Why first in 0c: every later combat batch is then judged by the stricter modes.
- Look: no.

**22. Defender side** - C0, C11, C12
- Files: Defense.h/.cpp, Combat.cpp, Effect/Effect.cpp, DungeonWorld_Combat.cpp, tools/RollTest/Main.cpp.
- C0: one pure defense::Mitigate for both Deal branches. Rolled minus soak is floored at 0, and only a resist above 1 heals.
- C11: TrainDefense moves below the deflected return, and LessonFrom stops leaking the armour lesson on a pierce crit that skips soak.
- C12: one worn-pieces helper with one hand rule for Soak, Resist, WornArmorClass and DefenseFor. DefenseFor reads PartyTarget::Soak.
- Judge: RollTest and PipelineTest. ADD:
  - RollTest: soak above the raw roll never goes negative unless resist > 1;
  - RollTest: LessonFrom on a soak-skipping pierce crit;
  - eval: a Wind-Warded member deflects a bolt and gains no avoid xp and no DEX creep;
  - eval: a holdable test item with armor gives the same soak/resists in PartyTarget, WornArmorClass and `char`.
- Look: no.

**23. Where a flight ends** - C43, C44
- Files: DungeonWorld_Combat.cpp, DungeonWorld_Throw.cpp, Projectiles.cpp, tools/RollTest/Main.cpp.
- C43: a LastOpenCell helper, shared with LandThrown. A burst bolt stopped by a shut door or a one-thick wall detonates on its open side; keep the phantom for spareCentre.
- C44: a bolt that breaks on a wall delivers its on-hit in that last open square.
- Judge: pipeline.eval door section (wall-face arrivals go from 4 to 1), Eval blast-geometry, PipelineTest. ADD:
  - RollTest: open cells on both sides of a solid centre;
  - eval: a firebolt breaking on the wall behind the party burns the member in the last open square.
- Eval numbers move.
- Look: no.

**24. Burst bolts on the party; the gust** - C1, C18
- Files: DungeonWorld_Combat.cpp, DungeonWorld_Load.cpp (ThreatProfile), Projectiles.h, tools/EvalScripts/spells.eval.
- C1: a local Detonate after the lane check in ResolveMonsterProjectileHit, with ThreatProfile re-priced.
- C18: a gust's repel scales the shot's blast and procs by the same share as its direct damage. A spent shot is removed without detonating.
- Judge: SpellTest, PipelineTest, AllocTest -Hand / -Impact. ADD:
  - spells.eval case: skel_magus's burst bolt hits the party and `blasts=` moves;
  - eval: a gust that weakens a magus burst bolt shrinks its blast and procs.
- Question for Michael: does a Wind Ward that deflects a burst bolt also stop its blast? The proposed answer is yes, because a turned bolt does not land.
- Look: yes, magus fights get harder.

**25. Spells and effects** - C9, C17, C19, C279
- Files: DungeonWorld.cpp, Effect/LightEffect.cpp, effects.cat (project + template), lang x5, DungeonWorld_SpellLight.cpp, Spell/ModifiedSpell.cpp, Spell/BoltSpell.cpp, Spell/AllSpells.cpp, Effect/WardEffect.cpp, Game_DevParty.cpp.
- C9: each effect kind names its own fade line (effects.cat `fade_party` / `fade_monster`, plus keys x5), and the category switch goes.
- C17: the flare reaches by step count using Detonate's open-square test, keeps a fixed list of dazzled monsters, scorches only those, and kindles nothing through rock.
- C19: a modified bolt carries its own spells.cat on_hit and push, and its defaults are derived after SpellBook::Build.
- C279: the ward constructors set m_school.
- Judge: SpellTest, AllocTest -Light / -Cast, PipelineTest. ADD:
  - SpellTest: a Firelight running out prints its own fade line, never log.sight_fades;
  - SpellTest: a firebolt_volley bolt carries burn 1 4;
  - eval: a flare beside a shut door dazzles nothing beyond it and kindles no sconce through rock;
  - `effect stoneskin ahead` then `effect fireshield ahead` leaves both wards held.
- Look: optional.

**26. Party attack odds and ends** - C5, C36, C39
- Files: DungeonWorld_Combat.cpp, DungeonWorld_Throw.cpp, GameUI_Items.cpp, Balance.cpp, lang x5.
- C5: skip the enchantment burst when ev.slew, and guard MonsterTarget::Wound against the dead.
- C36: name the stat through attr.<kStats[i].id>; the throw's stats become full ids in Balance; drop the stat.stamina and stat.health keys.
- C39: Punch, Kick and a skill-less held item swing as unarmed.
- Judge: PipelineTest. ADD:
  - eval: a killing blow with Sera's lit torch moves `slain` by exactly 1 and prints no "locks onto" line;
  - eval: INT and WIL points print attribute names, and no `stat.` key appears in dungeon.log;
  - eval: a Punch with a key in hand trains unarmed.
- Look: no.

**27. Throws and the fumble** - C10, C40, C47 (0h)
- Files: DungeonWorld_Combat.cpp, DungeonWorld_Throw.cpp, DungeonWorld_Save.cpp, Projectiles.cpp, Game.cpp, CLAUDE.md.
- C10: a severe fumble drops the held item with its charge and its kind's id, clears the hand, and logs from kind.nameKey.
- C40: throwing xp is awarded on contact, before the Detonate branch. A wall shatter does not count unless Michael says so.
- C47: a save writes in-flight cargo as a floor item at its landing square, without landing the live flight. Update CLAUDE.md THROWING to match.
- Judge: bombs.eval. ADD:
  - eval: a forced severe fumble with a part-burnt torch drops it with its charge;
  - eval: a flask hitting a monster moves throwing xp;
  - eval: saving with a fire flask in flight leaves party health unchanged, the save holds the flask at its landing square, and the flight continues;
  - AllocTest -Swing (P2's mode, built now): a party swing plus a forced severe fumble inside the window. It needs a dev hook that forces the next swing to fumble.
- Look: no.

**28. Melee target and cosmetic RNG** - C33, C73
- Files: DungeonWorld_Combat.cpp, DungeonWorld.cpp/.h.
- C33: PickMeleeTarget takes the front slot in the attacker's lane.
- C73: animation clip choice draws from a cosmetic RNG stream.
- Judge: the Eval suites (smallparty and crypt2 numbers move once) and Eval.ps1 -SelfTest. ADD:
  - eval: two skel_swarm in one square; Brand, front-left, hits the front slot in his lane;
  - a seeded sweep with one extra cosmetic clip on a monster matches the sweep without it.
- Look: no.

**29. A missed attack is noticed** - C34 (HOLD for Michael)
- Files: Effect/Effect.h/.cpp, DungeonWorld_Combat.cpp, CLAUDE.md.
- C34: after Deal, an ITarget::Noticed(ev) hook runs for every non-Tick event that reached a target. A monster provokes and credits threat; the party breaks rest.
- Judge: PipelineTest. ADD:
  - eval: a missed shot wakes an asleep monster;
  - rest.eval: a missed or turned swing ends rest as `attacked`.
- This changes CLAUDE.md's definition of rest's `attacked` ("a blow"). If Michael keeps that definition, land only the provoke half.
- Look: yes, stealth.

**30. Resource rule defaults** - C361, C362
- Files: Resource.h, Character.cpp, Game_Party.cpp, Balance.h/.cpp, BalanceDialog.cpp.
- C361: default curve caps of 0 make resource::Rules{} inert.
- C362: curve-form knobs are clamped at load and read through validated SkillForm() / StatForm().
- Judge: the resources and tiers evals unchanged. ADD:
  - RollTest: RecomputeMaxima({}) adds no practice term;
  - RollTest: out-of-range and negative forms clamp.
- Look: no.

**31. Doors and doorways** - C357, C65, C356 (0h)
- Files: DungeonWorld_Doors.cpp, DungeonWorld_Move.cpp, DungeonWorld.cpp, Validate.cpp, Game_Wiring.cpp.
- C357: one DoorwayOccupied (party and live monsters) for the three close sites, plus an optional Validate rule.
- C65: gameplay and editor door moves use the live-only occupancy query. The corpse-inclusive query is kept for inspection only.
- C356: the door inspector edits only initialOpen on a wrecked leaf, and refuses to close on a monster.
- Judge: ADD:
  - eval: a lever in a doorway wired to its own door is refused while the party stands there;
  - eval: kill a monster in an open doorway, and `opendoor` still closes the door, before and after save/load;
  - EditorTest: a smashed door stays open when the inspector closes it, and closing on a monster is refused.
- Look: no.

**0c - AI, and 0f - threads**

**32. AI pools allocate nothing** - C62, C66, C70 (0f)
- Files: MonsterAI.cpp/.h, DungeonWorld.cpp, DungeonWorld_Load.cpp, tools/AllocTest.ps1.
- C62: the BFS queue becomes a Brain-member vector walked by a head index. The inline Brain is pre-sized at level load.
- C66: both pools are filled at level load to kBucketCount + 2 map-sized grids, and a warning fires once if either grows.
- C70: a release/acquire in-use flag replaces `use_count()==1`.
- Judge: /check-threads; AllocTest default and -Melee, which require the growth warning to be absent. ADD: AllocTest -Rest (P2's mode, built now). It wounds the party, then rests with an aware monster in aggro behind a shut door.
- Look: no.

**33. Where a monster can stand** - C58, C57, C74
- Files: DungeonWorld.cpp, DungeonWorld_Load.cpp, DungeonWorld_Throw.cpp.
- C58: one "a monster can stand here" predicate for the side list, FreeSlotInCell and BuildAISnapshot; delete CellFreeForMonster.
- C57: only Engage intents take attack sides.
- C74: the predicate refuses stair and pit holes, and drops and throws use one "can this rest here" check.
- Judge: the Eval suites (arena, ladder) and AllocTest -Melee. ADD:
  - eval on crypt1: with the party at (8,4), bolt the skeleton at (10,4); it must reach a side and swing within a few seconds;
  - eval: with a skel_mage and a brute in a corridor dead end, the brute reaches a side and swings;
  - eval: a drop or throw onto a pit square is refused or lands below, and no monster steps onto a pit square.
- Eval numbers move.
- Look: yes, a crypt1 fight.

**34. AI director lifetime** - C52, C63 (0f), C69 (0f)
- Files: DungeonWorld_Save.cpp, DungeonWorld.cpp, MonsterAI.cpp, src/Core/ThreadManager.cpp/.h, DevConsole_Threads.cpp, tools/ThreadStress.
- C52: ResetForNewGame gives every monster a fresh runtimeId (or bumps a plan epoch), so a stale batch is dropped.
- C63: lockstep waits for State::Paused before the inline compute, and Restart keeps `paused`.
- C69: ~AsyncDirector calls a new Manager::Remove(id).
- Judge: /check-threads, and Eval.ps1 -SelfTest unchanged. ADD:
  - an async-AI eval (lockstep off): fight on crypt1, go to the title, then Continue and Start New Game; `monsters` must show every sleeper aware=0;
  - a ThreadStress case that enters lockstep while a worker is mid-scan;
  - after a LoadWorld, `threads` shows no Dead ai.bucketN slot.
- Look: no.

**35. Rest at 60x** - C64, C48
- Files: Game.cpp, DungeonWorld.cpp, DungeonWorld_Combat.cpp, Projectiles.cpp/.h.
- C64: rest's dt runs in fixed sub-steps with a per-frame cap. There is still one multiplier site.
- C48: ProjectileSystem::Update sub-steps to at most half a square.
- Judge: rest.eval, expedition.eval, AllocTest -Rest. ADD:
  - an eval measurement: rest with the real multiplier beside a chaser, and compare squares moved and thinks per simulated second with awake play;
  - a dev hook running one 60x-dt update without sub-steps: a shot at a resting party ends rest as `attacked` and stops at a one-thick wall.
- Look: yes. The cap slows rest on a slow frame.

**36. Thread registry** - C386, C387 (0f)
- Files: src/Core/ThreadManager.cpp/.h, StackTrace.cpp, tools/HealthTest.ps1, CLAUDE.md.
- C386: shared_ptr Workers (or a "restarting" mark), so threadreap cannot free a worker the supervisor is reading or restarting.
- C387: the supervisor's stall record and Kill record carry WalkThread frames.
- Judge: /check-threads and HealthTest. ADD:
  - a ThreadStress phase that races threadreap against supervisor restarts of a wedged worker;
  - a HealthTest expectation that the threadwedge stall line carries walked frames;
  - a `threadkill <id>` command, so the Killed kind is covered.
- Look: no.

**37. Crash path order** - C385, C388 (0f)
- Files: src/Core/Diagnostics.cpp/.h, CrashHandler.cpp/.h, ThreadManager.cpp, src/Main/Main.cpp, the crashpoke command, tools/HealthTest.ps1.
- C385: the handlers record quietly, write the dump, then log once.
- C388: SetThreadStackGuarantee on the main thread and on workers (or a pre-created dump thread).
- Judge: HealthTest (the expectation at :141 becomes one log line after the dump) and DiagTest. ADD: a `crashpoke overflow` case that requires a dump and the fault line.
- Warn Michael before the deliberate crash runs.
- Look: no.

**0g - allocation**

**38. Exit stair and the Help line** - C210, C217
- Files: Game.cpp, GameUI.cpp, Game_World.cpp, DungeonWorld.cpp/.h, GameSettings.cpp, docs/lighting-updates-plan.md, docs/message-allocation.md, tools/AllocTest.ps1.
- C210: a PromptActive term in `quiet`, and a pending-fall string that keeps its capacity.
- C217: cached key names plus loc::FormatLine for the Help line. Drop ReturnToTitle's redundant excuse, and remove the second policy from the docs.
- Judge: ADD:
  - AllocTest -Exit (P2's mode): step onto eval_arena's exit stair, answer No, then step into a pit. It refuses a PASS unless the prompt opened and the fall ran;
  - an AllocTest step that clicks the log corner's Help button inside the window.
- Look: no.

**39. Levers and niches** - C211
- Files: DungeonWorld_Doors.cpp, DungeonMap.cpp/.h, DungeonWorld.cpp, tools/AllocTest.ps1, an eval_arena addition.
- C211: ToggleNichesNamed returns a count into a fixed buffer. For the reveal, prefer a pre-built opened chunk. A once-per-niche alloc::Excused is the fallback, and it needs Michael's OK because it is an event exemption.
- Judge: ADD AllocTest -Lever (P2's mode): one press that matches no niche and one that reveals a named niche. It refuses a PASS unless both landed.
- Look: no.

**40. Long ids, quest and flag events** - C218, C212
- Files: Inventory.h, Game.cpp, Project.cpp, DungeonWorld_Light.cpp, Game_World.cpp, WorldMap.cpp/.h, DungeonWorld_Combat.cpp, tools/AllocTest.ps1.
- C218: the ItemSlot and HeldItem constructors (copy included) reserve one capacity, catalog load warns on longer ids, and the comment is fixed.
- C212:
  - delete the comment that revives the event exemption;
  - string_view find-first WorldState setters and catalog-sized stores;
  - the fumble's held-id copy uses HeldItem's reserve.
- Judge: AllocTest -Swing covers the fumble half. ADD:
  - AllocTest -Items -LongId: swap short-id runes through the cursor, then lift one of crypt1's 17-21 character potion ids;
  - an AllocTest step that lifts crypt_token twice and presses a `sets=` lever (it uses batch 39's level).
- Look: no.

**41. Effect warm-ups** - C219, C228
- Files: CharacterPanel.cpp, src/UI/Controls.cpp, Game.cpp, Character.cpp.
- C219: Warm(fx::kMaxEffects) in the CharacterPanel constructor. This also covers the Minimal cards.
- C228: ResetRoster reserves every member's effects after both assignment paths.
- Judge: ADD:
  - AllocTest -Effects: `autocast hold` with a ward, released on the first armed frame. It refuses a PASS unless a member's effect count rose. Run it under -Minimal too;
  - an AllocTest -Party run that grows the roster (a party of 1, then `newparty default`) and lands an effect on member 4.
- Look: no.

**42. Text capacity** - C220, C371 (0d)
- Files: CharacterSheet.cpp/.h, CharacterSheet_Lists.cpp, Spell/AllSpells.cpp, ItemDetailsDialog.cpp, src/Core/Loc.cpp.
- C220: the spell-row warm-up is sized from the spell registry, plus `static_assert(kValueCap >= loc::Line::kCapacity)`.
- C371: descriptions assign loc::View(key) directly, and Line::Assign backs off to a whole UTF-8 character.
- Judge: AllocTest -Sheet. ADD:
  - an -Sheet variant with a member taught every spell (open Known Spells and the party window);
  - a RollTest case for Line::Assign at the 255-byte boundary;
  - an eval: `lang de`, `itemdetails rune_light`, then `itemdetails status` prints the full length.
- Look: yes, German and Russian descriptions read whole.

**43. UI per-frame strings** - C223, C224 (0d)
- Files: src/UI/TreeInspector.cpp, UIContext.cpp, Controls.h/.cpp, WorldMapView.cpp.
- C223: alloc::Excused after uitree's enabled check.
- C224: DrawButtonFace takes string_view, the kNoLabel / kNoSelection statics go, and WorldMapView uses loc::View plus ui::FitText.
- Judge: ADD:
  - an AllocTest run with `uitree on` and the console closed, hovering the HUD;
  - an InGameTest world-map sweep under ru.lang with uioverlap. A trimmed label must report TextOverrun and never split a character.
- Look: no.

**44. Fonts** - C89 (0d), C221, C229
- Files: AssetDialog.cpp, src/UI/UIContext.cpp/.h, Font.cpp/.h, FontLibrary.cpp/.h, FloatingPanel.cpp, GameUI.cpp, src/Core/Loc.cpp, AllocTrack.h, CLAUDE.md.
- C89: AssetDialog's context comes from the FontLibrary. Delete the owned-font constructor and Font::SetHeight, and update CLAUDE.md.
- C221: font sizes hold at the drag-start scale until release, and trial scales are measured from vertical metrics.
- C229: pre-warm each .lang file's code points on language load, and drop the glyph-miss excuse.
- Judge: uioverlap over the asset dialog at 900p and taller; /check-build in both configs; AllocTest -Panels. ADD:
  - a FontLibrary count / high-water readout that must stay unchanged across the Hands-grip and sheet resizes;
  - an AllocTest run under language=ru, and under en hovering an em-dash tooltip.
- Look: yes. Text holds its size during a panel drag.

**45. Conjured pebbles** - C227 (HOLD for Michael)
- Files: DungeonWorld_Load.cpp, Spell/Rock.cpp, DungeonWorld.h, tools/AllocTest.ps1.
- C227: a fixed recycle pool for conjured drops that reuses the oldest uncollected pebble.
- Judge: ADD an AllocTest -Hand variant that casts Rock with full hands past 64 drops.
- If Michael rejects recycling, this becomes P2's self-reporting reserve.
- Look: no.

**0d - platform and UI**

**46. Typed text is UTF-8** - C383
- Files: src/Platform/Input.cpp, Window.cpp, src/UI/Controls.cpp (TextField), DevConsole.cpp, MapEditor palette filter, PartyRules.h, tools/TypingTest.ps1.
- C383: WM_CHAR, surrogate pairs included, is encoded to UTF-8 in TypedChars. Consumers walk code points and delete whole characters, and NameValid counts characters.
- Judge: RollTest (party creation section). ADD: a TypingTest non-ASCII case with u-umlaut, Cyrillic, a U+010D that must not submit, and a surrogate pair.
- Look: optional.

**47. Files and paths** - C231 (0f), C384
- Files: src/Assets/File.cpp, Image.cpp, Wav.cpp, src/Audio/AudioEngine.cpp/.h, src/Core/Profile.cpp, Paths.cpp, src/Platform/FileDialog.cpp, src/Main/Main.cpp, the exe manifest.
- C231: unique_ptr deleters and ComPtr at the C-API boundaries, and a checked ftell.
- C384: a UTF-8 activeCodePage in the exe manifest (or wide opens), using C383's helpers.
- Judge: /check-build (both configs plus the DN_PROFILE preset). ADD:
  - ReadBinaryFile on a directory returns empty;
  - the game and an AssetBaker import both run from a folder with a non-ASCII name.
- Look: no.

**48. UI clip and menu padding** - C208 (0f), C382
- Files: src/UI/Widget.cpp/.h, UIContext.cpp/.h, Controls.cpp, FloatingPanel.cpp, src/Platform/Input.h.
- C208: an RAII restore wherever a widget pushes a clip, and the clip is cleared at the start of UIContext::Update and Render.
- C382:
  - ContextMenu and MenuList draw at Rem() offsets;
  - ContextMenu reports TextOverrun;
  - vk::Control and Core's kPi replace FloatingPanel's copies.
- Judge: InGameTest and uioverlap (no screen may change); uioverlap with the bare-hand menu open below 900p. ADD:
  - a HealthTest case: crashpoke throws inside a ScrollArea's walk, and a later click outside it still lands;
  - a nested ScrollArea-in-tab poke whose later sibling still draws and takes the click.
- Look: no.

**49. Etched gold follows the material** - C204
- Files: tools/BuildEtchGlyphs.py, assets/ui/etch_*.png, src/UI/Controls.cpp, Skin.cpp, CLAUDE.md.
- C204: re-bake the grooves light-only, plus a white gold-floor mask tinted with CarvedGold / CarvedLit. Correct CLAUDE.md.
- Judge: ADD:
  - a `uimaterial` sweep logging the etched gold's contrast per material beside ResolveInks;
  - an InGameTest screenshot of the movement pad on a snow material.
- Look: yes, the movement pad on several materials.

**50. One scrollbar** - C127
- Files: src/UI/Controls.cpp/.h, Skin.cpp, new Controls_*.cpp.
- C127: a non-widget ui::ScrollBar shared by DropDown and ScrollArea. The ink solver moves to Skin.cpp, then the file is split by widget family.
- Judge: uioverlap; InGameTest (settings and Material dropdowns, party page). ADD: a thumb-drag and wheel check on a long DropDown.
- Look: optional.

**51. Party page** - C370, C109, C112 (0g)
- Files: GameUI.cpp, GameUI_Party.cpp, Game_Party.cpp, PartyCreationPage.cpp/.h, tools/EvalScripts/partypage.eval, CLAUDE.md.
- C370: RebuildForLanguage rebuilds whichever m_savesUi sub-page is showing.
- C109: one function gives a spec's shown name, face and colour.
- C112: Tick refreshes only on m_previewDirty or a selection change, and the CLAUDE.md line is fixed.
- Judge: partypage.eval and InGameTest sweep_partycreation. ADD:
  - a `lang <code>` step with the page open, run on debug;
  - clearing a premade member's name shows the premade name;
  - an AllocTest mode with an AllocTest-only switch that arms the guard on the idle page.
- Look: yes.

**52. Title and saves** - C366, C364, C367, C207 (0h), C349 (0f)
- Files: GameUI.cpp, Game.cpp, SaveGame.cpp/.h, Game_Editor.cpp, Game_DevCommands.cpp.
- C366: one has-saves flag per list, rebuilt in ReturnToTitle.
- C364: a shared BeginPlay tail.
- C367: a failed delete reports its error code.
- C207: ListSaves(world) takes the current project's world, and the global goes.
- C349: bounds checks and from_chars on member and pack indexes.
- Judge: ADD:
  - InGameTest: save, Esc, Return to Main Menu, and the title shows Continue and Load;
  - eval: from crypt2, Start New Game shows the intro;
  - deleting a read-only save gives a message and a log line;
  - WorldTest: switch world, save, and `load` lists that save; a type delete referenced only by that save refuses;
  - eval: a save with `char -1` and `packc 0 -1` is refused cleanly.
- Look: yes.

**53. One world tick** - C78 (0h), C125
- Files: Game.cpp, MapView.h.
- C125: one TickWorld(input, wdt, acceptInput) for the console, overlay, sheet and Playing paths, with a single kNoInput. This is also C78's one world-frozen decision.
- C78: SetMode clears the pause only when the mode actually changes.
- Judge: the Eval suites unchanged. ADD:
  - EditorTest: pause, then bare `editor` and an `editor tool` subcommand; the pause holds;
  - the console over the sheet advances the world clock, over a paused editor it does not, and a transition raised under the console is consumed.
- Look: no.

**54. Sheet content** - C372, C477, C263
- Files: CharacterSheet_Inventory.cpp, CharacterSheet_Lists.cpp, CharacterSheet_Status.cpp, CharacterSheet.h, CharacterPanel.cpp, PartyHudDraw.h/.cpp, PartyHud.h, HandSlot.h, SpellbookPanel.h, SlotGrid.h.
- C372: restore the avoidance-skill row; the rows array grows to 9, with an assert.
- C477: the status line uses SkillBarColor, SkillRow::tint goes, and the comments are fixed.
- C263: one DrawEffectIcon through DrawSlotFace.
- Judge: uioverlap hud and sheet; AllocTest -Sheet; InGameTest sweep_sheet. ADD:
  - a `sheet` readout of the armor tooltip rows (an unarmored member's rows sum to Roll);
  - `sheet status` prints the skill-name colour;
  - a PrintWindow shot of the HUD effect strip.
- Look: yes.

**55. Sheet size** - C96
- Files: GameUI.cpp, CharacterSheetLayout.h, CharacterSheet.h/.cpp, PartyWindow.cpp.
- C96: the sheet is sized in em, like PartyWindow::SizeForEm.
- Judge: AllocTest -Sheet / -All; uioverlap. ADD: an InGameTest sheet sweep at 2560x1080 and 1920x1200, with `sheet status` printing a square pack cell.
- Look: yes, an ultrawide window.

**56. Map view** - C373, C374, C376
- Files: MapView.cpp/.h.
- C373: wheel zoom goes through ComputeTransform before and after.
- C374: clear the hover before a toolbar callback.
- C376: delete VariantTint.
- Judge: InGameTest player-map sweep. ADD:
  - EditorTest: wheel-zoom at a fixed cursor keeps CellAt, and pan is unchanged at zoom 1 and 10;
  - an `editor` status field showing the hover is cleared after Level opens.
- Look: yes, zoom feel and the player-map tint.

**57. Dev console** - C379, C380, C381, C271
- Files: DevConsole.cpp/.h, DevConsole_Threads.cpp, DevConsole_Health.cpp, DevConsole_Panel.h, DevConsole_Profile.cpp.
- C379: clicks and ProfileHover below m_panelH are ignored.
- C380: a cell stores the newest event of its worst kind.
- C381: HealthRow::prev is sized by diag::kKindCount.
- C271: the semantic colours are named once.
- Judge: /check-build (plus DN_PROFILE) and /check-profile. ADD:
  - a /check-threads click below m_panelH with THREADS expanded changes no worker state and writes no settings.ini;
  - HealthTest: a stall and its restart in one cell, where clicking names the stall;
  - a PrintWindow shot of the perf panel.
- Land before P1 moves DevConsole.
- Look: optional.

**58. Asset loaders** - C394, C360 (0f), C396 (0e)
- Files: src/Assets/ObjLoader.cpp, Model.cpp, tools/AssetBaker/MipBaker.cpp.
- C394: parse `v//n` before `v/t/n`, and warn on faces with no normal.
- C360: a default material when a glTF has none.
- C396: sidecars are named by cgltf image index in both the loader and the baker. Then re-run `AssetBaker model-images`, and re-provision the other worktrees.
- Judge: the 89-tile picker survey. ADD:
  - a `v//n` OBJ fixture imports with non-zero normals;
  - a no-material glTF fixture draws through `preview`;
  - a model with a data: URI image ahead of a file image binds each sidecar by index.
- Look: no.

**0e - rendering and the device**

**59. Sprite arena** - C163
- Files: src/Graphics/SpriteBatch.cpp, UploadAllocator.cpp, MapView.cpp, GenerateKnobs.cpp.
- C163: size the arena for a 128x128 editor map; an overflow drops and counts; add a high-water gauge beside the SRV gauge.
- Judge: ADD an EditorTest phase that generates at 128x128 and opens the editor at fit zoom. It must not abort, and the gauge must show drops=0.
- Look: no.

**60. Shadow cache** - C178, C187
- Files: ShadowScheduler.cpp/.h, DungeonWorld_Render.cpp, DungeonWorld.cpp, DungeonWorld_Combat.cpp, DungeonWorld_Doors.cpp, src/Graphics/Renderer.h, CLAUDE.md.
- C178: a caster revision (a fixed list of changed casters) for doors, smashed props and vanished corpses.
- C187: match the incumbent by id, count a move past the wander, delete the dead counter, and fix the CLAUDE.md text.
- Judge: AllocTest -Lights. ADD:
  - a per-slot re-render readout, plus an -Lights step that opens a door with the party standing still and requires slot 0 to re-render;
  - a carried Firelight's cube re-renders while the party walks.
- Look: yes, a door's shadow.

**61. Animators** - C189, C183, C395
- Files: src/Graphics/Renderer.cpp, DungeonWorld_Render.cpp, DungeonWorld_Models.cpp, DungeonWorld_Load.cpp, src/Animation/Animator.cpp/.h, AssetPicker.cpp.
- C189: a persistent rest animator per kind keys the palette cache.
- C183: the map head shot poses that animator on the idle's first frame, with FitToPose.
- C395: one monster-Animator factory that calls LockRootTravel; a same-clip Play mid-fade is a no-op.
- Judge: the rootmotion eval, the picker survey, AllocTest default. ADD:
  - a map-icon survey of every monster kind beside its picker tile;
  - the palette upload count for two same-size rigs baked in one frame;
  - an Animator case for a same-clip Play mid-fade.
- Look: yes, the editor map's monster icons.

**62. Light candidates** - C181, C190
- Files: DungeonWorld.cpp, DungeonWorld_Light.cpp, DungeonWorld.h, DungeonWorld_Load.cpp.
- C181: a null guard in the fire loop, plus a header comment naming both null cases.
- C190: the category default moves above the enchantment block in ItemKindFor.
- Judge: AllocTest -Lights. ADD:
  - a `lightstress` variant that fills the 256 candidates before the fire loop runs;
  - an eval that drops flamebrand and frostbrand and reads element-coloured floor glows from `lights`.
- Look: optional.

**63. Floor items: pose and pick** - C180, C359, C258
- Files: DungeonWorld.cpp, DungeonWorld_Render.cpp, DungeonWorld_Light.cpp, DungeonWorld_Load.cpp, DungeonWorld.h, src/Graphics/Camera.h, DungeonWorld_Doors.cpp, DungeonWorld_Fires.cpp.
- C180: one FloorItemPose (hidden + upright) at the five sites.
- C359: the grounded height is cached per kind from FloorItemWorld, and GroundOffsetY is deleted.
- C258: one ray-sphere helper in units x kUnit, and the comment at Doors.cpp:276 is fixed.
- Judge: AllocTest -Items / -Throw / -Lever. ADD:
  - a rune in a shut niche has no floor_glow in `lights`;
  - a ray at a torch's drawn height hits it;
  - clicks on a niche item, an opener and a sconce land at their drawn spots.
- Look: yes, clicking small floor items.

**64. Load-path lifetimes** - C193, C154, C222 (0g), C471
- Files: DungeonWorld_Load.cpp, DungeonWorld_Arena.cpp, DungeonWorld_Save.cpp, DungeonWorld_Models.cpp, DungeonWorld_LevelIO.cpp, DungeonWorld.h, src/Assets/Model.h, AssetUtil.h/.cpp, Game.h, docs/ARCHITECTURE.md.
- C193: WaitIdle inside BuildDungeonMeshes, before the clears.
- C154: a quality swap reloads m_propTextures in place at the new tier.
- C222: the CPU images are dropped after BuildMultiMaterialModel.
- C471: a missing `_n` map gets a flat normal and one warning.
- Judge: Eval.ps1 -SelfTest and `arena` under the D3D12 debug layer with zero errors (WARP if possible). ADD:
  - a texture-resolution readout after `quality` Low -> Ultra -> Low, with the SRV live and peak as documented;
  - pinned CPU image bytes read 0 after a level load;
  - a levelcheck case for a set with no `_n` map.
- Look: yes, a Low -> Ultra swap.

**65. Thumbnails** - C158, C111
- Files: AssetUtil.h, AssetPicker.cpp, PortraitPicker.cpp, ThumbCache.h, DungeonWorld_Editing.cpp, Game.cpp.
- C158: LoadTextureThumb defaults to linear, and the swatches use linear thumbs.
- C111: eviction never takes a tile that is on screen.
- Judge: ADD:
  - a survey comparing each tile's and swatch's mean colour with the set's linear albedo mean;
  - a dev knob that forces the cache cap below the visible count, and a status line showing no visible tile was evicted.
- Look: yes, picker brightness.

**66. Node transforms** - C253
- Files: src/Assets/Model.h/.cpp, DungeonWorld_Load.cpp, DungeonWorld_Models.cpp, tools/AssetBaker/ModelImport.cpp.
- C253: one assets::BakeNodeTransform at the seven sites: inverse-transpose normals, flipped winding under a mirror, and skinned meshes left alone. The multimaterial cull radius comes from the bounds.
- Judge: geomhash unchanged; AllocTest -Glass; the 89-tile survey, rigged tiles included. ADD:
  - baking a mirrored, non-uniformly scaled node;
  - viking_dagger's cull radius read from its bounds.
- Look: yes, do the survey with Michael.

**67. GPU failure evidence** - C195
- Files: src/Graphics/D3DUtil.h, GraphicsDevice.cpp, src/Core/CrashHandler.cpp.
- C195: DN_HR logs the HRESULT, the device-removed reason and the DRED breadcrumbs before ReportFatal.
- Judge: /check-health. ADD: a `crashpoke devremoved` HealthTest case (ID3D12Device5::RemoveDevice).
- Look: no.

**68. Display command, Windowed placement, relaunch** - C196 (0d), C398 (0d)
- Files: GameUI.cpp, src/Platform/Window.cpp, Process.cpp, src/Graphics/DisplayEnum.cpp, Game.cpp/.h, src/Core/Log.cpp.
- ADD first: a `video` dev command with three forms: `status` (staged vs running adapter, monitor, mode and size), `apply` (drives OnVideoApply) and `restart` (reaches RestartApp).
- C196: Windowed seeds from the live client size, clamps to the monitor's work area, and centres there.
- C398: rebuild the command line from GetModuleFileNameW plus the original arguments, quit only on success, and have the child wait for the parent before opening the log.
- Judge: ADD:
  - the window rect lies inside the chosen monitor's work area after `video apply`;
  - the relaunched child keeps `-project`, and the parent's log is intact.
- Look: yes.

**69. Monitors and adapters** - C198, C199 (0d), C197
- Files: src/Graphics/DisplayEnum.cpp/.h, GraphicsDevice.cpp, Game.cpp, GameUI.cpp, Game_Wiring.cpp, src/Platform/Window.cpp, GameSettings.cpp, CLAUDE.md.
- C198: monitors are enumerated independently of the rendering adapter.
- C199: one display list, refreshed on Settings open and on WM_DISPLAYCHANGE. The monitor is saved by DeviceName after a successful apply.
- C197: the adapter is saved by a stable identity, and the running adapter is staged.
- Judge: ADD:
  - a WARP boot shows non-empty monitor and resolution lists, and Borderless applies through `video apply`;
  - a simulated display-change refresh;
  - an ini round-trip of the monitor;
  - a RollTest case resolving a saved adapter identity.
- Look: yes, two monitors and a laptop.

**70. Fullscreen, refresh, DPI** - C194, C200, C201 (0d)
- Files: src/Graphics/GraphicsDevice.cpp, src/Platform/Window.cpp, Game.cpp, GameUI.cpp, src/Main/Main.cpp, src/Main/CMakeLists.txt (manifest).
- C194: BeginFrame detects a lost exclusive state and calls ResizeBuffers, Present is skipped while minimized, and the game re-enters exclusive on WM_ACTIVATEAPP.
- C200: one fractional-rate formula for the frame cap and its labels.
- C201: PerMonitorV2, AdjustWindowRectExForDpi and WM_DPICHANGED.
- Judge: /check-profile. ADD:
  - `display drop`: the following frames present, and the log shows the buffers were recreated;
  - a RollTest frame-cap section (59.94 Hz, 165 Hz at interval 2);
  - a DPI boot log line, checked by InGameTest.
- Look: yes, Alt+Tab from Exclusive and a scaled display.

**0h - data and editor**

**71. Type editor** - C101, C100, C235 (0f)
- Files: TypeEditorDialog.cpp, CatalogSchema.cpp, Game_Wiring.cpp, src/Game/CMakeLists.txt.
- C101: add the damage-type case, plus /we4062 on the Game lib. Fix every switch the warning flags.
- C100: a uniqueness check before moving text_<id>.
- C235: swatches are resolved at draw time.
- Judge: /check-build in both configs; worldquest.eval. ADD:
  - a `typeset dialog rows` status and an EditorTest step that demands a widget for every schema row;
  - typing stage2 -> stage10 keystroke by keystroke keeps stage1's text;
  - a theme open in the type editor, then `quality low`, leaves no fault in the log.
- Look: no.

**72. Catalog sweeps and writes** - C305, C306, C323
- Files: DungeonMap.cpp, DungeonWorld_Editing.cpp, Game_Editor.cpp, TypeEditorDialog.cpp, Project.cpp, Effect/Effect.cpp, Serialize.cpp, Catalog.cpp.
- C305: a TypeRecords family for surface features.
- C306: an "identity is C++" flag per catalog refuses renaming effects, attacks and spells.
- C323: the block writer stops dropping comments: header-only files, deleting the first entry, and monster-config saves.
- Judge: rename.eval and `catround`. ADD:
  - a floorfeature type rename follows its records and a delete refuses;
  - `typeset rename` is refused on effects, attacks and spells;
  - the three new catround cases.
- Look: no.

**73. World map overlay** - C79, C77, C102, C365 (0d)
- Files: WorldMapView.cpp/.h, Game_Wiring.cpp, Game.cpp, WorldSettingsDialog.cpp/.h, GameUI.cpp.
- C79: Editing() = Editor && !overlay.
- C77: no dialog opens from the overlay.
- C102: one note label per tab.
- C365: one BackdropState helper.
- Judge: worldedit.eval. ADD:
  - after `worldedit on`, the M world page in a dungeon shows no toolbar, keeps fog on and does not paint, and a doorway right-click opens nothing;
  - a dialog status verb showing each tab's own note;
  - an InGameTest sweep of Esc and the sheet on the world map.
- Look: yes.

**74. Monster inspector and patrol routes** - C80, C232 (0f), C233 (0f), C104, C99
- Files: Game.cpp, Game_Inspect.cpp, Game_Wiring.cpp, Game_DevWorld.cpp, Game_Editor.cpp, MapView.cpp, DungeonWorld_Editing.cpp, EntityInspector.cpp, MonsterConfigDialog.cpp.
- C80 + C232:
  - a finished route reopens the inspector through OpenInspectorFor(RouteId()), looked up by runtimeId;
  - a respawn ends route-laying;
  - `typeset` closes the inspectors.
- C233: UnloadWorld ends route-laying and clears the inspect config, and the route block runs only in Editor mode.
- C104: the Patrol tab survives a route clear.
- C99: Caster defaults its spell in both copies.
- Judge: ADD an EditorTest case per issue, as named in each entry:
  - C80: inspect A, Edit route, right-click B, Esc, Enter; the inspector reopens on A with A's waypoint count.
  - C232: inspect a skel_warrior, Edit route, `typeset` its kind, finish the route; the inspector reopens on the live monster or declines, with no fault in the log.
  - C233: Edit route, New world, M then Esc; route-laying has ended and the new world's .ent is untouched.
  - C104: clear a route from the Patrol tab; the Patrol tab stays active.
  - C99: set Caster on a type and on an instance, save and reload; the spell is written and no load warning appears.
- Look: no.

**75. Esc in dialogs** - C81
- Files: InstanceInspector.cpp, LevelSettingsDialog.cpp, NewWorldDialog.cpp, GenerateDialog.cpp, AssetDialog.cpp, MapView.cpp, Game.cpp.
- C81: a PopupOpen gate per dialog; AssetDialog closes on Esc; the editor ladder closes the level dropdown first.
- Judge: ADD an EditorTest step: with a dropdown open in an inspector and in Level settings, Esc keeps the dialog and its edits; Esc closes AssetDialog.
- Look: optional.

**76. Inspector text through Loc** - C107
- Files: ProjectileInspector.cpp, Game_Inspect.cpp, MapEditor.cpp, lang x5.
- C107: new keys plus loc::Format.
- Judge: langswitch.eval. ADD:
  - a lang check that every literal key used in src exists in all five .lang files;
  - EditorTest `editor cell` under a non-English language.
- Look: no.

**77. Reset: level transients** - C292, C293
- Files: DungeonWorld_Save.cpp, DungeonWorld_LevelIO.cpp, DungeonWorld_Validate.cpp, DungeonWorld_Combat.cpp, DungeonWorld.h.
- C292 + C293: one ClearLevelTransients (blasts, m_fixtureBreaks, monster effects, pending fall) at every reset site. ResetForEval's resets move into ResetForNewGame, and containers are emptied with clear().
- Judge: Eval.ps1 -SelfTest with batch 12's readout. ADD resettest.eval repros:
  - a poison flask, then the stairs or a load, leaves no blast;
  - save, smash a door, load, and the door is whole.
- Look: no.

**78. Reset: rest, clocks, undo** - C295, C294, C297
- Files: DungeonWorld_Save.cpp, DungeonWorld.h, Game.cpp, DungeonWorld_Validate.cpp, Game_Generate.cpp, DungeonWorld_Undo.cpp.
- C295: rest ends quietly in ResetForNewGame.
- C294: the throw cooldown and the kindle clock are zeroed, through the same quiet SetResting.
- C297: ClearUndoHistory in ResetForNewGame and before an encounter stem changes.
- Judge: rest.eval, strokeundo.eval, and the -SelfTest readout. ADD:
  - `rest on` then `load` leaves resting off and lockstep restored;
  - undo depth is 0 after `reset`, `newgame` and a same-level `load`.
- Look: no.

**79. Reset: Game side** - C296, C115 (0f), C234 (0f)
- Files: Game.cpp, Game_Eval.cpp, Game_World.cpp, Game_Editor.cpp, GameUI.cpp/.h, tools/Eval.ps1.
- C296: one helper clears the cursor from StartNewGame and ResetForEval.
- C115: CancelConfirm in UnloadWorld and at game start.
- C234: SwitchWorld refuses while a bake runs.
- Judge: the -SelfTest readout. ADD:
  - lift an item, go to the title, start a new game, and the cursor is empty;
  - a confirm status verb, and an eval: raise a location prompt, then `reset` or `worlds load` leaves no confirm pending;
  - `worlds load` during a bake refuses, and both worlds' catalogs and imports.cat are untouched.
- Look: no.

**80. Stash rules** - C308, C298, C307
- Files: DungeonWorld_LevelIO.cpp, DungeonWorld.h, Game.cpp, DungeonWorld_Levels.cpp, DungeonWorld_Remote.cpp, DungeonWorld_Census.cpp, DungeonWorld_Validate.cpp, DungeonWorld_Resize.cpp.
- C308: a map-dirty flag.
- C298: a dirty map is always stashed.
- C307: a LevelForReading helper.
- Judge: ADD EditorTest cases:
  - crypt1 -> crypt2 -> crypt1, then savemap leaves crypt2.map unchanged;
  - paint, then load a save on another level, and the stash keeps the paint;
  - read-only queries (a cancelled dungeon delete, `dungeons what`, a level rename, a remote paint) leave untouched levels clean in git.
- Look: no.

**81. Records truth** - C326, C327, C311, C355
- Files: DungeonWorld_LevelIO.cpp, DungeonEntities.cpp/.h, DungeonWorld_Remote.cpp, DungeonWorld_Save.cpp, DungeonWorld_Undo.cpp, DungeonWorld_Editing.cpp, DungeonWorld.cpp, Game_Wiring.cpp, Game_Editor.cpp, DamageLedger.h.
- C326: write spawnX / spawnZ.
- C327: a monotonic m_nextId.
- C311: the two callers sync the records, StashActive, respawn and ApplyActiveSnapshot, and an erase removes its record.
- C355: the ledger rebases at every editor removal.
- Judge: PipelineTest. ADD the four EditorTest cases from the entries:
  - after monsters move, die or wake, savemap leaves spawn lines byte-identical;
  - a browsed level's monster arrives alive;
  - type-save and rename lose no placements, and undo resurrects nothing;
  - `pipelineguard strict` editor erases report no violation.
- This is interim until P9's record-first placement.
- Look: no.

**82. Placement on browsed levels** - C310, C344, C351
- Files: MapEditor.cpp, MapView.cpp, Placement.cpp, DungeonWorld_Remote.cpp, DungeonWorld_Editing.cpp, DungeonWorld_Doors.cpp, DungeonMap.cpp/.h, DungeonWorld.h.
- C310: AddBoreRemote, a bore erase rung, and the dead overloads deleted.
- C344: one rehoming helper for wall decorations.
- C351: Placement returns a niche result, with hover-face tracking.
- Judge: ADD EditorTest:
  - a bore on a browsed level lands in that level's stash, and middle-click erases it;
  - open a bannered wall on a browsed level, savemap, and Check opens without an abort;
  - an item brush on a niche wall writes `niche=`.
- Look: no.

**83. Brush and undo** - C352, C353, C348
- Files: MapEditor.h/.cpp, MapView.cpp, DungeonWorld_Editing.cpp, DungeonWorld_Undo.cpp, DungeonWorld_Remote.cpp, DungeonMap.cpp, DungeonWorld_Resize.cpp.
- C352: the brush is stored by id.
- C353: an erase commits only on a change.
- C348: refuse a paint, or a crop, that removes the start square.
- Judge: palette.eval and strokeundo.eval. ADD:
  - the same type lands after browsing a level with a different palette;
  - redo survives an empty erase;
  - both refusals.
- Look: no.

**84. Encounters and required ids** - C299, C333, C334
- Files: Game_Generate.cpp, Game.cpp, Game_World.cpp, Game_NewWorld.cpp, Game_Editor.cpp, DungeonWorld_Save.cpp, DungeonWorld_Doors.cpp, Project.cpp, Generate.cpp/.h.
- C299: refuse a save only when InEncounter() && !onWorldMap, and write a real stem.
- C333: Project::ExitStairType is found by its flag, one open-side helper places it, and deleting the last exit stair type is refused.
- C334: the lock door type is found by flag.
- Judge: WorldTest; LevelBuildTest (shipped output unchanged). ADD:
  - worldencounter.eval: encounter, leave, save, load;
  - rename the exit stair type, and `encounter` places it;
  - rename or delete wooden_door, then generate with locks:1.
- Look: no.

**85. Generator and level ids** - C136, C331, C332
- Files: Game_Generate.cpp, Game_World.cpp, Game_Wiring.cpp, Catalog.h, Project.cpp, DungeonWorld_Move.cpp, DungeonWorld_Undo.cpp, DungeonWorld_LevelIO.cpp.
- C331: a case-keeping id splitter.
- C136: one pure arrival rule for ArrivalsOn and RemapArrivals.
- C332: a shared stair-line formatter; regenerating carries the stair `flag=`, the atmosphere and the uistone.
- Judge: WorldTest; EditorTest phase 18; LevelBuildTest levelrecipe / levelplay. ADD:
  - a level renamed to Keep1 stays consistent;
  - regenerating a capitalised-stem level behind a doorway (and with an empty startLevel) arrives on a walkable square;
  - the three records survive regeneration.
- Look: no.

**86. Model files resolve** - C301
- Files: AssetUtil.cpp, DungeonWorld_Load.cpp, AssetDialog.cpp, TypeEditorDialog.cpp, Game_Editor.cpp.
- C301:
  - batch 13's resolver accepts whichever of .gltf / .glb exists, everywhere it is used;
  - the create dialog and Save refuse a model the category cannot load;
  - ItemKindFor binds the entry's texture.
- Judge: the InGameTest installed-models pass. ADD: an EditorTest weapon from a .gltf and a decoration from a .glb, then a reload, and levelcheck resolves both. Update batch 13's extension-mismatch mutation to the new rule.
- Look: yes, an imported weapon is no longer white.

**87. Kind caches** - C302, C330, C347
- Files: DungeonWorld_Load.cpp, DungeonWorld_Editing.cpp, DungeonWorld.cpp, Game_Editor.cpp, Game.cpp, GameUI.cpp, GameUI_Items.cpp, Spells.cpp, Character.cpp, decorations.cat (project + template).
- C302:
  - caches are keyed by (catalog, id);
  - the decoration is renamed portcullis_grate;
  - create, rename and duplicate refuse an id used in a related catalog;
  - an ItemKind is rebuilt in place.
- C330: wearByType is cleared, and glow_radial is loaded once and shared.
- C347: ItemKind is the one definition of "rune".
- Judge: SpellTest MEMORIZE. ADD:
  - a weapon type-save is visible through `itemdetails` without a reload;
  - the decoration and door portcullis sit side by side;
  - WorldTest: a switch to a world where an item lost `wear` makes the doll refuse it;
  - a rune under a non-`rune_` id memorizes and draws its glyph.
- Look: no.

**88. Import and bake** - C346, C336, C393
- Files: Game_Wiring.cpp, Game.cpp, Game_DevWorld.cpp, Game_Editor.cpp, DungeonWorld_Editing.cpp, AssetDialog.cpp, tools/ReplayImports.ps1, src/Assets/PbrMaps.cpp, tools/AssetBaker/ImportTextures.cpp.
- C346: the texture is written only after its bake succeeds.
- C336: an exact worn_<base>_<tier> match.
- C393: a tri-state flip-green flag, and the GL token only at the end of a name.
- Judge: ADD:
  - EditorTest: `typeset walls <id> texture <unbaked set>` bakes or refuses, and a failure leaves the catalog unchanged;
  - a pure match check (pot vs pottery);
  - an import with `--no-flip-green` and a mid-word "gl" normal map.
- Look: no.

**89. World map data** - C345, C343
- Files: WorldMap.cpp/.h, Game_World.cpp, Game_Editor.cpp, Game_DevWorld.cpp, DungeonWorld_Editing.cpp.
- C345: save-time glyph validation, unused glyphs for new terrains, a refusal to delete a used terrain, and a CheckWorld rule.
- C343: one validated both-or-none entry mutator.
- Judge: WorldTest (worldtypes.eval, worldedit.eval). ADD:
  - two "+ New" terrains, a bad-glyph save and a used-terrain delete are refused, and the world reopens;
  - `worldloc set <id> entryx 5` and a negative Z are refused.
- Look: no.

**0a additions (run with batch 5)**

**5a. RollTest knob defaults and the verdict line** - C423, C425
- Files: tools/RollTest/Main.cpp, src/Game/Balance.h, src/Game/Defense.h, tools/Bc7Test/Main.cpp, tools/DiagTest/Main.cpp, tools/ThreadStress/Main.cpp.
- C423: fix or delete the base-25 strike row (real defense is 45), use `defense::StanceRules{}` where RollTest retypes it, and move the knob defaults into a pure constexpr header RollTest includes instead of retyping them.
- C425: RollTest's (and Bc7Test's) last line follows the `<TOOL> RESULT=X checks=N failures=M` convention the other tools print.
- Judge: RollTest PASS and --self-test still FAILs; CheckAll parses the new last lines. ADD: changing a constexpr default moves the RollTest expectation.
- Look: no.

**0h additions - the asset tools (after batch 89)**

**90. Worn-block bake: one authority** - C406, C409, C408, C438
- Files: tools/AssetBaker/ModelBaker.cpp, src/Game/CatalogSchema.cpp, src/Game/Game_Wiring.cpp, tools/ReplayImports.ps1, CLAUDE.md.
- C406: one per-texture-set record (kind, relief, seed) read by both bake paths and by the type editor, so `AssetBaker models`, `wornblock` and an editor restyle agree.
- C409: `wear = 0` emits the flat quad for walls, floors AND ceilings, and relief x wear scales every wear term (procedural noise and the wall bow included).
- C408: take the texture's aspect from the first sized image (or the albedo, as the game does), so a resolution fallback cannot drop it.
- C438: ReplayImports uses the shared baker wrapper and passes relief and wear.
- Judge: ADD - for every specs[] set, `AssetBaker wornblock` with no catalog relief/wear writes worn_<set>_*.gltf byte-identical to `AssetBaker models`; `wornblock --wear 0` gives the flat quad for all three kinds; square sets re-bake byte-identical. Fix CLAUDE.md's "untouched types bake as before".
- Look: yes, if any shipped set's relief changes (it should not).

**91. "Use installed" never re-bakes a shared set** - C407
- Files: src/Game/Game_Wiring.cpp, src/Game/Game_Editor.cpp, src/Game/AssetUtil.cpp.
- C407: skip the bake when worn_<set>_med.gltf exists, and refuse when another type binds the set as a different surface kind.
- Judge: ADD an EditorTest phase - "Use installed" with a set already bound as a wall, creating a floor type, is refused and the worn mesh's hash is unchanged; a same-kind create runs no bake.
- Look: no.

**92. Baked files are current** - C410, C437, C414
- Files: tools/AssetBaker/Main.cpp, MipBaker.cpp, src/Game/AssetUtil.cpp, src/Assets/Model.cpp, src/Assets/Image.cpp, tools/FetchModels.ps1, tools/FetchAnimLibrary.ps1, CLAUDE.md.
- C410: one `assets::BakedIsCurrent`, used by the texture loader too: a .dds older than its PNG warns and the PNG is decoded; `AssetBaker runes` bakes its own mips.
- C437: FetchModels and FetchAnimLibrary end with `AssetBaker model-images`; a missing sidecar logs once per model; add FetchAnimLibrary to CLAUDE.md's regenerable list.
- C414: mip chains are averaged in linear light for sRGB albedo and rounded, not truncated; then re-bake the mips. Needs batch 7 (a working Bc7Test baseline) first.
- Judge: check-bc7 with the working baseline. ADD - touch a rune PNG newer than its .dds and the log warns; a Downsample unit check that an sRGB 2x2 average keeps the linear mean.
- Look: yes, after the mip re-bake (distant textures may brighten slightly).

**93. The baker's writes fail loudly** - C416
- Files: tools/AssetBaker/GltfWriter.cpp, SoundBaker.cpp, ImportTextures.cpp, src/Assets/File.cpp.
- C416: write through `assets::WriteBinaryFile`, escape JSON names, check the WAV frame count, and log a found map's load error.
- Judge: ADD - `AssetBaker models` with a read-only target exits non-zero with a log line; a name containing a quote round-trips as valid JSON.
- Look: no.

**94. Fetch and template scripts** - C402, C439, C434
- Files: tools/FetchTextures.ps1, assets/maps/ (deleted), README.md, docs/editor-type-authoring.md, src/Game/DungeonWorld_Load.cpp, tools/BuildTemplate.py, assets/templates/default/project.ini, tools/ConvertMesh.py, tools/FetchModels.ps1, CLAUDE.md.
- C402: FetchTextures builds its wanted list from the catalogs' `texture` fields (project, library, template) and throws on a wanted name with no archive folder; delete the dead assets/maps/ and every doc line that describes it as live.
- C439: BuildTemplate reads start_items and the default fixture ids from the source project.ini (fire_flask is missing today).
- C434: ConvertMesh's `is_skeletal` uses `all_fcurves(action)`; FetchModels runs Blender with `--python-exit-code` so a traceback fails the import.
- Judge: ADD - in a fresh worktree FetchTextures imports 12/12 crypt sets with no magenta; re-running BuildTemplate gives start_items equal to dungeon-demo's; ConvertMesh --keep-rig on a two-action fixture produces a rigged glb.
- Look: no.

**95. Blender script geometry** - C435, C436, C404
- Files: tools/BuildWallArch.py, BuildDoorFrame.py, BuildFountain.py, BuildPotion.py, BuildRock.py, BuildStatue.py, the re-baked models.
- C435: BuildWallArch builds its slab from a solidity grid like BuildDoorFrame, with the shared closed-shell assert (0 open edges, 300 today), then re-bake both arches.
- C436: the fountain basin and spout, the potion corks and the rock take u from the segment index (the BuildPillar rule), so no face column spans the circumference backwards.
- C404: BuildStatue's box() gets the sorted-bounds winding fix its siblings have.
- Judge: ADD - the per-face outward-normal check (CLAUDE.md's winding test) and a per-face u-span check on the re-run assets.
- Look: yes, the arches, fountain, corks and rock.

**96. Dev commands that write around the UI** - C448, C449, C450
- Files: src/Game/Game_DevParty.cpp, GameUI.cpp, GameSettings.h, Game_DevCommands.cpp, MapEditor.cpp, DungeonMap.cpp, Game_DevDiagnostics.cpp, Game.cpp.
- C448: `hudpanel` goes through GameUI so the Settings sliders and the tray follow, and its panel list comes from kHudPanelFields.
- C449: `editor place` picks its default wall from the VIEWED map with FreeSconceWall / FreeNicheWall.
- C450: `allocguard status` reports the last quiet streak from before the console opened, or the dead line goes.
- Judge: AllocTest -Panels. ADD - `hudpanel` then the Settings slider reads the new scale; an EditorTest case for `editor place` on a browsed level whose first face is taken.
- Look: no.

**Moved to phase 4:** C46 (a Huge monster's 2x2 footprint is known only to monster occupancy). It is latent - no Huge monster is authored - and the footprint is the "body" column of P4's passage grid.

#### Moved out of phase 0
- **C46 -> P4.** It is latent: no Huge monster is authored. The footprint test is the "body" column of P4's Passage grid, and the findings already close it there.
- **C125's file split -> P1.** Moving UpdateStates, Render and the loading tasks into their own files belongs to the GameApp target split. TickWorld itself stays in batch 53.
- **C454's ThreadStress half.** It duplicates C418's "monsters in occ" item and lands in batch 6. C454 keeps only the docs.
- **C213 is split.** Only the tier additions land (batch 14). P2 keeps the extended tier, the Chase / Consume / Map / -Cold modes, the self-reporting reserves, worker judging and the M map. Six of P2's AllocTest modes are built in phase 0 beside the fixes they judge: -Rest, -Swing, -Exit, -Lever, -Items -LongId and -Effects.
- **C216's real post-resume coverage -> P2's -Cold mode.** Only the wording is fixed now.
- **C301's retirement of `multimaterial` by file contents -> 8c.**
- **C311's record-first placement -> P9** (Michael chose records). Batch 81 is the interim sync.
- **C136's EditHistory -> P10.**
- **Dead UI code -> P7 (C92).** That covers C455's dead helpers (DialogTitleFont, the panel-rect AddCloseButton, CloseButtonRect) and C456's TextOutput.
- **C451's StatBonus/SpeedScale hook decision -> 5g.**
- **Held for Michael's answer:**
  - C34: changing rest's "attacked". If he says no, land only the provoke half.
  - C227: recycling pebbles. If he says no, it moves to P2.
  - C1: does a Wind Ward stop a burst?
  - C211: the once-per-niche excuse fallback.
  - C40: does a wall shatter train throwing?
- **C64:** findings put it in P6 step 6. It is standalone, but the per-frame cap changes how fast rest runs, so Michael should feel it.

#### Must land together
**One commit:**
- **C78 + C125.** TickWorld is C78's one world-frozen decision.
- **C77 + C79.** Editing() removes C77's routes.
- **C80 + C232 + C233.** They share one route-reopen path and route state.
- **C35 + C49 + C71.** C49 (reserve) and C71 (fixed array) prescribe different containers for m_activeBlasts. Take the fixed array with C49's never-evict rule.
- **C66 + C70.** Same pools.
- **C58 + C57 + C74.** One side list and one stand predicate.
- **C292 + C293.**
- **C295 + C294.** One quiet SetResting.
- **C308 + C298.**
- **C357 + C65 + C356.**
- **C178 + C187.**
- **C189 + C183.**
- **C219 + C228.**
- **C417 + C422.**
- **C333 + C334.**
- **C136 + C331.** C331's lowercasing ParseTags is half of ArrivalsOn's bug.
- **C311 + C326 + C327 + C355.**
- **C220 + C371.** kValueCap against loc::Line::kCapacity.
- **C180 + C359.** Both use FloorItemWorld.
- **C456 + C457.** One CLAUDE.md pass.
- **C64 + C48.** Fixed rest ticks and projectile sub-steps; one 60x judge sees both.

**In this order (conflicts):**
- **C441 (batch 13) before C301 (batch 86).** C441's ".glb where the category loads .gltf must fail" mutation stops being true once C301 accepts either extension. C301 must update that case.
- **C466 (batch 20) before C398 (batch 68).** C466 drops RestartApp's extraArgs and the `-newgame` comment, so C398 must forward the original command line and not revive extraArgs.
- **C384 (batch 47) before C201 (batch 70).** Both add to the exe manifest.
- **C300 (batch 12) before C292, C296 and C297.** It is their judge. Both C300 and C297 clear undo, so keep one call site.
- **C35/C49/C71 (batch 21) before C292.** ClearLevelTransients empties the final blast container.
- **C451 (batch 19) before C219/C228.** Warm(fx::kMaxEffects) must see the raised ceiling.
- **C10 -> C218 -> C212.** All three touch the fumble's held id.
- **C89 before C221 and C229.** C89 deletes Font::SetHeight and the owned-font path.
- **C208, C224, C382 and C204 before C127.** C127 splits Controls.cpp.
- **C52 (batch 34) before batches 77-79.** All edit ResetForNewGame.
- **C11 (batch 22) before C1 (batch 24).** Both edit ResolveMonsterProjectileHit, and the deflect-return position decides the Wind Ward question.
- **Inside batches:** C43 -> C44 -> C47; C1 -> C18; C62 and C63 -> C64; C383 -> C384; C385 -> C388; C386 -> C387; C198 -> C199; C442 and C443 -> C444; C442 -> C446.
- **C401 first** for every harness batch (3, 4, 8, 14). **C214 first** for every 0g batch. **C418 before C63 and C386**, whose new ThreadStress cases need honest paths.

## Phase 1 - split Game into six CMake targets

Michael chose all six targets now (answer 5). Closes C118, C120, C121, C122, C126, C128, C129, C130, C132, C134, C148, C458, C476. This section is the include-graph analysis it rests on: the targets, every file's place, the six wrong-way edges and their cuts, and the commit sequence.

The whole Game library has only six include edges that point the wrong way. Six small cut commits fix them, then six build commits create the targets. The first build commit adds a layer check that runs as part of the build.

Method: I collected every quoted `#include` in `src/Game` (1,215 edges, 665 of them `Game/` to `Game/`) and checked them against the assignment below. I also checked every free-function declaration and definition across files, and every `Class::` definition site. No call crosses a layer without an include, so the include edges are the complete list.

### 1. Targets

| Target | Depends on (PUBLIC) | Files | Lines | Scope |
|---|---|---|---|---|
| GameRules | Core, Assets | 157 | ~21.5k | Pure rules: combat, defense, effects, spells, character, party, map, entities, world map, generator, carve, styles, validation, AI brain, projectiles, saves, catalogs/project, sound bank data, asset-pool loaders |
| GameWorld | GameRules, Animation, Graphics, Audio (Platform comes through Graphics) | 35 | ~22.6k | DungeonWorld (whole) with its simulation and rendering, mesh builder, fires, shadows, liquid |
| GameHud | GameRules, UI, Audio | 53 | ~15.4k | GameUI, the sheet, party panels, pickers, party creation, GameSettings, dialog chrome, shared icons |
| GameEditor | GameWorld, GameHud | 61 | 16.8k | MapView, MapEditor, WorldMapView, AssetPicker, every editor dialog and inspector, CatalogSchema |
| GameApp | GameEditor, DevTools | 19 | 16.0k | The Game class: state machine, wiring, dev commands, eval runner |
| DevTools (moves to `src/DevTools/`) | Core, Platform, Graphics, UI | 9 | 4.5k | The console shell plus the perf, profile, health and threads panels |

**Why each target needs what it links:**
- **GameRules needs Assets.** Catalog, Project, DungeonMap, DungeonEntities, SaveGame and WorldMap read files through `Assets/File.h`. SoundBank holds `assets::SoundData`. The asset-pool loaders call `assets::LoadModel` and `LoadWavFile`. After cuts E1-E3 and E6, nothing in it needs Platform, Graphics, UI, Audio or Animation.
- **GameHud needs Audio.** `GameUI.h:17` and `:726` hold `audio::AudioEngine&` for the UI sounds.
- **GameEditor depends on GameHud.** This is my decision; here is the case:
  - 20 editor files include `DialogLayout.h`, and `AssetPicker.h` includes `ThumbCache.h`. Both are shared with ItemDetailsDialog and PortraitPicker.
  - `MapView.h`, `MapEditor.cpp`, `MapEditor_Categories.cpp` and `WorldMapView.h` need GameSettings, because the dock, palette and grouping state is saved in settings.ini.
  - The alternative is to move DialogLayout and ThumbCache into UI (C134, a namespace change) and split an EditorSettings out of GameSettings. That is later work.
  - The edge goes one way only: no HUD file includes an editor file.
- **Main** links GameApp.
- **Nothing in DevTools changes behaviour.** DevConsole includes no Game header. Its only game knowledge is the CmdGroup enum (`DevConsole.h:53`) and the group titles (`DevConsole_Commands.cpp:32`). They stay for P1. P10 (C130/C143) turns them into groups that GameApp registers, plus a `needsWorld` flag.
- **Dev commands stay in GameApp.** They are `Game::Register*Commands` members, and `DevCommandArgs.h` includes `Spells.h`. The dependency correctly runs GameApp -> DevTools.
- **The Game class files** (`Game*.cpp`, `Game_Dev*.cpp`) need every Game layer plus DevTools and direct engine headers: Platform/Process, Window, PerfMonitor; Graphics/PostProcess, ModelPreview, DisplayEnum; UI/FontLibrary, TreeInspector; Audio. That is exactly GameApp's position.

**Where I placed files differently from the findings' sketch, because the include graph forces it:**
- **SoundBank goes to GameRules, not GameWorld.** `GameUI.h:28` and `:727` hold `const SoundBank&`. It is a plain struct of `assets::SoundData`.
- **LoadQueue.h and ItemDetails.h go to GameRules.** GameUI.h and DungeonWorld.h both use LoadQueue.h. DungeonWorld and ItemDetailsDialog both use ItemDetails.h. Moving LoadQueue to Core (C134) can come later.
- **GameSettings, DialogLayout and ThumbCache go to GameHud.**
- **CatalogSchema stays in GameEditor, as specified.** It has no Game includes, so moving it to GameRules in P9 is a one-line CMake change.
- **NewWorld.h goes to GameEditor**, the lowest layer that uses it.
- **Character.h:33/:420** forward-declares `gfx::Texture` for one pointer. That is a name only, with no include or link edge, so it is tolerated.

### 2. File-to-target list

Stems without an extension mean both .h and .cpp.

**GameRules (157):** Area, AssetUtil (pool part, see E6), Balance, Blast, Carve, Catalog, Character, Combat, Curve, DamageLedger, DamageTypes.cpp, Defense, DungeonEntities, DungeonMap, Entity, Generate, GenerateKnobs, Inventory.h, ItemDetails.h, LightProfile, LoadQueue.h, Magic, Mishap, MonsterAI, Party, PartyRules.h, Placement, Power, Project, Projectiles, RenderQuality.h (new, E5), Resource, Roll, SaveGame, Serialize, SlotGrid.h, SoundBank, SpellIdList.h, Spells, Style, StyleLibrary, StyleLook, Threat, Trail, UseDefaults.h, Validate, Validate_Flags.cpp, Validate_Styles.cpp, WorldMap.
- Effect/: AllEffects.cpp, DotEffect, Effect, LightEffect, SightEffect, SmokeEffect, SupplyEffect, WardEffect.
- Spell/: AllSpells.cpp, Airbolt, BoltSpell, Earthbolt, Embersight, Farsight, Firebolt, Firelight, Fireshield, Flame, Gust, HandSpell, LightSpell, ModifiedSpell, Rock, Scrying, SightSpell, Skylight, Spell, Splash, Stonelight, Stonesight, Stoneskin, Tidelight, WardSpell, Waterbolt, Waterveil, Windward.

**GameWorld (35):** DungeonWorld, DungeonWorld_{Ahead, Arena, Census, Combat, Doors, Editing, Fires, Flight, Leader, Ledger, LevelIO, Levels, Light, LightBudget, Load, Models, Move, Remote, Render, Resize, Save, SpellLight, Throw, Undo, Validate}.cpp, DungeonMeshBuilder, FireEffect, Liquid, ShadowScheduler.

**GameHud (53):** GameUI, GameUI_{Items, Party, Stone}.cpp, GameSettings, SharedIcons (new, E6), DialogLayout, ThumbCache.h, MessageLog, MenuPanel, HudTray, StonePicker, PartyHud.h, PartyHudTypes.h, PartyHudDraw, PartyBar, CharacterPanel, ControlBar, MemberCards, HandSlot, GuardSlider, SpellbookPanel, PartyWindow, CharacterSheet, CharacterSheetLayout.h, CharacterSheet_{Inventory, Lists, Stats, Status}.cpp, ItemDetailsDialog, PortraitPicker, PartyCreationPage.

**GameEditor (61):** MapView, MapView_{Docks, Issues, Tools}.cpp, MapEditor, MapEditor_{Categories, Quests, Shapes, Styles}.cpp, MapColors.h, WorldMapView, AssetDialog, AssetPicker, BalanceDialog, CurvePlot, CatalogSchema, GenerateDialog, InspectPicker, LevelSettingsDialog, MonsterConfigDialog, NewWorld.h, NewWorldDialog, TypeEditorDialog, ValidateDialog, WorldSettingsDialog, WorldsDialog, InstanceInspector, ButtonInspector, DoorInspector, EntityInspector, FixtureInspector, NicheInspector, PropInspector, StairInspector, ProjectileInspector.

**GameApp (19):** Game, Game_{DevCommands, DevDiagnostics, DevDungeons, DevEval, DevParty, DevWorld, Editor, Eval, Generate, Inspect, NewWorld, Party, Populate, Styles, Wiring, World}.cpp, DevCommandArgs.h.

**DevTools (9, `src/DevTools/`):** DevConsole, DevConsole_Panel.h, DevConsole_{Commands, Health, Perf, Profile, Snapshots, Threads}.cpp.

**Engine addition:** Graphics/TextureFile.{h,cpp} (E6).

### 3. Every wrong-way edge and its cut, in commit order

These six are the complete set. With the cuts in place, the checker reports zero violations.

| # | Edge | Cut |
|---|---|---|
| E1 | `Character.cpp:5` -> `Game/GameSettings.h` (Rules -> Hud), only for `kDefaultMemberColors` at `:131` | Move `kMemberColorCount` and `kDefaultMemberColors` (`GameSettings.h:79-85`, comment included) into `Character.h`. GameSettings.h includes Character.h explicitly. Delete the include. |
| E2 | `Party.h:19` -> `Platform/Input.h` (Rules -> Platform), for `Party::HandleInput` (`Party.h:77`, `Party.cpp:183-211`). Its only caller is `DungeonWorld.cpp:431`. | Move the key loop, unchanged, into a private `DungeonWorld::DriveParty(const Input&)` in `DungeonWorld_Move.cpp`. Party gains `const MoveKeys& Keys() const`, and declares `bool IsTurnAction(MoveAction)` (currently at `Party.cpp:42`) in `Party.h`. `DungeonWorld.h` includes `Platform/Input.h` itself. MoveKeys and LookSettings stay as plain data in Party.h. |
| E3 | `Projectiles.h:35` -> `Graphics/ParticleBatch.h` (Rules -> Graphics, which pulls D3D12 into every Spell TU), for `AppendBillboards` (`h:249`, `cpp:313`). Its only caller is `DungeonWorld.cpp:545`. | Replace it with `template <class Emit> void ForEachBillboard(Emit&& emit) const` in Projectiles.h, body moved verbatim with the same order and maths. DungeonWorld.cpp passes a capturing lambda that does `m_particleScratch.push_back({p, s, c})`. There is no std::function, so nothing allocates. |
| E4 | `DungeonWorld_Render.cpp:9` -> `Game/PartyHudDraw.h` (World -> Hud), for `RuneGlowColor` (`:1034`) and also `kRuneGrooveMean` (`:1036`), which C129 missed | Move `RuneGlowColor` (`PartyHudDraw.cpp:240`) and `kRuneGrooveMean/Swing/BreathSeconds` (`PartyHudDraw.h:113-115`) into `Spells.h`/`.cpp`, beside `ElementColor`. The HUD still reaches them through Character.h -> Spells.h. |
| E5 | `DungeonWorld.h:32` -> `Game/GameSettings.h` (World -> Hud). The member is at `:3965`. The world uses `MeshSuffix` (Editing:189, Load:236/254/273/2297), `TextureSuffix` (Editing:193, Load:176/371/413/2298), `QualityLabel` (Load:2297) and `maxPointLights` (Light:176, LightBudget:126). | New `Game/RenderQuality.h` in GameRules, holding `enum class Quality`, `kLightBudgets`, and `struct RenderQuality { Quality quality; int maxPointLights; MeshSuffix(); TextureSuffix(); QualityLabel(); }`. Then `struct GameSettings : RenderQuality`, so every `settings.quality` and `settings.maxPointLights` site is unchanged. The DungeonWorld constructor and member become `const RenderQuality&` (`h:83`, `:3965`; `cpp:74,77`). Game passes `m_settings` as today, and the binding stays live, so quality hot-swap still works. The `static_assert` against `gfx::kMaxPointLights` stays in GameSettings.cpp. |
| E6 | `AssetUtil` has 31 includers spread over all five Game layers. `AssetUtil.h` needs `Graphics/Texture.h`, its .cpp needs `UI/ControlIcons.h`, and `SoundBank.cpp:6` (Rules) needs `LoadSound`. Wherever it sits, one sibling points the wrong way. | Three-way split, described below. |

**E6 in detail:**
- **Graphics/TextureFile.{h,cpp}** (`gfx::`) gets `TryLoadTextureFile`, `LoadTextureThumb` and `LoadTextureFile`, plus the magenta placeholder (`AssetUtil.cpp:44-83`, `155-185`).
  - Add `#include "Graphics/TextureFile.h"` and qualify about 52 calls with `gfx::` in: AssetDialog, AssetPicker, DungeonWorld_Editing, DungeonWorld_Load, DungeonWorld_Models, Game, GameUI, GameUI_Stone, MapView, PartyCreationPage, PortraitPicker (.cpp).
- **Game/SharedIcons.{h,cpp}** goes to GameHud with names unchanged: `CloseIcon`, `ToolbarIcon`, `LoadSharedControlIcons`, `ReleaseSharedIcons` (`AssetUtil.cpp:85-153`).
  - Include it in: AssetDialog, AssetPicker, BalanceDialog, DialogLayout, Game, GameUI, GenerateDialog, InspectPicker, InstanceInspector, ItemDetailsDialog, LevelSettingsDialog, MapView, MonsterConfigDialog, NewWorldDialog, PortraitPicker, ProjectileInspector, TypeEditorDialog, ValidateDialog, WorldMapView, WorldSettingsDialog, WorldsDialog (.cpp).
- **AssetUtil keeps** `LoadModelOrDie`, `LoadModelIfPresent`, `LoadSound`, `Installed*`, `AssetInfo` and `kRes*`. It then uses only Core and Assets, so it goes to GameRules. Call sites are unchanged.
  - Files that keep the include: AssetDialog, AssetPicker (.h and .cpp), DungeonWorld_Editing, DungeonWorld_Load, DungeonWorld_Models, GameUI_Stone, Game_DevCommands, Game_DevWorld, Game_Wiring, LevelSettingsDialog, SoundBank.
  - Files that drop it: BalanceDialog, DialogLayout, Game, GameUI, GenerateDialog, InspectPicker, InstanceInspector, ItemDetailsDialog, MapView, MonsterConfigDialog, NewWorldDialog, PartyCreationPage, PortraitPicker, ProjectileInspector, TypeEditorDialog, ValidateDialog, WorldMapView, WorldSettingsDialog, WorldsDialog.
  - Fix the comments that name "AssetUtil's ToolbarIcon": `MapView.h:460`, `WorldMapView.h:163`, `DialogLayout.h:119`.

### 4. Commit sequence

Each commit builds in debug and release and keeps RollTest passing.

| # | Commit | Size |
|---|---|---|
| 1 | E1: member colours move to Character.h | S: ~20 lines, 2 files |
| 2 | E2: Party stops reading the keyboard | S: ~70 lines, 5 files |
| 3 | E3: ForEachBillboard | S: ~50 lines, 3 files |
| 4 | E4: rune glow colour and groove constants move to Spells | S: ~30 lines, 5 files |
| 5 | E5: RenderQuality | S: ~90 lines, 6 files |
| 6 | E6: AssetUtil split. Add TextureFile.cpp to Graphics' CMake list and SharedIcons.cpp to Game's. Update CLAUDE.md's close-icon and TryLoadTextureFile notes. | M: ~600 lines moved, ~90 edited, ~36 files |
| 7 | GameRules target. See the build details below this table. | M: ~6 files, ~550 lines, mostly lists |
| 8 | DevTools moves below Game. See the build details below. | M: 4,465 lines moved, ~60 edited, ~14 files |
| 9 | GameWorld target: 35 files out of the `Game` list. Game links GameWorld. Add GameWorld's row to the layer check. | S |
| 10 | GameHud target: 53 files. Game links GameWorld and GameHud. | S |
| 11 | GameEditor target: 61 files. Game links GameEditor and DevTools. | S |
| 12 | Rename the remaining `Game` target to GameApp, and Main links GameApp. Docs: the ARCHITECTURE.md diagram and module table, and CLAUDE.md's "Nine strictly layered static libs..." line and Game-split paragraph. | S-M: ~150 lines |

**Commit 7 in detail:**
- Change the root `dn_ide_layout(target dir [HEADERS ...])` so it takes an explicit header list. Without that, every target in `src/Game` globs all 140 headers.
- Add a `dn_game_lib(<t> DEPS ... SOURCES ... HEADERS ...)` helper in `src/Game/CMakeLists.txt`. Every .h and .cpp is listed once. The helper writes `game_layers.txt` through `file(CONFIGURE ...)`, which only rewrites when the content changes.
- Create the GameRules target. `Game` keeps the rest and links GameRules.
- Add `tools/CheckLayers.cmake`, run by a `LayerCheck` custom target. GameRules depends on it, and its inputs come from a CONFIGURE_DEPENDS glob of `src/`.
  - It fails with `file:line` on any Game include outside the allowed set, on any Game file not in the table, and on `Game/` or `DevTools/` includes from engine directories.
  - `-DSELFTEST=1` injects `DungeonWorld_Render.cpp -> Game/PartyHudDraw.h` and must fail.
- RollTest becomes `add_executable(RollTest Main.cpp ${CMAKE_SOURCE_DIR}/src/Graphics/LightTiles.cpp)` plus `target_link_libraries(RollTest PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,GameRules>")`.
  - WHOLE_ARCHIVE links every GameRules object, so a rules TU that calls Graphics, UI, Platform or a higher layer fails this link.
  - Rewrite its comment block. It no longer "links Core only".
- ThreadStress becomes `Main.cpp` plus `target_link_libraries(ThreadStress PRIVATE GameRules)`.

**Commit 8 in detail:**
- `git mv` the 9 files to `src/DevTools/`, with a new CMakeLists that links PUBLIC Core, Platform, Graphics and UI.
- Root CMakeLists: `add_subdirectory(src/DevTools)` after Audio.
- Rename the namespace to `dungeon::devtools`.
- Add `using devtools::DevConsole; using devtools::CmdInfo; using devtools::CmdGroup;` in Game.h and DevCommandArgs.h, so the 155 registrations are untouched.
- Rewrite 19 `Game/DevConsole*` include lines across 11 files.

### 5. Risks and what the checks must show

**Risks:**
1. **The include wall is only the lint.** Core puts `src/` on every target's include path, so a CMake dependency stops no include. I expect MSVC's linker to re-scan static libraries (I haven't confirmed it), in which case a wrong-way call still links into Dungeon.exe. Only RollTest's WHOLE_ARCHIVE catches calls out of GameRules. GameWorld, GameHud and GameEditor rely on the lint alone.
2. **Transitive-include fallout.** Removing GameSettings.h from DungeonWorld.h also drops UI/Controls, UIContext, PartyHudTypes and DisplayEnum. Removing Input.h from Party.h, and windows.h/d3d12 from Projectiles.h (and so from Spell TUs), drops more.
   - Fix each error by adding the exact header to the failing TU. Never re-add the header that was cut.
   - Watch for an LNK2019 naming an `...W` symbol. That is the Win32 A/W macro trap. I scanned the headers and found no colliding identifiers.
3. **Two commits relocate logic:** 2 (key mapping) and 3 (billboard emission). Both must be verbatim in order and maths.
4. **Unmerged branches will conflict.** The `sound` worktree likely touches SoundBank, LoadSound and the CMake list. Any open branch that adds a Game .cpp will conflict in `src/Game/CMakeLists.txt`; the resolution is to add the file to the right list, and the lint names which. Survey and merge before starting, per the warn-about-unmerged-branches note in memory.
5. **No speed gain from the split.** It does not reduce the 54-TU recompile on a DungeonWorld.h edit (C119/C120).

**What the checks must show afterwards:**
- **`/check-build`:** both configurations clean with no new warnings, and the "Checking the Game layer walls" step passes.
  - Two mutations must each fail, then be reverted: a `Game/PartyHudDraw.h` include in DungeonWorld_Render.cpp must fail at LayerCheck, and a `gfx::` call in a GameRules TU must fail RollTest's link.
  - `dumpbin /dependents RollTest.exe ThreadStress.exe` must list no d3d12, dxgi or XAudio2.
- **RollTest:** PASS, with output identical line for line to a capture taken before the split.
- **ThreadStress / `/check-threads`:** same verdict, no force-terminate, clean supervised reboots.
- **Eval:** `Eval.ps1 -SelfTest` PASS with output unchanged. The ten suites' output must match a baseline taken before the split, run both windowed and headless.
- **Also run:** `geomhash` unchanged; AllocTest default plus `-Walk` (commit 2), `-Cast` and `-Impact` (commit 3), `-Sheet` (commit 4) and `-Panels` (commit 6); `/check-ingame`'s `uioverlap` sweep (icons moved); HealthTest and TypingTest (the console moved).

## Phase 2 - shared helpers and guard coverage

Goal: give the small rules one home, and let the allocation guard see what
phase 0 showed it could not. Low risk; most of it is new code beside old.

- **Text and parsing.** Core/StringUtil grows into the text module: Trim,
  Split, Join, ToLower / ContainsNoCase, strict ParseInt / ParseFloat /
  ParseBool / ParseColor, IsIdChar / FilterId, UTF-8 helpers - all on
  string_view so guarded frames stay allocation-free (C238, C239, C240,
  C249). `devargs::Member / Int / OnOff / Dir`, each refusing a bad token the
  way `Need` does (C241, C273). One FNV-1a (C244).
- **Grid.** One Direction-ordered step table and `DungeonMap::FirstSolidWall`
  (C242, C243, C251). The AI BFS and the combat best-slot search keep their
  neighbour order, or the Eval suites are re-run when they move.
- **Workers are judged** (answer 2). ThreadManager brackets each worker tick
  after its warm-up; a tick that allocates is a violation, logged with its
  stack like a main-thread frame, and AllocTest's verdict counts worker
  violations (C390, C230). The supervisor thread registers too.
- **The player's M map is guarded** (answer 7). Its per-frame strings and
  marker copies become fixed buffers, and SteadyStateFrame arms while the map
  is open in Player mode (C209). The editor stays exempt.
- **AllocTest grows the rest of its modes:** Chase (monsters walking a path
  to the party), Consume, Map (fight under the M map) and `-Cold` (arm from
  the first Playing frame after a load, so first-instance allocations are
  seen), and the full tier runs every mode (C213's remainder). Phase 0 has
  already built -Rest, -Swing, -Exit, -Lever, -Items -LongId and -Effects
  beside the fixes they judge.
- **Reserves report their own growth.** A container that grows past the
  capacity it promised logs its site in an armed frame, warm-up or not.
- **Tool plumbing:** a shared tools/Pipeline.ps1 + pipeline_common.py for the
  archive root and Blender discovery (C405); AssetBaker's image helpers in one
  place (C412); RollTest split into one function per concern (C400), with
  AnimTest covering the root lock (C421).
- Also: C31, C143, C185, C340, C375, C475.

Judges: RollTest, the Eval suites, `AllocTest -SelfTest`, and every new mode
refusing a PASS unless its action landed.

## Phase 3 - one reset

Goal: close the whole class of reset bugs for good. Phase 0 fixes the
individual leaks in place (batches 77-79); this makes the next one impossible.

- **LevelRuntime**: one struct for a level's transient state (blasts, pending
  bolts, the pending fall, breaks, rest and lockstep, the clocks), reset as a
  whole by every path that leaves a level - the `m_harness = {}` pattern at
  src/Game/DungeonWorld_Save.cpp:180. The cursor's held item is cleared beside
  it.
- **WorldSession**: world-scoped Game and editor state (inspect cache,
  preview animator, routes, strokes, armed selections, the bake request, the
  prompt) in one struct that dies just before `m_world.reset()` (C236, C142,
  C150).
- Judge: `Eval.ps1 -SelfTest`, whose baselines print the `transients`
  readout added in batch 12.

## Phase 4 - one passage rule

Goal: one statement of what stops a body, sight, a flight, a blast and light.

- A pure, door-aware `Game/Passage.h`: a flat per-cell flag grid (rock, bore
  axis, closed door, solid prop, opaque prop, floor fixture, hole, party,
  monster footprint with capacity) and `Blocks(flags, mover, axis)` for
  Party, Monster(size), Missile, Blast, Sight, Light. DungeonWorld owns the
  live grid, versioned by a blockers revision that doors, breaks and
  placements bump; BuildAISnapshot copies it so the workers keep an immutable
  view.
- Every one of the 19 rules the review listed calls it: both line-of-sight
  checks (C53), the BFS walks, formation sides, FreeSlotInCell, the door jam,
  projectile blocking, blast spread, the drop / landing floor rule (C45,
  C191, C255), and a Huge monster's footprint (C46).
- Judges: a new RollTest section (the Blast.h pattern), the Eval suites,
  AllocTest -Melee / -Impact. ADD eval cases: a shot at a shut door, a blast
  at a door, a monster beside a brazier. Eval numbers will move - reported,
  not tuned.

## Phase 5 - the one hit pipeline

Goal: Michael's "ONLY ONE place". fx::Deal and the three adapters stay as the
ledger's foundation; one layer is added above them and the hooks below them
grow. Each step converts one call site per commit and is judged before the
next.

- **5a. Potency inside Deal.** DamageEvent carries the attacker's potency and
  Deal applies it, deleting the 8 hand copies (C3). Magic's hard-coded
  0.35 / 0.10 / 0.01 become Balance knobs (C7).
- **5b. The payload says what it is.** Blast type, flavour and strike kind
  travel on the payload; one flight struct; range and speed in squares
  (C28, C29, C30, C320, C452). Both the shared landing and spells-as-calls
  need this.
- **5c. Attack builders, Strike, one landing.** `MakePartyAttack(member,
  hand, verb)` and `MakeMonsterAttack(monster, kind)` are the only places that
  read stance, strength, curves and potency (C2). `fx::Strike(attack,
  delivery, target, ctx, narrator)` runs potency, Deal, the rider (skipped
  once the target is slain), procs (one policy for downed targets), the
  element stage, Learn, then React; the call site only narrates through a
  small INarrator (C4, C13, C14, C470). Convert in this order: party melee,
  monster melee, bolts, throws, blasts, scorch/crackle, fumbles.
- **5d. The Learn stage.** `ITarget::Learn(event, role)` - a no-op on monsters
  and breakables, `defense::LessonFrom` on a member; training leaves the call
  sites (C41, C42, C464).
- **5e. Monsters cast through Cast(ctx)**, with the side in the context, and
  pending bolts and active blasts move into ProjectileSystem with fixed
  capacity and one Clear; a blast keeps its shooter (C20, C23, C32, C59).
- **5f. The element stage** (answer 4). Relations in damagetypes.cat (water
  douses burn and lit things, fire ignites flammable things, air fans fires),
  `EffectKind::OnElement`, fixtures / torches / floor items as fx targets
  (one FixtureTarget merging Fire and FixtureBreak), and a zero-damage
  `DamageEvent::Touch` so Splash and Flame only put an element on a square
  (C6, C15, C16, C21, C324). **Blasts reach downed members** (answer 8), and
  melee and aimed bolts still do not (C453). This retires CLAUDE.md's
  "MonsterTarget::Wound is the one seam" for ignition.
- **5g. OnTick / OnExpire hooks** with a narrow fx::IWorld, so the DoT bite,
  light, sight and dazzle move into their own classes, and CastServices
  shrinks to spawn, applyEffect, touch, blast and message (C8, C22, C24, C25,
  C26, C50, C51). Rewrite docs/effects.md's status here.

Judges: PipelineTest (every route must move), SpellTest with `--selftest`,
AllocTest -Melee / -Cast / -Throw / -Impact / -Hand / -Light, RollTest, the
Eval suites. ADD: a RollTest table proving both builders give the same Attack
for the same inputs; a pipeline route for an element landing on a fixture;
an eval where a waterbolt douses a burning mummy and a firebolt lights a
sconce.

## Phase 6 - AI behaviours as classes

Goal: Michael's "each behaviour has its own implementation of a base class",
with one class per ENGAGEMENT style (answer 6). Needs phases 4 and 5e.

1. One archetype table, static_asserted against the enum; parse, write and
   the five predicates read it (C56).
2. Perception as data on `Agent` (cone, wake range, later hearing); swarm,
   lurker and sentry become presets of Melee; the lurker's "relentless"
   pursuit is built or deleted.
3. The worker half: `Behaviour::Decide(const Agent&, const IWorldView&)` - a
   const, allocation-free singleton, tested in RollTest against a fake world
   view - and a real BFS for return and patrol (C60).
4. The host half: `MonsterServices` (FollowPath, KeepRange, FleeFrom,
   ReturnTo, Strike, Shoot, Announce, LineOfSight) implemented once by
   DungeonWorld; `OnProvoked`; formation asks `takesSide`; the monster AI moves
   into its own DungeonWorld_Monsters.cpp (C54, C55, C61, C67, C72, C123).
5. Editor and save: one behaviour form for the type editor and the instance
   inspector, `behaviour =` with `archetype` kept as an alias, and a fixed
   per-monster state line in the save (C68). This reverses "MonsterConfigDialog
   owns the archetype rows". Rewrite docs/ai.md here.
6. A Thief, as the proof: a file pair, AllBehaviours.cpp, CMakeLists, lang
   keys x5 and an eval suite.

Judges: the Eval suites, AllocTest -Melee and Chase, PipelineTest,
ThreadStress (honest after batch 6). ADD eval suites for sentry, patrol,
leash and kite.

## Phase 7 - dialogs on one base

Goal: Michael's generic chrome and ONE base for editor dialogs and inspectors.
Independent of phases 3-6; it can run in its own worktree.

1. `ModalDialog` (+ `EditDialog<Config>` for working-copy dialogs) beside
   DialogLayout. The base owns the context, font sizing, open flag, one
   rebuild flag, the Esc ladder (an open popup first, then OnEscape, then
   Cancel - one path for the close box and Esc), the backing through
   DrawPanelFace with the pushed Theme, the help and busy overlays, and a
   Footer builder with ONE placement rule (answer 11: actions right-aligned
   across the whole panel, "?" at the far left). A dialog only builds its
   body. Rebase InstanceInspector on it and bring ProjectileInspector back
   (C75, C97).
2. Port the editor dialogs one per commit, simplest first, AssetDialog last
   (it leaves the legacy font path) (C84, C85, C86, C87, C88, C90, C103).
3. `ModalStack` (a fixed array, so a push from a guarded right-click
   allocates nothing) replaces Game's routing chains: the top dialog takes
   input in every AppState, draw bottom to top, CloseAll on unload, one
   preview owner (C76, C82, C188).
4. The game dialogs move over (ItemDetails and PortraitPicker as build-once,
   mouse-only, skinned), and a ConfirmDialog replaces the window-fraction
   Yes/No and SlotList's hand-drawn confirm (C91, C141). Re-run AllocTest
   -Sheet here.
5. One preview contract, then delete the dead UI API (C92 in phase 11's list,
   C159, C205, C284, C479).

Judges: `uioverlap` over every dialog, EditorTest, InGameTest, AllocTest -Sheet.

## Phase 8 - graphics seams

- **8a. A checked shader boundary** (answer 3): ShaderCompiler passes the C++
  constants as #defines (MAX_POINT_LIGHTS, MAX_SKIN_JOINTS, MAX_DUST_PUFFS,
  LIGHT_TILE_COUNT, SHADOW_SLOTS) and folds them into the cache key; an
  include handler serves a common.hlsli (tonemap, skinning VS, falloff,
  bias, the shared cbuffers), each included file's hash in the key; at
  startup D3DReflect compares every cbuffer's offsets and size against
  offsetof / sizeof and asserts; the shadow pass gets its own
  ShadowConstants instead of 8 KB per face (C131, C169, C170, C171, C172,
  C173, C176, C177, C203).
- **8b. One frame arena and one offscreen scope** in Graphics: a FrameArena
  per frame slot owned by GraphicsDevice (returns empty when full, counts
  drops, a gauge beside SRV), and an RAII OffscreenScope (barriers, viewport,
  flush glass, rebind the back buffer) that replaces the three bake rigs,
  ModelPreview's heaps and the raw D3D12 in DungeonWorld_Render (C162, C164,
  C165, C166, C168, C186).
- **8c. One kind-to-parts function per family** and one material-source rule,
  used by the world draw, the icon bake, the inspector preview and the picker
  (C157, C179, C182, C184, C192).
- **8d. A texture cache keyed by (set, resolution, sRGB)**: one set resident
  once, a quality swap reloads everything, and the model cache drops CPU-side
  images after upload (C155, C156, C161, C397).
- **8e. A deferred-release queue** (answer 9): `GraphicsDevice::Retire`
  keeps a resource until the fence passes, retiring most of the ~22 WaitIdle
  sites and ThumbCache's drain amortising (C167). Only after 8b and 8d.

Judges: AllocTest -Glass / -Lights, `/check-profile`, `geomhash`, a survey of
all 89 picker tiles, and the D3DReflect check itself (mutation: shift a
cbuffer field and require the startup assert).

## Phase 9 - data integrity

- **The schema is the single source**: loaders read CatalogSchema's `def`;
  the rename / delete sweep is derived from the CatalogRef rows instead of a
  hand-kept list; one `{catalog, id, consequence}` table names every id the
  C++ depends on, which Validate reports and rename / delete refuse (C303,
  C304, C319, C337, C461).
- **TryParse for DungeonMap and WorldMap**: a bad file becomes an Issue and a
  refusal with a message, not an abort; writers verify their output with it
  (C317, C318).
- **One truth for the active level** (answer 10): monsters and decorations
  are backed by records like doors and items, live instances spawn from them,
  and the live and *Remote editing APIs merge into one that takes a level as
  its target (C309, C312, C313, C314, C98, C133, C256, C257).
- **One value grammar** for lists, colours, bools and cells (C315, C316).
- Also: C322, C325, C329, C335, C354, C358, C472.

Judges: EditorTest, WorldTest, LevelBuildTest, `catround`. ADD: a check that
the sweep covers every CatalogRef row.

## Phase 10 - extractions, as features need them

Pull these out of DungeonWorld and Game, each when a feature needs it rather
than all at once: a LightSystem (C138), a LevelStore (from phase 9), an
EditHistory up in Game (C136's remainder), a WorldEditor, DevTools command
groups registered by GameApp, GameHud's IWorldActions in place of 24
pass-through std::functions (C140, C144), and a Main facade (C135). Together
these close C119 and C124. Also C139, C151, C152.

Judges: the quick tier and `/check-build`.

## Phase 11 - deletion and the doc check

Delete the dead code, sweep the remaining stale docs and comments, and add a
script that lists every backticked name in CLAUDE.md and docs/ that grep
cannot find in src/, tools/ or assets/. Then `/check-build`, because release
is where removing something "unused" breaks. The 86 issues are listed in the
index below.

## Dropped

- **C137** (party rules inside DungeonWorld): the verifier found no defect,
  and the shared-RNG decision argues against splitting it now.
- **C283** (ui::TextOutput dead): superseded by C92, which deletes the dead
  UI API in phase 7.
- **C369** (WorldState::seen searched linearly): negligible at today's world
  size; revisit if worlds grow.

## Questions this plan raised

These are asked one at a time; the answers are recorded here.

1. **Does a Wind Ward that deflects a burst bolt also stop its blast?**
   (batch 24, C1) Proposed: yes - a turned bolt does not land.
2. **Does a miss or a turned blow end rest as "attacked" and wake the
   monster?** (batch 29, C34) Today only a landed blow does. Proposed: yes for
   both, which changes CLAUDE.md's definition of rest's `attacked`.
3. **Should conjured pebbles recycle?** (batch 45, C227) Rock with full hands
   drops a pebble each cast; past the 64-drop pool it allocates. Proposed:
   reuse the oldest uncollected pebble.
4. **May a secret-niche reveal excuse itself once?** (batch 39, C211) The
   preferred fix pre-builds the opened chunk; the fallback is one
   alloc::Excused per niche, which is an event exemption.
5. **Does a flask that shatters on a wall train throwing?** (batch 27, C40)
   Proposed: no - only contact with a monster.
6. **Should monster shots pay the offense stance?** (phase 5c, C2) Melee pays
   it and the guard bonus assumes it; shots do not. Paying it cuts caster
   accuracy from about 75-85 to 34-47 and moves threat, pools and evals.
   The alternative is to make the guard bonus melee-only.
7. **Rest at 60x in fixed sub-steps** (batch 35, C64): monsters then think at
   their real rate while the party rests, but a slow frame caps how fast rest
   runs. Proposed: yes, and Michael feels it in game.

## Index - where each issue is closed

One line per issue: severity and kind, where it is closed, and its headline. "P0 batch N" is a phase-0 batch above; "P5 5c" a step; "P11" the deletion and docs phase.

- C0 - high bug - P0 batch 22 - Rolled blows can heal: ResolveAttack lets soak overflow invert damage while the unrolled path clamps
- C1 - high bug - P0 batch 24 - A monster's burst (Hagalaz) bolt never detonates when it hits the party
- C2 - high bug - P5 5c - Monster attack profile is built three ways: ranged/caster shots and blasts drop strength, stance, crit-pierce and the shooter's potency
- C3 - high rule - P5 5a - Attacker potency and the enchantment rider are applied by hand at each call site outside fx::Deal, and the copies disagree (throw, fumble)
- C4 - high rule - P5 5c - Post-Deal consequences (procs, on_crit, React, flinch, narration, training) are hand-written at ~10 sites and have drifted
- C5 - medium bug - P0 batch 26 - MonsterTarget::Wound has no dead guard, so an enchanted killing blow re-kills the corpse
- C6 - high rule - P5 5f - Element interactions (water douses fire, ignition, quench, cures) live outside fx as per-source world code
- C7 - medium rule - P5 5a - Spell power and fail roll use hard-coded constants and a separate offence shape instead of balance knobs
- C8 - medium structure - P5 5g - Effect behaviour (light/sight/dazzle schools, tick, modifiers) lives in DungeonWorld; effect classes are data bags and the modifier hooks are dead
- C9 - medium bug - P0 batch 25 - A Sowilo light running out prints the Sight spell's fade line
- C10 - high bug - P0 batch 27 - A severe fumble's drop/fling loses a torch's charge and allocates in the swing frame
- C11 - medium bug - P0 batch 22 - A Windward-deflected bolt still trains avoid (TrainDefense runs before the deflect check)
- C12 - low bug - P0 batch 22 - Soak/Resist/DefenseFor count hand slots while the armor class excludes them; DefenseFor re-sums soak
- C13 - low cleanup - P5 5c - Door strikes hand-clear rolled instead of using the Impact preset
- C14 - low cleanup - P5 5c - DamageEvent::source/slew mean different things per side (victim in monster melee for fire-shield threat)
- C15 - medium structure - P5 5f - Breakable coverage overclaimed: bolts and throws strike doors only; one fixture carries two effect lists
- C16 - low cleanup - P5 5f - Fire-state logic (ignite/clear/relight, bracket torch rule) repeated across functions
- C17 - medium bug - P0 batch 25 - A light flare scorches/shocks every dazzled monster on the level and kindles or dazzles through walls and shut doors
- C18 - medium bug - P0 batch 24 - Gust repel scales only atk.damage, so a turned shot keeps its full blast and procs
- C19 - medium bug - P0 batch 25 - ModifiedSpell builds bolts from the base spell's payload, so its own spells.cat on_hit is dead
- C20 - medium structure - P5 5e - Monster casting is a parallel bolt-only API: volley timing re-implemented, threat ignores volley and burst
- C21 - medium rule - P5 5f - Shove and Gust repel are resolved outside fx (hard-coded step after Deal, contest in ProjectileSystem)
- C22 - low cleanup - P5 5g - Ward, Sight and Light spell forms repeat the same apply-effect-for-duration shape
- C23 - medium structure - P5 5e - ModifiedSpell is a dynamic_cast switch over the forms it modifies
- C24 - medium structure - P5 5g - CastServices has grown into a flat list of single-spell world verbs, and the flare's school switch lives in DungeonWorld
- C25 - low cleanup - P5 5g - CastServices/CastContext filled by positional aggregate init; null checks inconsistent; dead field
- C26 - low cleanup - P5 5g - Hand-spell, blast, scorch and flare particle bursts are hard-coded literals (school->profile switches, glow pick copies)
- C27 - low cleanup - P11 - Dead Spell getters, unread spells.cat fields and stale spell headers
- C28 - medium cleanup - P5 5b - Projectile structs re-declare the same flight fields and are copied by positional init
- C29 - medium structure - P5 5b - The projectile payload does not say what it is: blast type, flavour and strike kind supplied by callers
- C30 - medium rule - P5 5b - Flight range and speed mix metres and squares against the kUnit rule
- C31 - low cleanup - P2 - Quadrant-lane lateral and member lane side re-derived at each site
- C32 - low structure - P5 5e - ProjectileSystem doubles as the world's general VFX particle pool
- C33 - low bug - P0 batch 28 - Party melee strikes whichever monster is first in the list in a multi-occupant square
- C34 - medium bug - P0 batch 29 - Waking a monster and interrupting rest only happen in the apply stage, so a miss or immune hit does neither
- C35 - medium cleanup - P0 batch 21 - Stat lists as std::vector<std::string>; TrainDefense's function-local statics allocate on the first blow in an armed frame
- C36 - medium bug - P0 batch 26 - Stat-up log lines print raw loc keys (INT/WIL/VIT and the throw's abbreviated stat ids)
- C37 - low cleanup - P11 - kMeleeUses is a second hand-kept copy of Balance's attack-verb list
- C38 - low cleanup - P11 - Skill-level curve and its level^2 inverse hand-written at several sites
- C39 - low bug - P0 batch 26 - Punch/Kick with a command-less item in hand uses that item's empty skill
- C40 - medium bug - P0 batch 27 - Thrown bombs never train throwing
- C41 - low cleanup - P5 5d - Lighting a magical torch spends mana but trains no attunement; mana has no single spend seam
- C42 - low cleanup - P5 5d - Training XP units hard-coded at four sites while other rates are balance knobs
- C43 - high bug - P0 batch 23 - A blast whose bolt stops in a shut door or a 1-thick wall spreads to both sides
- C44 - medium bug - P0 batch 23 - A wall expiry resolves its payload in the wall's own cell, so the cell-wide expiry catches nobody
- C45 - medium bug - P4 - Solid props stop bodies but not bolts, throws, blasts or sight
- C46 - low bug - P4 - A Huge monster's 2x2 footprint is known only to monster occupancy; party movement, melee, bolts and blasts test its anchor
- C47 - medium bug - P0 batch 27 - Saving with a shattering flask in flight detonates it, possibly on the party
- C48 - medium bug - P0 batch 35 - Projectile flight is endpoint-sampled, so at 60x rest shots tunnel through bodies and thin walls
- C49 - high rule - P0 batch 21 - m_activeBlasts is never reserved: the first blast of a session allocates in a guarded frame
- C50 - medium cleanup - P5 5g - The light-dims-as-it-runs-out rule and reach shrink are retyped in several places
- C51 - low cleanup - P5 5g - Light-profile ids are string literals scattered across files plus a hand-kept validation list
- C52 - high bug - P0 batch 34 - Stale worker plans re-latch monster awareness after a new game or load (runtimeIds survive the reset)
- C53 - high bug - P4 - Line of sight is implemented twice, the copies disagree (braziers, bores) and neither is blocked by a shut door
- C54 - medium bug - P6 - ProvokeMonster forces Engage on kiters and fleers, stalling them until their next think
- C55 - medium structure - P6 - Archetype behaviour is an enum switched across ~15 sites and two threads, with host executors on DungeonWorld instead of one class per behaviour
- C56 - medium cleanup - P6 - Archetype name table, traits and the behaviour editor form are restated in about five places
- C57 - medium bug - P0 batch 33 - AssignFormation gives melee attack sides to kiters and fleers
- C58 - high bug - P0 batch 33 - Formation assigns attack cells no monster can enter (brazier, crate, shut door), and the monster stalls
- C59 - low bug - P5 5e - A caster's non-bolt spell silently falls back to the ember bolt with no warning or validation
- C60 - medium bug - P6 - Leash return and patrol use greedy steps that stall behind walls; sentry/patrol/leash are untested
- C61 - low cleanup - P6 - Dead AI writes, unused members and misplaced AI comments
- C62 - high rule - P0 batch 32 - Brain::FindPath allocates a std::deque per BFS, on workers and on the main thread in armed frames while resting
- C63 - medium bug - P0 batch 34 - Lockstep Pause does not wait for a tick in flight, so ComputeInline can race a worker on the plan pool
- C64 - medium bug - P0 batch 35 - Rest at 60x is frame-quantized: fast-bucket monsters think about once per simulated second
- C65 - medium bug - P0 batch 31 - A corpse in a doorway jams the door forever (MonsterRuntimeIdAt ignores Alive)
- C66 - medium rule - P0 batch 32 - AI snapshot/grid pools grow lazily to their in-flight high-water, allocating in random guarded frames
- C67 - low cleanup - P6 - Run, Flee and Defend creature states are authorable but never chosen
- C68 - low bug - P6 - Monster save drops facing and patrol progress, with no seam for per-behaviour state
- C69 - medium bug - P0 batch 34 - AsyncDirector leaves dead worker slots whose jobs capture the destroyed director (boot = use-after-free; 4 slots per world load)
- C70 - low rule - P0 batch 32 - AI pools rely on use_count()==1 as a synchronization point; a force-kill leaks a pooled buffer
- C71 - medium rule - P0 batch 21 - m_formationScratch is never reserved; AssignFormation allocates mid-fight at a new aware-monster high
- C72 - low cleanup - P6 - Threat lock test and highest-threat search duplicated
- C73 - low bug - P0 batch 28 - Animation variant picks draw from the seeded combat RNG, so editing clips changes seeded combat
- C74 - low bug - P0 batch 33 - Pits and stair holes are plain floor to monsters, drops and landings
- C75 - high structure - P7 - No common modal-dialog base: ~15 dialogs and pickers copy the shell (ctor, font clamp, Esc, rebuild, wash, backing, close/revert)
- C76 - high structure - P7 - No modal stack: Game and GameUI route every modal through hand-ordered input/draw/teardown lists that already disagree
- C77 - medium bug - P0 batch 73 - Worlds and World-settings dialogs open in Playing but receive input only in the WorldMap state
- C78 - medium bug - P0 batch 53 - With the console open the world runs through the editor pause and modals; editor subcommands clear the pause
- C79 - medium bug - P0 batch 73 - WorldMapView keeps Editor mode as the Playing overlay page, routing neither world dialogs nor stroke commit
- C80 - medium bug - P0 batch 74 - Finishing a patrol route reopens the inspector from a shared m_inspectCfg (wrong monster)
- C81 - medium bug - P0 batch 75 - Esc is inconsistent: AssetDialog ignores it, and with a drop-down or picker open Esc closes the whole dialog
- C82 - low cleanup - P7 - Dead console-open guards after the console's early return
- C83 - low cleanup - P11 - Identical onPickAsset wiring and small copy-pasted callbacks across Game wiring
- C84 - low cleanup - P7 - Inspector delete/undo-bracket lambda pasted eight times (reports removed even when nothing was)
- C85 - medium cleanup - P7 - Form-field helpers copied across dialogs: AddNumericField x3, record-id filter x12; RenameType checks nothing
- C86 - medium cleanup - P7 - Three word-wrap implementations; the shared help overlay allocates every frame and the type editor's copy cuts fields off
- C87 - low cleanup - P7 - The typed-delete owner flow is repeated in TypeEditorDialog and WorldsDialog
- C88 - medium cleanup - P7 - Click-to-rename title state machine duplicated between TypeEditor and LevelSettings
- C89 - medium bug - P0 batch 44 - AssetDialog alone uses the legacy owned-Font UIContext (Consolas 18px, no scales), keeping Font::SetHeight and a second font path alive
- C90 - low bug - P7 - Editor dialogs paint their panel in the user Theme but widgets in the default Theme
- C91 - medium bug - P7 - Yes/No confirm and InspectPicker are hand-placed by window fractions (body overruns); SlotList draws a second hand-built confirm
- C92 - low cleanup - P11 - Dead UI API: panel-fraction AddCloseButton/CloseButtonRect, DialogTitleFont/DialogTextFont and other helpers
- C93 - low cleanup - P11 - Settings page is the one menu page not on a PageCard
- C94 - medium cleanup - P11 - Tooltip body hand-drawn at ~10 sites in several looks (some bypass the theme); ui::DrawTooltip is private
- C95 - medium cleanup - P11 - Sheet and party-window cards duplicate the status band, trim helper, bank pointers and service-lambda wiring
- C96 - medium bug - P0 batch 55 - Character sheet lays out in 16:9-tuned panel fractions while its panel takes the window aspect
- C97 - medium structure - P7 - ProjectileInspector is a standalone modal outside the inspector base
- C98 - medium structure - P9 - Inspector seam on DungeonWorld is per-kind and ad hoc: two handle types, mismatched preview/remove keys, Config structs mirroring Edit structs
- C99 - medium bug - P0 batch 74 - Monster behaviour form copied into EntityInspector; both show a caster spell that is never saved
- C100 - medium bug - P0 batch 71 - Renaming a quest stage through another stage's id wipes that stage's log line
- C101 - high bug - P0 batch 71 - Type editor builds no control for FieldKind::DamageType (dmgtype and damage_type uneditable)
- C102 - medium bug - P0 batch 73 - WorldSettingsDialog's single note label points at the wrong tab when a doorway is selected
- C103 - medium cleanup - P7 - Keep-a-stale-value-selectable drop-down hand-coded 5 times and missing in ~8 more
- C104 - low bug - P0 batch 74 - EntityInspector rebuild resets to the AI tab ('Clear route' bounces the user away)
- C105 - low cleanup - P11 - Vestigial WorldsDialog create path and unused members
- C106 - low cleanup - P11 - Two seed-reroll rules for the one generator; Create & play re-implements PressCreate
- C107 - low bug - P0 batch 76 - Inspector and InspectAt text hard-code English, bypassing Loc
- C108 - low cleanup - P11 - PartyCreationPage RefreshFaces re-implements SyncPortraits
- C109 - medium cleanup - P0 batch 51 - A party spec's shown name/face/colour is worked out in about seven places; one copy drifted
- C110 - low cleanup - P11 - PortraitGrid and StonePicker hand-roll wrapping thumbnail grids
- C111 - low bug - P0 batch 65 - ThumbCache eviction does not protect on-screen portrait tiles
- C112 - low rule - P0 batch 51 - PartyCreationPage::Tick reformats its labels every frame
- C113 - low cleanup - P11 - PartyCreationPage hygiene (hard-coded slots, Stack downcasts, 'human' id in code)
- C114 - low cleanup - P11 - Dead members and stale comments in the game pages slice
- C115 - low bug - P0 batch 79 - Pending Yes/No prompt survives a console-driven game restart and answers against the new game
- C116 - low cleanup - P11 - AssetPicker::Close keeps its thumbnails and preview resident until next Open
- C117 - low rule - P11 - Asset picker tile badge is formatted per tile per draw
- C118 - medium structure - P1 - Game is one flat 331-file library whose internal layering is unchecked
- C119 - medium structure - P10 - DungeonWorld is a runtime + editor + renderer god class behind a 4761-line header; split the class by concern
- C120 - medium structure - P1 - Private nested types live in DungeonWorld.h; headers include it whole for one nested type
- C121 - medium structure - P1 - PoolModelLook (editor tile-framing heuristics) and its loader are parked on DungeonWorld
- C122 - medium structure - P1 - DungeonWorld.cpp and DungeonWorld_Load.cpp are grab-bags (gameplay pick/drop in the Load file)
- C123 - medium structure - P6 - DungeonWorld_Combat.cpp is ~2900 lines covering ~12 concerns, including AI executors
- C124 - medium structure - P10 - Game is a god class (dialogs, editor services, world tier, harness, save mapping)
- C125 - medium structure - P0 batch 53 - Game.cpp's 735-line UpdateStates copies the world-tick tail four times and they diverge
- C126 - medium structure - P1 - GameUI.cpp (and eight other files) exceed the ~2000-line split rule; huge BuildSettings/BuildHud
- C127 - medium structure - P0 batch 50 - Controls.cpp is 2500 lines; DropDown re-implements ScrollArea's scrollbar against the one-owner rule
- C128 - low structure - P1 - Rules-side headers (Character.cpp, Projectiles.h, GameSettings.h, PartyHudTypes.h) drag in UI and D3D12 headers
- C129 - low structure - P1 - DungeonWorld reaches up into app settings and the HUD draw module
- C130 - low structure - P1 - The dev console and its perf/profile dashboards are engine tooling living in Game as one class
- C131 - medium structure - P8 8a - Graphics public types and shaders are named after game features
- C132 - low structure - P1 - Generic texture-file loaders sit in Game/AssetUtil
- C133 - low structure - P9 - Catalog/Serialize live in Game, so FontLibrary cannot read fonts.cat
- C134 - low structure - P1 - Small engine-generic helpers parked in Game (dialog chrome, thumb cache, load queue)
- C135 - low structure - P10 - Main does more than glue and the command line is parsed four times
- C136 - medium structure - P0 batch 85 - Undo and stair moves reach into Game's WorldMap/Project through raw pointers, writing a const project
- C137 - low structure - drop - Party rules (tick, rest, supplies, leader, throw) live inside DungeonWorld
- C138 - medium structure - P10 - Light budget, fades and profiles are DungeonWorld members over five TUs; extract a LightSystem
- C139 - low structure - P10 - Character <-> SaveData mapping lives in Game::SaveGame/LoadGame
- C140 - medium structure - P10 - Item-use game rules (memorize, eat/drink stepping, throw clear) live in GameUI
- C141 - low structure - P7 - Constructor still holds ~31 dialog-wiring lambdas despite the split banner
- C142 - low structure - P3 - Eval/alloctest harness state sits loose in Game.h instead of one struct
- C143 - medium structure - P2 - Dev commands: 1,200-1,500-line Register functions and a no-world gate kept apart from registration
- C144 - medium structure - P10 - GameUI is a hub of 48 std::function members with a dangling-reference trap
- C145 - low cleanup - P11 - The seven UIContexts are enumerated by hand in five places
- C146 - low cleanup - P11 - Game_DevParty has drifted from its concern
- C147 - low cleanup - P11 - DrawProfileSection is one 895-line function
- C148 - low cleanup - P1 - Frame-rate metric owned by DevConsole; Game reaches FPS through the console
- C149 - low structure - P11 - WorldMap.h is a grab-bag (FlagOp, WorldState, overworld map) with backdoor API
- C150 - low structure - P3 - DungeonWorld_Validate.cpp mixes level install, generator queries and the read-only cache under a validation banner
- C151 - medium structure - P10 - MapView has become the editor's shell (986-line Render, editor chrome, split drag state)
- C152 - medium structure - P10 - The palette re-implements text field, checkbox, scroll and tooltip outside the control tree
- C153 - low cleanup - P11 - Win32 key codes leak: Platform's vk table is too small, so UI/Game use VK_* via transitive Windows.h
- C154 - medium bug - P0 batch 64 - Quality hot-swap reloads only surfaces; props, fixtures, doors and runes keep their first tier
- C155 - medium cleanup - P8 8d - PBR sets have two unshared owners and no keyed cache, so one set can be resident several times
- C156 - low cleanup - P8 8d - Set-stem resolution, the stem+_n+_mr load triple and LoadPbrSet's fallback are restated at several sites
- C157 - medium structure - P8 8c - 'Bound set or the model's own materials?' is decided by four rules in the kind loaders and a fifth in the picker
- C158 - medium bug - P0 batch 65 - 2D thumbnails and swatches sample sRGB views into a UNORM target and draw gamma-darkened
- C159 - medium cleanup - P7 - Three model-preview loaders and two set-preview loaders; AssetDialog keeps the old meshes[0] look
- C160 - medium cleanup - P11 - Font atlas uploads as RGBA8 with a CPU mip chain on every glyph commit
- C161 - low cleanup - P8 8d - glow_radial.png is loaded into two textures; model-less items get a placeholder each
- C162 - low cleanup - P8 8b - The world's ParticleBatch is rebuilt on every level load
- C163 - high bug - P0 batch 59 - SpriteBatch's fixed 4 MB arena aborts on the editor map of a 128x128 level
- C164 - low rule - P8 8b - Per-frame upload arenas are fixed-size with a fatal assert, uncapped producers and no high-water readout
- C165 - low cleanup - P8 8b - Game keeps a second ParticleBatch just for the inspector's torch preview
- C166 - low cleanup - P8 8b - Root signature/sampler/PSO boilerplate repeated in four pipeline owners; duplicate 1x1 textures
- C167 - medium structure - P8 8e - GPU resource lifetime rests on ~20 caller-side WaitIdle drains; no deferred-release queue
- C168 - medium structure - P8 8b - Offscreen icon-bake rig is raw D3D12 in Game, written three times beside ModelPreview; BakeMesh/MonsterIcon inline the Begin/End bake
- C169 - medium cleanup - P8 8a - Shadow faces upload the full 8 KB FrameConstants per face and submit the whole sphere to all six faces
- C170 - medium structure - P8 8a - C++/HLSL layouts and array sizes are mirrored by hand (no #include/defines), and shadow.hlsl's copy has drifted
- C171 - low cleanup - P8 8a - Glass pixels run the dust raymarch twice
- C172 - low cleanup - P8 8a - scene.hlsl repeats the point-light falloff and the shadow bias literal
- C173 - low cleanup - P8 8a - Shadow-cube SRV table depends on eight consecutive AllocateSrv results, unasserted
- C174 - low cleanup - P11 - Unused Graphics API, uploaded-but-unread constants (directional light, gPointLightCount)
- C175 - low cleanup - P11 - Stale comments in the GPU core and shaders
- C176 - low cleanup - P8 8a - Hand-mirrored C++/HLSL bar constants only roughly match
- C177 - low bug - P8 8a - particle.hlsl has no inline-tonemap path, so the torch preview flame is ungraded
- C178 - high bug - P0 batch 60 - Shadow-cube cache never invalidates for non-monster dynamic casters or a vanished corpse
- C179 - medium bug - P8 8c - 'Kind -> draw parts' is built separately for world, icon bake and inspector previews; door/lever previews omit parts
- C180 - medium bug - P0 batch 63 - Rune / enchanted-weapon floor glow ignores wall niches; floor-item pose is resolved separately per consumer
- C181 - medium bug - P0 batch 62 - Fire-light loop dereferences PushLight's result, which is null at the 256-candidate ceiling
- C182 - low cleanup - P8 8c - LoadPoolModelLook's set-wins material approximates ApplyPropMaterial
- C183 - low bug - P0 batch 61 - The map's monster head-shot bakes the bind pose
- C184 - low cleanup - P8 8c - Rune tablet pulse and materials duplicated by hand-matched constants beside lights.cat
- C185 - low cleanup - P2 - Projectile particles use metre literals while FireEffect scales by kUnit; colour parsing has two grammars
- C186 - low cleanup - P8 8b - Fires sort particles regardless of view; one new kind re-bakes every icon of its category
- C187 - low cleanup - P0 batch 60 - ShadowScheduler: dead frame counter, positional incumbency, stale ShadowSlotCache docs
- C188 - medium structure - P7 - Dialog 3D previews come in five protocols, chosen by an if/else chain in Game::Render that also restores the output merger by hand
- C189 - low bug - P0 batch 61 - Skinned-palette upload cache is keyed by a temporary animator's buffer address
- C190 - medium bug - P0 batch 62 - Enchanted weapon's element glow is overwritten by the category tint
- C191 - medium bug - P4 - Light-reach and Earth-stone walks ignore doors and bores
- C192 - low cleanup - P8 8c - BuildMultiMaterialModel uploads each primitive separately; the picker re-uploads split parts
- C193 - medium bug - P0 batch 64 - BuildDungeonMeshes frees chunk meshes before draining, with frames in flight (arena, eval reset)
- C194 - high bug - P0 batch 70 - Alt+Tab out of exclusive fullscreen skips ResizeBuffers, so the next Present aborts
- C195 - medium bug - P0 batch 67 - GPU failures abort without the HRESULT, device-removed reason or DRED data
- C196 - medium bug - P0 batch 68 - An untouched Windowed Apply sizes and centres on the primary monitor, ignoring the Monitor choice
- C197 - medium bug - P0 batch 69 - The chosen GPU is saved by LUID, which changes every reboot
- C198 - medium bug - P0 batch 69 - On hybrid laptops the auto-picked dGPU owns no outputs: Borderless does nothing and lists are empty
- C199 - medium bug - P0 batch 69 - Video tab uses a display list frozen at boot and saves the monitor as an index
- C200 - medium bug - P0 batch 70 - The frame cap works in whole hertz and drifts against fractional refresh rates
- C201 - medium bug - P0 batch 70 - No DPI awareness: scaled displays bitmap-stretch the swapchain and resolutions apply at the wrong size
- C202 - medium bug - P11 - The DDS layout is encoded twice with hand-written offsets (reader vs baker writer); stale docs
- C203 - low cleanup - P8 8a - Rune groove 0.45 mirrored by hand between RuneBaker and scene.hlsl
- C204 - medium bug - P0 batch 49 - Etched-symbol gold baked into PNGs skips the per-material ink solve
- C205 - medium structure - P7 - Shared UI icon textures live in namespace-scope globals and a UI-lib registry with manual release
- C206 - low cleanup - P11 - D3D12 message-throttle state is file-scope globals although the callback carries a context
- C207 - low bug - P0 batch 52 - Process-wide save-list world filter is hidden Game policy and goes stale after a world switch
- C208 - medium bug - P0 batch 48 - Global UI clip stack: nested clips not restored and one throw clips every later frame
- C209 - medium rule - P2 - The player map and world-map travel screen are exempt from the guard and rebuild strings/markers every frame
- C210 - high rule - P0 batch 38 - Exit-stair prompt and pit-fall latch allocate in frames that stay in Playing, invisible to the transition disarm
- C211 - high rule - P0 batch 39 - Secret-niche reveal and lever presses rebuild chunk meshes (WaitIdle + upload per chunk) inside armed play frames
- C212 - medium rule - P0 batch 40 - Quest/flag/found hooks allocate in armed frames, and OnItemFound re-asserts the retired event exemption
- C213 - medium rule - P0 batch 14 - AllocTest leaves most gameplay paths unexercised; CheckAll runs few modes, no Python judges and no RollTest
- C214 - medium bug - P0 batch 15 - The first armed frame after a disarm captures no stacks, so a violation there is logged nowhere
- C215 - medium rule - P0 batch 15 - log:: calls do not excuse themselves, so reports from guarded frames count as violations
- C216 - low cleanup - P0 batch 14 - AllocTest -Pause coverage overstated, plus stale comments
- C217 - low rule - P0 batch 38 - Two excuses are not 'reporting' (MoveKeysHelp / Help button, ReturnToTitle)
- C218 - high rule - P0 batch 40 - Item ids over 15 chars allocate on lift/rename; allocation-free moves rely on MSVC SSO
- C219 - high rule - P0 batch 41 - Party-bar effect strip Repeater is never warmed, so the first effect allocates widgets in play
- C220 - low rule - P0 batch 42 - Warm/reserve sizes below their producers (32 spell rows vs 44 spells; 160-byte labels fed 255)
- C221 - medium rule - P0 batch 44 - Floating-panel resize bakes and keeps a font atlas (with GPU drain) per pixel size; FontLibrary never evicts
- C222 - medium rule - P0 batch 64 - Model cache keeps every model's embedded image bytes in CPU RAM for the world's lifetime
- C223 - low rule - P0 batch 43 - The uitree overlay flag is invisible to SteadyStateFrame; inspect::Draw allocates every frame
- C224 - low rule - P0 batch 43 - DrawButtonFace takes const std::string&, forcing per-frame temporaries
- C225 - low cleanup - P11 - Every CharacterSheet (incl. 4 cards) re-bakes Skills+Effects rows every frame regardless of tab
- C226 - low bug - P0 batch 15 - stack::SeenSet reports every new stack again forever once full
- C227 - medium rule - P0 batch 45 - Floor-drop headroom (64) is a soft limit conjured items can exceed
- C228 - medium rule - P0 batch 41 - ResetRoster's resize path drops new members' effect-list reservations
- C229 - low rule - P0 batch 44 - Font glyph misses are excused rather than pre-warmed (bake + upload + WaitIdle mid-play)
- C230 - low cleanup - P2 - Documented reserve floors (projectiles, palette cache, sprite vertices) are not enforced bounds
- C231 - low rule - P0 batch 47 - Several C-API boundaries (FILE*, stb buffers, IXAudio2) are not RAII-wrapped
- C232 - high bug - P0 batch 74 - ReloadTypeKind frees kind GPU data that Game-held preview caches still borrow (use-after-free)
- C233 - medium bug - P0 batch 74 - Patrol-route id, inspector config and Delete closure survive UnloadWorld and hit the next world's monster
- C234 - low bug - P0 batch 79 - A console world switch during a bake lets FinishBake write into the next world's catalog
- C235 - low bug - P0 batch 71 - Type editor theme swatches hold raw albedo pointers freed by a quality change
- C236 - medium structure - P3 - UnloadWorld is a hand-kept borrower list with no counterpart for in-world reloads
- C237 - low cleanup - P11 - About 360 KB of profile-only storage sits inline in DevConsole in every build
- C238 - medium cleanup - P2 - Core/StringUtil lacks Trim/split; they are re-implemented in ~20 places (Style's Trim differs)
- C239 - low cleanup - P2 - ASCII-lowercase / contains-no-case and whitespace-split copied about a dozen times
- C240 - medium cleanup - P2 - Number parsing has no shared helper; dev commands use unchecked atoi/atof
- C241 - medium cleanup - P2 - About 22 dev commands hand-parse member index / on-off / direction; a typo targets member 0
- C242 - medium cleanup - P2 - Direction name/token tables re-declared in many places (and compass tokens parsed four ways)
- C243 - low cleanup - P2 - Facing-delta / cardinal-step tables and kPi copied across TUs
- C244 - low cleanup - P2 - FNV-1a, fixed-buffer copy and UTF-8 conversion re-written across Core
- C245 - low cleanup - P11 - gfx::Rect has only Contains; Intersect and insets re-derived
- C246 - low cleanup - P11 - Small math copies (Vec4 Mix, inline smoothstep, normalization)
- C247 - low cleanup - P11 - The probability roll repeated beside generate's Chance()
- C248 - low cleanup - P11 - Read-whole-file-as-text open-coded
- C249 - medium cleanup - P2 - Allocation-free text helpers (tenths, loc key builders, fit) copy-pasted per file
- C250 - low cleanup - P11 - Five hand-rolled fixed-capacity inline string types
- C251 - medium cleanup - P2 - First-solid-wall mount scan copied six-plus times
- C252 - low cleanup - P11 - Mesh-vertex AABB loop hand-written about ten times; cached bounds recomputed
- C253 - medium bug - P0 batch 66 - glTF node-transform bake copied at ~7 sites with a shared normal/winding defect; multimaterial cull radius ignores it
- C254 - medium cleanup - P11 - Mesh-builder call with five resolver lambdas copied three times
- C255 - medium cleanup - P4 - What blocks a square is coded three times (party, monster slot, AI grid)
- C256 - low cleanup - P9 - Button/item placement rules copied between load, live-add and remote-add
- C257 - medium cleanup - P9 - The decoration world transform rebuilt at seven sites
- C258 - low cleanup - P0 batch 63 - Ray-sphere click picking copied four times; niche pick ignores kUnit
- C259 - low cleanup - P11 - Door motion resolved twice; DoorTypeAt scans records needlessly
- C260 - low cleanup - P11 - Repeated literals and helpers (dimmed radius, flame colour, Trim, direction tokens)
- C261 - medium cleanup - P11 - Button push clock and carved-stone face written three times
- C262 - low cleanup - P11 - MessageLog's corner row hand-draws buttons instead of hosting ui::Button
- C263 - low cleanup - P0 batch 54 - Effect icon look and time sliver coded twice
- C264 - low cleanup - P11 - Press-then-release click latching hand-written five times
- C265 - low cleanup - P11 - Easing re-implements EaseShape; kEaseShapeCount hand-typed
- C266 - low cleanup - P11 - 15 spell classes carry only constructor numbers that spells.cat sets again
- C267 - low cleanup - P11 - Literal 4 used instead of party::kMaxMembers (and 5 for the sheet tab count)
- C268 - low cleanup - P11 - The 'first level or level1' fallback re-inlined
- C269 - low cleanup - P11 - Small copy-paste blocks in GameUI
- C270 - low cleanup - P11 - Copy-pasted lookups, path builders and button drawing in the dev console
- C271 - low cleanup - P0 batch 57 - Console colours retyped, breaking 'one colour per processor'
- C272 - low cleanup - P11 - Three copies of an empty Input; two function-local statics
- C273 - low cleanup - P2 - setstat parses stat names by hand
- C274 - low cleanup - P11 - rune is a weaker twin of give
- C275 - low cleanup - P11 - project.ini path concatenated by hand in 8 places
- C276 - low cleanup - P11 - Area/location edit rules restated in dev lambdas
- C277 - low cleanup - P11 - Issue counting/printing duplicated between generate, validate and ValidateDialog
- C278 - low cleanup - P11 - Dead Character ward/effect helpers, Inventory::Grow and other unused queries
- C279 - low cleanup - P0 batch 25 - Ward kinds declare no school, so the dev effect command hard-codes one
- C280 - low cleanup - P11 - Dead DungeonWorld public methods (MonsterInstanceAt, MonsterPowerRange, Ledger::DropAll)
- C281 - low cleanup - P11 - Seven Core functions have no callers; ResetThisThread would corrupt the verdict if used
- C282 - low cleanup - P11 - Unused public API in Assets/Platform/Audio
- C283 - low cleanup - drop - ui::TextOutput is dead but still compiled and documented
- C284 - low cleanup - P7 - Dead InstalledTextureSets and unreachable optionsFor cases
- C285 - low cleanup - P11 - ParseOnHit's deprecated aliases have no users
- C286 - low cleanup - P11 - FireEffect::SteadyCount unused
- C287 - low cleanup - P11 - Dead functions and fields in the HUD slice
- C288 - low cleanup - P11 - Dead MapView bits (m_device, include, kPi, raw VKs)
- C289 - low cleanup - P11 - Dead hit-scale safeguard and write-only ProfSeries fields
- C290 - low cleanup - P11 - Stale dev preview command keeps a render branch and four Game members alive
- C291 - low cleanup - P11 - Dead DungeonMap API and copy-only shims
- C292 - high bug - P0 batch 77 - Level-runtime reset is hand-copied into ~7 paths; active blasts, smashed props and DoTs survive level change, new game and load
- C293 - medium bug - P0 batch 77 - m_fixtureBreaks carries broken/burning state across levels by coordinates
- C294 - low bug - P0 batch 78 - Throw cooldowns, kindle clock and rest's AI mode survive resets
- C295 - medium bug - P0 batch 78 - Rest carries on into a loaded game
- C296 - high bug - P0 batch 79 - The cursor's held item survives Start New Game and eval reset
- C297 - medium bug - P0 batch 78 - Undo history survives a same-level new game, load or eval reset
- C298 - medium bug - P0 batch 80 - stashCurrent=false transitions discard unsaved editor edits on the active level
- C299 - high bug - P0 batch 84 - After leaving a random encounter CurrentLevel stays ~encounter and every world-map save is refused
- C300 - medium bug - P0 batch 12 - ResetForEval recycles the current level, not evalLevel, and asserts after an encounter
- C301 - high bug - P0 batch 86 - Model file extension hard-coded per kind loader: editor-imported items/decorations abort the game
- C302 - medium bug - P0 batch 87 - One kind cache keyed by bare id serves four catalogs (portcullis collides); ReloadTypeKind evicts items from the wrong cache
- C303 - medium bug - P9 - Catalog and monster field defaults restated by schema, MonsterKindFor, item kinds and Threat, and they disagree
- C304 - medium bug - P9 - Rename/delete reference sweep is a closed hand list missing light/trail/door-part/flag refs
- C305 - medium bug - P0 batch 72 - Surface features are invisible to the type rename/delete sweep
- C306 - medium bug - P0 batch 72 - Effects: delete refused as class-backed but rename allowed, dropping tuning
- C307 - medium rule - P0 batch 80 - Never stash to read: StairsInto, RenameLevel, MapOf/PaletteDonor stash levels they only read; read lookup duplicated
- C308 - low bug - P0 batch 80 - Leaving a level stashes its .map unconditionally, so savemap rewrites visited levels
- C309 - medium structure - P9 - Live and *Remote editing APIs re-implement every rule and have drifted
- C310 - high bug - P0 batch 82 - Bore (window) brush on a browsed level writes to the active level
- C311 - high bug - P0 batch 81 - RespawnFromRecords rebuilds the level from stale records: type saves and undo drop or resurrect unsaved work
- C312 - medium structure - P9 - Level-format writers live in DungeonWorld while parsers live in DungeonMap/Entity
- C313 - medium cleanup - P9 - Active and stashed level writers build records separately (decoration facing token, button spawn copies)
- C314 - medium cleanup - P9 - Level .map/.ent text hand-assembled outside the serializer in ~5 places
- C315 - medium cleanup - P9 - Bool, colour and other value grammars reimplemented per reader and disagree (GetBool reads 'no' as true)
- C316 - low cleanup - P9 - List and resists splitters hand-rolled in several TUs; cures splits differently from on_hit
- C317 - medium structure - P9 - Level-file syntax errors abort the process, including from read-only scans and writer self-checks
- C318 - medium cleanup - P9 - .map record parser copy-pastes token helpers; number-or-assert re-implemented per record type
- C319 - medium structure - P9 - Catalog categories are registered in several parallel hand-maintained lists
- C320 - medium cleanup - P5 5b - Member effects use a separate save conversion and line that drop tint and source
- C321 - medium cleanup - P11 - Save reader keeps pre-reset migration shims and docs cite the retired version ladder
- C322 - medium structure - P9 - settings.ini scalars hand-listed twice with unanchored key search; slider ranges stated twice
- C323 - low bug - P0 batch 72 - Catalog comment fidelity holes (trailing comments dropped, monster config rewrite)
- C324 - medium cleanup - P5 5f - Breakable profile parsed and seeded in several places with mismatched defaults
- C325 - low cleanup - P9 - No CatalogFloat helper; stair fields re-read at ~15 sites
- C326 - high bug - P0 batch 81 - Level save writes monsters at their roaming square, not their spawn
- C327 - medium bug - P0 batch 81 - DungeonEntities::Add can reuse a removed record's id
- C328 - low cleanup - P11 - Per-surface heights/factors/setters triplicated and Palette(Surface) re-derived with ternaries
- C329 - low cleanup - P9 - CatalogSchema surface tables and light row copy-pasted
- C330 - medium bug - P0 batch 87 - Item catalog fields parsed twice (ItemCategoryBank vs ItemKind); wearByType never cleared on world switch
- C331 - medium bug - P0 batch 85 - ParseTags lowercases case-sensitive id lists (dungeon levels, quest stages)
- C332 - medium bug - P0 batch 85 - Regenerating a level drops stair flag gates, atmosphere and uistone
- C333 - high bug - P0 batch 84 - Exit stair hard-coded as 'stairs_exit' (and placement written four ways)
- C334 - high bug - P0 batch 84 - The generator hard-codes 'wooden_door'; renaming that type aborts on load
- C335 - medium cleanup - P9 - InstallLevelText round-trips through fixed %TEMP% files
- C336 - medium bug - P0 batch 88 - SyncProjectToSource never copies an import's worn meshes
- C337 - low cleanup - P9 - Which catalogs count as items differs per site
- C338 - medium cleanup - P11 - The 'always someone' rule and other generator blocks copied
- C339 - medium cleanup - P11 - Run and Populate duplicate density/count formulas and place lambda
- C340 - medium cleanup - P2 - The 2x2 room rule is coded three times
- C341 - low cleanup - P11 - Style fields and theme members read ad hoc
- C342 - low cleanup - P11 - SplitKnobs re-tokenises Encode's output
- C343 - medium bug - P0 batch 89 - Setting one entry coordinate of a location makes a world that aborts on read-back
- C344 - high bug - P0 batch 82 - Opening a wall on a browsed level leaves wall decorations hanging, and the parser asserts
- C345 - high bug - P0 batch 89 - Terrain glyph edits and deletes brick the world (WorldMap::Load asserts)
- C346 - medium bug - P0 batch 88 - Surface type editor writes texture before the worn-mesh bake succeeds
- C347 - medium bug - P0 batch 87 - Two definitions of 'is a rune' (category+symbol vs id prefix)
- C348 - low bug - P0 batch 83 - A start square can be cropped by a resize and the writer emits no P
- C349 - low bug - P0 batch 52 - ReadSave does not bound member/pack indices
- C350 - low cleanup - P11 - Undo snapshots copy every stashed level per step
- C351 - high bug - P0 batch 82 - Item-into-niche placement is unreachable since the placement gate
- C352 - medium bug - P0 batch 83 - Armed brush is a row index re-resolved by rebuilding the palette at every use
- C353 - medium bug - P0 batch 83 - Middle-click erase on an empty square commits an empty undo step and clears redo
- C354 - low cleanup - P9 - Fixtures' mount re-parsed at 7 sites instead of the cached kind
- C355 - medium bug - P0 batch 81 - Damage ledger pairs values by address; deleting a monster in the live editor raises a false violation
- C356 - low bug - P0 batch 31 - Door inspector writes open directly (can shut a smashed door or close on a monster)
- C357 - low bug - P0 batch 31 - Door toggle guard checks monsters but not the party
- C358 - low cleanup - P9 - NicheItemPos hard-codes a catalog id and baker geometry
- C359 - low bug - P0 batch 63 - Floor-item pick height uses model units; GroundOffsetY dead
- C360 - low bug - P0 batch 58 - materials[0] indexed without a guard on single-mesh kind paths
- C361 - low bug - P0 batch 30 - resource::Rules{} / PoolRules{} are not inert defaults
- C362 - low bug - P0 batch 30 - Curve-form knobs cast to CurveForm with no range check
- C363 - low cleanup - P11 - PartyRules keeps its own stat count/order with no static_assert
- C364 - low bug - P0 batch 52 - A new game that needs a level load loses its intro log lines
- C365 - medium bug - P0 batch 73 - Pause or sheet from the world map draws the parked dungeon behind it
- C366 - high bug - P0 batch 52 - Title menu never gains Continue/Load after a first save (shared has-saves flag)
- C367 - low bug - P0 batch 52 - A failed save-slot delete is silent
- C368 - low cleanup - P11 - SoundBank count repeated as a literal
- C369 - low cleanup - drop - WorldState::seen searched linearly per rendered cell
- C370 - high bug - P0 batch 51 - RebuildForLanguage destroys the party page widgets while it stays shown (dangling pointers)
- C371 - medium bug - P0 batch 42 - Item, spell and effect descriptions cut at 255 bytes (mid-UTF-8)
- C372 - medium bug - P0 batch 54 - Armor tooltip dropped the avoidance-skill term
- C373 - high bug - P0 batch 56 - Editor wheel zoom is not cursor-anchored
- C374 - low bug - P0 batch 56 - Toolbar hover and tooltip hang under the dialog a toolbar button opens
- C375 - medium cleanup - P2 - Map chrome implemented twice (MapView vs WorldMapView) and drifted
- C376 - low cleanup - P0 batch 56 - Player map VariantTint reads the raw variant, against the one-resolver rule
- C377 - low cleanup - P11 - Browse-snapshot refresh open-coded six times, re-copying the level every drag frame
- C378 - low cleanup - P11 - Map inks scattered as literals outside MapColors.h
- C379 - medium bug - P0 batch 57 - Dev-console THREADS/HEALTH hit rects stay clickable after scrolling away
- C380 - low bug - P0 batch 57 - Clicking a health mark reports the window's newest event
- C381 - low bug - P0 batch 57 - HealthRow::prev hard-coded to 6 while indexed up to kKindCount
- C382 - low rule - P0 batch 48 - Raw pixel padding in ContextMenu and MenuList against the rem rule
- C383 - high bug - P0 batch 46 - Typed text is truncated UTF-16, not UTF-8: non-ASCII becomes garbage or Enter/Backspace
- C384 - low bug - P0 batch 47 - Path strings mix encodings (ANSI Paths and file opens vs UTF-8)
- C385 - medium bug - P0 batch 37 - diag::Record logs synchronously, so crash handlers allocate and lock before the minidump
- C386 - medium bug - P0 batch 36 - ThreadManager Get/Reap can free a Worker the supervisor is reading or restarting
- C387 - medium cleanup - P0 batch 36 - Stalls and kills are recorded with no stack though WalkThread exists
- C388 - medium bug - P0 batch 37 - Stack overflow listed as covered, but the fault filter runs on the exhausted stack
- C389 - low cleanup - P0 batch 9 - Diagnostics ResetEntry misses throttle fields; per-thread registries could share one table
- C390 - low cleanup - P2 - The supervisor thread is unmanaged and allocates every 100 ms
- C391 - medium bug - P0 batch 16 - -headless still shows the window (SetWindowed always shows; Borderless/Exclusive take a monitor)
- C392 - medium bug - P0 batch 16 - Alt+F4 is swallowed and borderless modes have no close button
- C393 - low bug - P0 batch 88 - The flip-green checkbox can only force the flip on
- C394 - medium bug - P0 batch 58 - OBJ loader drops normals for v//n faces
- C395 - low structure - P0 batch 61 - Animator contracts held by convention (opt-in root lock, mid-fade Play)
- C396 - low bug - P0 batch 58 - Embedded-image sidecars keyed by decode-success order
- C397 - low cleanup - P8 8d - Model loading failure policy inconsistent and mis-documented
- C398 - low bug - P0 batch 68 - RestartApp drops -project, quits on failed relaunch and lets the child truncate the log
- C399 - medium structure - P11 - ModelBaker.cpp is 2,677 lines with tombstones and triplicated builders
- C400 - medium structure - P2 - RollTest main() is one ~2,500-line function
- C401 - medium cleanup - P0 batch 1 - Harnesses copy launch / input / wait code and the copies drift
- C402 - medium bug - P0 batch 94 - FetchTextures.ps1 reads the dead level1.map, so a fresh clone fetches 2 of 12 sets
- C403 - low cleanup - P11 - Level-generator scripts target removed levels; unused icons and stale tool comments
- C404 - low cleanup - P0 batch 95 - Blender build scripts carry drifted private helper copies
- C405 - low cleanup - P2 - Pipeline plumbing (archive root, Blender discovery) copied per script
- C406 - medium bug - P0 batch 90 - Worn-block bake parameters have four disagreeing authorities
- C407 - medium bug - P0 batch 91 - Per-type wear/relief baked into per-set files, so a second type re-bakes the first
- C408 - low bug - P0 batch 90 - Height-map resolution fallback can discard the texture's aspect
- C409 - medium bug - P0 batch 90 - wear/relief only reach the height-map term
- C410 - medium bug - P0 batch 92 - runes writes only PNGs while the loader prefers a stale .dds
- C411 - medium cleanup - P11 - TextureBaker.cpp is entirely dead
- C412 - low cleanup - P2 - Baker image and mesh helpers re-implemented per file
- C413 - low cleanup - P11 - Unreferenced bake outputs; the 'unused' clean wall block is the editor swatch
- C414 - low bug - P0 batch 92 - Mip chains box-filtered in gamma space with truncating division
- C415 - low cleanup - P11 - Stale baker comments
- C416 - low rule - P0 batch 93 - GltfWriter raw FILE* with unchecked writes; dropped errors
- C417 - high bug - P0 batch 7 - Bc7Test's baseline loader reads nothing, so the regression gate never fires
- C418 - medium bug - P0 batch 6 - ThreadStress never sets targets, so every monster paths to a wall
- C419 - medium bug - P0 batch 5 - Self-test verdicts accept any failure (RollTest, ProfileTest, InGameTest, HealthTest)
- C420 - medium bug - P0 batch 9 - DiagTest log checks pass when the log is unreadable
- C421 - low cleanup - P2 - AnimTest samples no looping wrap or root-locked playback
- C422 - low cleanup - P0 batch 7 - Bc7Test audit labels and GPU-proof claims are stale
- C423 - low cleanup - P0 batch 5a - RollTest restates Balance defaults by hand
- C424 - low bug - P0 batch 5 - RollTest indexes parser results without size checks
- C425 - low cleanup - P0 batch 5a - Verdict lines don't follow the shared convention; three Check helpers
- C426 - medium bug - P0 batch 3 - No harness detects a stale exe; -Only never builds
- C427 - medium bug - P0 batch 8 - InGameTest's coverage label shows the command ran, not that the screen opened
- C428 - low bug - P0 batch 1 - Console-ready retry loops satisfied by an earlier echo
- C429 - medium bug - P0 batch 1 - HealthTest waits on 'Game loaded:', too early
- C430 - medium bug - P0 batch 2 - Eval.ps1 and Python judges allow a second instance in one worktree and ignore exit codes
- C431 - medium bug - P0 batch 4 - EditorTest and WorldTest modify the real project and restore non-atomically
- C432 - low bug - P0 batch 4 - Tautological and vacuous checks in WorldTest/LevelBuildTest
- C433 - low cleanup - P0 batch 2 - Muting and -Config only follow CheckAll's own config
- C434 - low bug - P0 batch 94 - ConvertMesh --keep-rig de-dup reads removed Action.fcurves
- C435 - medium bug - P0 batch 95 - BuildWallArch's slab has T-junctions and no closed-shell assert
- C436 - low bug - P0 batch 95 - atan2 UV seam reverses a face column on fountain, cork and rock
- C437 - medium rule - P0 batch 92 - Model-regeneration scripts never bake embedded-image sidecars
- C438 - low cleanup - P0 batch 90 - ReplayImports.ps1 drifts from the baker discipline
- C439 - low cleanup - P0 batch 94 - BuildTemplate.py hard-codes stale start_items
- C440 - low cleanup - P11 - Build scripts name their metre-to-unit factor KUNIT
- C441 - low bug - P0 batch 13 - levelcheck checks only the model field, by stem
- C442 - medium bug - P0 batch 10 - Declined commands Print instead of Refuse, so eval scripts never count them
- C443 - low bug - P0 batch 11 - StepWorld keeps ticking after a mid-step wipe
- C444 - medium bug - P0 batch 11 - rest until ignores StepStop and mislabels stops
- C445 - low bug - P0 batch 11 - An unreadable script mid-batch re-emits the previous verdict
- C446 - low bug - P0 batch 10 - setskill accepts any skill id
- C447 - low bug - P0 batch 11 - Console throw drops the held item's charge
- C448 - low bug - P0 batch 96 - hudpanel writes settings directly, bypassing GameUI
- C449 - low bug - P0 batch 96 - editor place default-face scan ignores taken faces
- C450 - low bug - P0 batch 96 - allocguard's armed line never prints; 120 duplicates kWarmupFrames
- C451 - medium cleanup - P0 batch 19 - Stale effect-pipeline comments and design-doc status lines
- C452 - low cleanup - P5 5b - Stale projectile comments and docs
- C453 - low cleanup - P5 5f - combat.md's overkill-on-a-downed-member route never happens
- C454 - medium cleanup - P0 batch 6 - docs/ai.md and AI header comments describe AI that no longer exists
- C455 - medium cleanup - P0 batch 18 - Dialog docs and CLAUDE.md name deleted chrome helpers
- C456 - medium cleanup - P0 batch 18 - CLAUDE.md settings-page section describes the removed Flow helper
- C457 - low cleanup - P0 batch 18 - CLAUDE.md HUD, UIContext and Player-map paragraphs describe removed UI
- C458 - medium cleanup - P1 - ARCHITECTURE.md module table, rules and frame flow are out of date
- C459 - low cleanup - P11 - DIAGRAMS.md describes the pre-world-on-demand app
- C460 - low cleanup - P0 batch 17 - Damage types are a catalog of 8; docs say seven
- C461 - low cleanup - P9 - C++ names damage types the catalog header says it never names
- C462 - low cleanup - P0 batch 17 - Features catalog is surfacefeatures.cat
- C463 - low cleanup - P0 batch 17 - AssetBaker full-bake doc is wrong
- C464 - medium cleanup - P5 5d - Comments still describe the deleted vit_exertion creep
- C465 - low cleanup - P11 - Stale DungeonWorld/Game role docs and orphaned comments
- C466 - medium cleanup - P0 batch 20 - Game.h/GameUI.h/Game.cpp comments contradict rules (relaunch-era world switch)
- C467 - low cleanup - P11 - Misplaced and crossed comments left by file splits
- C468 - low cleanup - P11 - Phase-era comments in Core
- C469 - low cleanup - P11 - PerfMonitor comments describe the pre-worker design
- C470 - low cleanup - P5 5c - Stale combat comments
- C471 - low cleanup - P0 batch 64 - Stale slice-loading comments
- C472 - low cleanup - P9 - Stale data-model comments; DungeonMap 'static layer' banner
- C473 - medium cleanup - P0 batch 17 - CLAUDE.md says facing +1 is on-screen left; code says right
- C474 - low cleanup - P11 - Stale character comments
- C475 - low cleanup - P2 - Stale dev command banners and arena params
- C476 - low cleanup - P1 - Stale console comments
- C477 - low cleanup - P0 batch 54 - Stale HUD comments
- C478 - low cleanup - P11 - Stale map view / editor comments
- C479 - low cleanup - P7 - Stale dialog comments
- C480 - low cleanup - P0 batch 17 - CLAUDE.md MSVC debug vector noexcept claim is wrong
- C481 - low cleanup - P0 batch 14 - check-* skill files describe old harnesses
- C482 - low cleanup - P0 batch 20 - Script usage lines encode the -File comma trap
