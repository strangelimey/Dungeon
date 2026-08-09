# Sound sources

The decision record for what gets imported and from where. Pairs with the
`$soundSets` table in `tools\FetchSounds.ps1` — this file is where a source is
argued for, that table is where it becomes an asset.

## Sonniss GDC 2026 Game Audio Bundle

Free, royalty-free for commercial use, no attribution, no per-project limit.
Five zips, 6.45 GB, in `OneDrive\DungeonAssets\audio\sonniss-gdc2026\`.

**347 WAVs. 161 are dungeon material; 186 are not** — vehicles, crowds,
aircraft, trains, comms, sci-fi, cartoon. It is a general game-audio bundle,
not a fantasy one.

### How the tables below were built

Every filename carries a **UCS (Universal Category System) CatID** prefix —
`WEAPSwrd_`, `AMBDsgn_`, `FIRECrkl_` — so the sorting is the industry taxonomy
rather than a guess at what the words mean. That makes it far more reliable
than keyword matching, but it is still a ROUGH PASS, and it inherits UCS's
blind spot: a CatID describes the *sound*, not the *use*. A sword swing
recorded as metal-on-metal friction files as `METLFric` and lands under Doors,
which is where the ear has to take over. Move rows freely.

### What these files ARE, which is not what "sound pack" suggests

They are **source recordings at sound-design resolution**, not game-ready
assets:

| | |
|---|---|
| Sample rate | 96 kHz (280 files), 192 kHz (40), 48 kHz (27) |
| Channels | stereo (303), mono (39), **multichannel (5)** |
| Depth | 24-bit |
| Length | up to ~3 min; the 199 MB files are the bundle's per-file cap |

Two consequences worth holding before any listening:

- **Nothing here is a loop.** A Phase 4 bed has to be *cut* from a three-minute
  recording at a point where it will seam — which is exactly what
  `import-sound --loop` measures and complains about. Expect to iterate.
- **96/192 kHz is deliberate**, so designers can pitch material down without
  artefacts. The importer resamples to 44.1 kHz with a windowed sinc whose
  cutoff follows the lower rate, so a 4:1 downsample band-limits properly
  instead of aliasing — but it is doing real work on every import.

### THE GAP: there are no footsteps

Phase 5 is footwear x surface, and this bundle contains **no usable footstep
material at all**. Searching foot/step/walk/boot/gravel returns a barber
chair's foot pump, a metro-station ambience with footsteps buried in the mix,
and tyre skids. Phase 5 needs a dedicated Foley source — a purchase, or a
recording session.

### The multichannel trap

Five files are wider than stereo, and two would be **silently corrupted** by a
default import:

| Ch | File |
|---|---|
| 4 | `Camp fire, crackling... _B-format, Ambix` — **ambisonic** |
| 4 | `WATRLap_Small Waves sloshing onto Rock Slabs... _Quad` |
| 6 | `CRWDCheer_Small Club... Surround` |
| 6 | `AMBUrbn_Ambience, City, Madrid... Surround` |
| 10 | `CRWDWalla_Dinner Party Crowd... Atmos` |

Ambisonic B-format is not four microphones. It is W (omnidirectional) plus
X/Y/Z (directional gradients), and averaging the four is **not a downmix**: the
gradients are signed and sum toward nothing, leaving a quieter, hollower W with
the gradient noise stirred in. The correct mono reduction is **W alone**.

**Measured on the campfire**, importing the same file both ways:

| Fold | Peak before normalize |
|---|---|
| Naive 4-channel average | **−15.0 dBFS** |
| W alone (correct) | **−1.9 dBFS** |

**13.1 dB of cancellation** — an amplitude factor of about 4.5. And the damage
is worse than the level suggests, because normalization then pumps the wreckage
back up by +14 dB and lifts the residual gradient noise with it, so what is lost
is signal-to-noise, not just gain. The trim length gives the same story from
another angle: the cancelled version sat below the silence floor for 2444 ms
against the real one's 1366 ms.

`import-sound` now detects this from the filename (`b-format`, `ambix`,
`ambisonic`, `fuma`) on any 4-channel source, the same way the texture importer
spots an OpenGL normal map — the convention is in the name because it is nowhere
in the data. `--ambisonic` forces it. Asking for `--stereo` on a B-format file
is refused outright: W/X/Y/Z are not speaker feeds and nothing downstream could
rescue that.

5.1 sources fold by the ITU rule (front pair at unity, centre and surrounds at
−3 dB) with **LFE discarded** — the LFE is a separate band-limited effects
channel already around 10 dB hot, not the bass of the mix, so averaging it in
drops a low-frequency lump over everything.


## Ambience beds — 29 files, 591 MB

Phase 4 — the level bed, and the most valuable category in the bundle.

| MB | ch | kHz | File | Library |
|---:|---:|---:|---|---|
| 199 | 2 | 192 | AMBDsgn_Evil Spell Ambience_344 Audio_Ghostly Presences | 344 Audio - Ghostly Presences Vol. 1 |
| 48 | 2 | 96 | AMBSwmp_Meadow Pipits calling many Insects humming Wind blowing through Grass_JSE_HoN_Stereo | Just Sound Effects - Highlands of Norway |
| 39 | 2 | 96 | 04 Church Bells, Near Distance, In Church Tower-3 Different Bell 02 | Ivo Vicic - Church Bells |
| 38 | 2 | 96 | AMBInd_Factory Hall Busy Alarm Machines Voices_CW | Victor Ermakov - Industrial Ambiences - Ship Repair Factory |
| 33 | 2 | 96 | DSGNSynth_Scifi Loop Ship Reactor Synth Layer Layered Saw Buzz_ESM_SGA3 | Epic Stock Media - Strange Game Ambient Loops 3 |
| 33 | 2 | 96 | DSGNSynth_Dark Loop Mystic Forest Tonal Steady Synth_ESM_SGA3 | Epic Stock Media - Strange Game Ambient Loops 3 |
| 33 | 2 | 96 | AMBRoom_Factory Loop Heavy Machinery Tonal Roomtone Dark Wind Vent_ESM_SGA3 | Epic Stock Media - Strange Game Ambient Loops 3 |
| 25 | 2 | 48 | 25 Church Bells, Far Distance, Rural Soundscape Distant, Hills Ridge, Spring 02 | Ivo Vicic - Church Bells |
| 23 | 2 | 192 | DSGNTonl_Designed Metal Bowed Screech Tonal Reverb 7_The Noisery_Moaning Metal | The Noisery - Moaning Metal |
| 17 | 2 | 96 | Dark Industrial Ambience | Cinematic Sound Design - Sci-Fi Drones |
| 13 | 2 | 96 | DSGNBram____Cinematic Horn Braam, Epic, Cinematic, Dark, Instrument, Huge-32 | Jake Fielding - Cinematic Horn Braams |
| 11 | 2 | 96 | DSGNBram____Cinematic Horn Braam, Epic, Cinematic, Dark, Instrument, Huge-67 | Jake Fielding - Cinematic Horn Braams |
| 11 | 2 | 96 | CREAEthr_Aztec Death Whistle Drone_05_IMA_Death Whistle Samples | InMotionAudio - The Death Whistle |
| 9 | 2 | 192 | DSGNBass_Bass Drop & Downer Slow 10_344 Audio_Bass Drops & Downers | 344 Audio - Bass Drops & Downers Vol. 1 |
| 8 | 2 | 96 | DSGNErie_NoiseBoxHit_10_InMotionAudio_SinisterTextures4 | InMotionAudio - Sinister Textures 4 |
| 8 | 2 | 96 | DSGNErie_NoiseBoxHit_36_InMotionAudio_SinisterTextures4 | InMotionAudio - Sinister Textures 4 |
| 6 | 2 | 192 | DSGNBass_Tone Downer (Reverb)_344 Audio_Bass Drops and Downers Vol 3 | 344 Audio - Bass Drops & Downers Vol. 3 |
| 6 | 2 | 192 | DSGNBass_Rattling Downer 3_344 Audio_Bass Drops and Downers Vol 3 | 344 Audio - Bass Drops & Downers Vol. 3 |
| 5 | 2 | 192 | DSGNBass_Bass Drop & Downer Slow 07_344 Audio_Bass Drops & Downers Vol 2 | 344 Audio - Bass Drops & Downers Vol. 2 |
| 5 | 2 | 192 | DSGNBass_Bass Drop & Downer Medium 04_344 Audio_Bass Drops & Downers | 344 Audio - Bass Drops & Downers Vol. 1 |
| 4 | 2 | 192 | DSGNBass_Jump Start Drop 3_344 Audio_Bass Drops and Downers Vol 3 | 344 Audio - Bass Drops & Downers Vol. 3 |
| 4 | 2 | 192 | DSGNBass_Bass Drop & Downer Medium 15_344 Audio_Bass Drops & Downers Vol 2 | 344 Audio - Bass Drops & Downers Vol. 2 |
| 4 | 1 | 192 | DSGNTonl_Metal Scrape Low Tonal LFE 4_The Noisery_Moaning Metal | The Noisery - Moaning Metal |
| 3 | 2 | 192 | DSGNBass_Bass Drop & Downer Fast 12_344 Audio_Bass Drops & Downers Vol 2 | 344 Audio - Bass Drops & Downers Vol. 2 |
| 2 | 2 | 192 | DSGNBass_Bass Drop & Downer Fast 16_344 Audio_Bass Drops & Downers | 344 Audio - Bass Drops & Downers Vol. 1 |
| 2 | 2 | 96 | DSGNEthr_Jumpscare Vocal Aggressive Whisper Harsh Distortion 02_ESM_HALG | Epic Stock Media - Halloween Game - Haunted House and Horror Audio Scare Kit |
| 1 | 2 | 96 | CREAEthr_Aztec Death Whistle Distortion_02_IMA_Death Whistle Samples | InMotionAudio - The Death Whistle |
| 1 | 2 | 96 | CREAEthr_Aztec Death Whistle DryPitch12_IMA_Death Whistle Samples | InMotionAudio - The Death Whistle |
| 0 | 1 | 96 | CREAEthr_Ethereal Entity Grim Pain Long 4_SNDBTS_VB-SE | SoundBits - Vox Bestiae - Source Elements |


## Wind — 6 files, 387 MB

Phase 4 — moaning through tunnels. `WINDInt` (interior) is the one to hear first.

| MB | ch | kHz | File | Library |
|---:|---:|---:|---|---|
| 199 | 2 | 192 | WINDDsgn_EXT, Eye Of The Storm_344 Audio_Extreme Winds Vol 1 | 344 Audio - Extreme Winds Vol. 1 |
| 90 | 2 | 96 | WINDTonl_Wind Strong Gusts Hurricane Vents Rattle 06_The Noisery_City Rain | The Noisery - City Rain |
| 64 | 2 | 96 | WINDInt_ChimneyWind05_InMotionAudio_ChimneyWind | InMotionAudio - Chimney Wind |
| 30 | 2 | 96 | WINDInt_Wind Strong Metal Rattle_The Noisery_City Rain | The Noisery - City Rain |
| 4 | 2 | 96 | WINDInt_Loop Weather Wind Whipping Constricted Flow Turbulent 01_ESM_SNLS | Epic Stock Media - Synthesized Nature Loops and Sounds |
| 1 | 2 | 96 | WINDDsgn_Wind, Rush, Whoosh, Long x5 01_344 Audio_Elemental Palette Designed Vol 1 | 344 Audio - Elemental Palette Designed Vol. 1 |


## Water — 7 files, 413 MB

Phase 4 — dripping and running. Short drips have to be cut out of long recordings.

| MB | ch | kHz | File | Library |
|---:|---:|---:|---|---|
| 139 | 2 | 96 | WATRWave_Soft Waves Cliffs_JSE_RCoN_Stereo | Just Sound Effects - Rocky Coast of Norway |
| 108 | 2 | 96 | WATRWave_Medium Waves at Pebble Beach_JSE_RCoN_Stereo | Just Sound Effects - Rocky Coast of Norway |
| 102 | 4 **!** | 96 | WATRLap_Small Waves sloshing onto Rock Slabs at huge Glacial Lake _JSE_HoN_Quad | Just Sound Effects - Highlands of Norway |
| 57 | 2 | 96 | WATRLap_Summer Tennessee Lake Dock Water Ripples Wake Wave Gentle 05 Distant_ESM_CPS | Epic Stock Media - Public Spaces - Storms Lakes Parks and Rural Nature Exteriors |
| 5 | 2 | 96 | WATRPlmb_ToiletFlush01_InMotionAudio_USAHotel | InMotionAudio - USA Hotel |
| 2 | 2 | 96 | WATRImpt_Impact Water Deep Submerge Bubble Drown Ship Hit 05_ESM_EMWI | Epic Stock Media - Elemental Mutation Whooshes and Impacts |
| 1 | 2 | 96 | WATRMisc_Water, Liquid Impact, Bubble, Sci Fi, Hit 04_344 Audio_Elemental Palette Designed Vol 1 | 344 Audio - Elemental Palette Designed Vol. 1 |


## Fire — 6 files, 372 MB

Phase 4 — sconce and brazier loops, positional per fixture.

| MB | ch | kHz | File | Library |
|---:|---:|---:|---|---|
| 199 | 2 | 192 | FIRECrkl_Fire Crackling, Popping, Witch's Cauldron_344 Audio_Haunting Ambiences Vol 5 | 344 Audio - Haunting Ambiences Vol. 5 |
| 84 | 4 **!** | 96 | Camp fire, crackling, putting oil in fire, close, night_B-format, Ambix | Ivo Vicic - Campfire FX - Ambisonic |
| 66 | 2 | 96 | 24 Campfire, Dropping Fresh Pine Branches in Fire, Crackling, Sizzling Strong, Close 02 | Ivo Vicic - Campfire - Bonfire FX |
| 17 | 2 | 96 | 42 Campfire, Putting Out Fire, Water from Bottle, Variation, Close | Ivo Vicic - Campfire - Bonfire FX |
| 4 | 1 | 96 | FIREBurn_Loop Elements Fire Crackling Crunchy Flame Burn 03_ESM_SNLS | Epic Stock Media - Synthesized Nature Loops and Sounds |
| 2 | 2 | 96 | FIREWhsh_Whoosh Fire Deep Growl Monster Saturated Crisp 03_ESM_EMWI | Epic Stock Media - Elemental Mutation Whooshes and Impacts |


## Doors & mechanisms — 5 files, 49 MB

Doors shipped silent; this is what closes that debt.

| MB | ch | kHz | File | Library |
|---:|---:|---:|---|---|
| 37 | 1 | 192 | METLFric_Large Metal Box, Drag, Geofon_344 Audio_Extreme Winds Vol 1 | 344 Audio - Extreme Winds Vol. 1 |
| 5 | 2 | 96 | METLFric_SWING SCRAPE Swift Melee Weapon Swing With A Long Blade 14_DDUMAIS_MWP2 | David Dumais Audio - Melee Weapons Sound Effects Pack 2 |
| 4 | 2 | 96 | DOORMetl_StairWellDoor01_InMotionAudio_USAHotel | InMotionAudio - USA Hotel |
| 3 | 2 | 96 | WOODFric_Wood Shaker Microphone Head Roll Table Alternate Grainy 05_ESM_HDGM | Epic Stock Media - HD Game Materials |
| 1 | 2 | 192 | MECHLtch_Click Deep Mechanism Latch Button Nearfield Thunk 02_ESM_HDLM | Epic Stock Media - HD Lock And Mechanism Sound Design Kit |


## Impacts — 12 files, 433 MB

Phase 6 defender side, plus the wall bump and the pit landing.

| MB | ch | kHz | File | Library |
|---:|---:|---:|---|---|
| 199 | 2 | 192 | METLImpt_Metal Bangs, Metal Hits, Banging On Doors_344 Audio_Haunting Ambiences Vol 3 | 344 Audio - Haunting Ambiences Vol. 3 |
| 199 | 2 | 192 | WOODImpt_Wooden Hit, Dark, Heavy Hit, Vampire's Prison_344 Audio_Haunting Ambiences Vol 3 | 344 Audio - Haunting Ambiences Vol. 3 |
| 13 | 2 | 192 | METLTonl_Item Spring Wire Impact Flick Top Clatter Light Tap Roll Handling Short 01_ESM_HDLM | Epic Stock Media - HD Lock And Mechanism Sound Design Kit |
| 6 | 2 | 96 | METLImpt_METAL SWING HIT Weapon Swing To Metallic Body Impact And Resonant Tail 01_DDUMAIS_MWP2 | David Dumais Audio - Melee Weapons Sound Effects Pack 2 |
| 4 | 2 | 96 | OBJFurn_Barber Chair, Foot Pump 4_344 Audio_Barbershop Vol 1 | 344 Audio - Barbershop Vol. 1 |
| 3 | 2 | 96 | WOODImpt_Hit Blood Spill Splat Wood Impact Light Hit Squelch Small Thump 03_ESM_TDG | Epic Stock Media - Tower Defense Game |
| 3 | 1 | 192 | DSGNImpt_Metal Hit Thud Thump Low Ring Geofon 1_The Noisery_Moaning Metal | The Noisery - Moaning Metal |
| 2 | 2 | 96 | METLMvmt_  Tinkering Antique Lock_344 Audio_Antique Small Metals | 344 Audio - Antique Small Metals |
| 2 | 2 | 96 | GLASMvmt_Whoosh Glass Crystal Fragments Sharp Shards Dry 05_ESM_EMWI | Epic Stock Media - Elemental Mutation Whooshes and Impacts |
| 1 | 2 | 96 | METLMvmt_  Opening Lid Of Antique Blowtorch_344 Audio_Antique Small Metals | 344 Audio - Antique Small Metals |
| 1 | 2 | 96 | METLImpt_Metal Old File Impact Tap Against Tire Iron Metallic Hit 01_ESM_HDGM | Epic Stock Media - HD Game Materials |
| 0 | 2 | 96 | METLMvmt_  Antique Measuring Tape_344 Audio_Antique Small Metals | 344 Audio - Antique Small Metals |


## Weapons & combat — 12 files, 90 MB

Phase 6 attacker side. The Medieval Weapons library is the direct hit.

| MB | ch | kHz | File | Library |
|---:|---:|---:|---|---|
| 40 | 2 | 192 | WEAPArmr_Metal Shield Spin On Floor, Buckler MKH_344 Audio_Medieval Weapons Vol 2 | 344 Audio - Historical Weapons Vol. 2 |
| 14 | 2 | 192 | WEAPSwrd_Sword Slide Cuts, Metallic, Impact CM4 2_344 Audio_Medieval Weapons Vol 2 | 344 Audio - Historical Weapons Vol. 2 |
| 12 | 2 | 192 | WEAPBlnt_Spear And Stick Impact, Wooden MKH 2_344 Audio_Medieval Weapons Vol 2 | 344 Audio - Historical Weapons Vol. 2 |
| 10 | 2 | 192 | FGHTGrab_Choking, Tension 03_344 Audio_Cinematic Fight Vol 1 | 344 Audio - Cinematic Fight Vol. 1 |
| 5 | 2 | 96 | WEAPWhip_WHIP Snap Crack 05_DDUMAIS_MWP2 | David Dumais Audio - Melee Weapons Sound Effects Pack 2 |
| 4 | 2 | 192 | FGHTImpt_4 x Punch, Body 02_344 Audio_Cinematic Fight Vol 1 | 344 Audio - Cinematic Fight Vol. 1 |
| 1 | 2 | 96 | GORESplt_Gore Designed Transient Heavy Impact Smash 01_ESM_HALG | Epic Stock Media - Halloween Game - Haunted House and Horror Audio Scare Kit |
| 1 | 2 | 96 | FGHTImpt_Combat Punch Impact Light Hit Delay Crunchy Vintage Quick Smack 05_ESM_AG | Epic Stock Media - Anime Game |
| 1 | 2 | 96 | GOREMisc_Cladding_Scratch06_InMotionAudio_SinisterTextures5 | InMotionAudio - Sinister Textures 5 |
| 1 | 2 | 96 | Arrow Hit Rattle | Cinematic Sound Design - Cartoon Bloopers |
| 1 | 2 | 96 | GOREMisc_Cladding_NailScratch19_InMotionAudio_SinisterTextures5 | InMotionAudio - Sinister Textures 5 |
| 0 | 2 | 96 | GOREMisc_Concrete_MetalPipe02_InMotionAudio_SinisterTextures5 | InMotionAudio - Sinister Textures 5 |


## Monsters — 13 files, 35 MB

Per-kind vocalisations. Thin, and mostly the wrong animals.

| MB | ch | kHz | File | Library |
|---:|---:|---:|---|---|
| 11 | 2 | 96 | ANMLRept_Dinosaur Eating Meat 01_344 Audio_Dinosaurs Vol 2 | 344 Audio - Dinosaurs Vol. 2 |
| 7 | 2 | 96 | ANMLRept_Large Herbivore Roar 01_344 Audio_Dinosaurs Vol 2 | 344 Audio - Dinosaurs Vol. 2 |
| 3 | 2 | 96 | CREABeast_Creature Werewolf Growl Menacing Monstrous 06_ESM_HALG | Epic Stock Media - Halloween Game - Haunted House and Horror Audio Scare Kit |
| 3 | 2 | 96 | CREAMnstr_Designed Sea Beast Creature Pain Intense Yell Long 04_ESM_HC4 | Epic Stock Media - Humanoid Creatures Vol 4 - Monstrous and Undead Creature Vocalization Sound Sets |
| 3 | 2 | 96 | ANMLRept_T Rex_344 Audio_Dinosaurs | 344 Audio - Dinosaurs Vol. 1 |
| 2 | 2 | 96 | CREAHmn_Designed Orc Male Attack Long Heavy Hit Charged Up 03_ESM_HC4 | Epic Stock Media - Humanoid Creatures Vol 4 - Monstrous and Undead Creature Vocalization Sound Sets |
| 1 | 1 | 96 | CREAAqua_Aquatic Creature Gurgling 2_SNDBTS_VB-SE | SoundBits - Vox Bestiae - Source Elements |
| 1 | 2 | 96 | ANMLRept_Raptor Flair_344 Audio_Dinosaurs | 344 Audio - Dinosaurs Vol. 1 |
| 1 | 1 | 96 | ANMLAmph_Animal Frog Echo Short Squished Frequency Formant 03_ESM_SNLS | Epic Stock Media - Synthesized Nature Loops and Sounds |
| 1 | 2 | 96 | ANMLRept_Hatchling Calling Out_344 Audio_Dinosaurs | 344 Audio - Dinosaurs Vol. 1 |
| 1 | 2 | 96 | ANMLRept_Dinosaur Egg Hatching 02_344 Audio_Dinosaurs Vol 2 | 344 Audio - Dinosaurs Vol. 2 |
| 1 | 1 | 96 | CREAInsc_Insectoid Creature Tremble Attack Long 1_SNDBTS_VB-SE | SoundBits - Vox Bestiae - Source Elements |
| 0 | 1 | 96 | CREAHmn_Violent Humanoid Creature Exhale Short 4_SNDBTS_VB-SE | SoundBits - Vox Bestiae - Source Elements |


## Magic — 22 files, 263 MB

Phase 7 — release, flight and impact for the bolt spells.

| MB | ch | kHz | File | Library |
|---:|---:|---:|---|---|
| 83 | 2 | 48 | CRWDWalla_Crowd, Walla, Movement, 30 People Working Film Crew, Preparing a Movie Set in a Warehouse, Spanish Hum of Voice, Decor Mount, Ladders, Warning Alarm, Light Contruction-Stereo_KSL_KS015 | Sonik Sound Library - Spanish Crowds |
| 46 | 2 | 96 | ice, movement, ice drift, ice field cracking up, initial, wide-001 | Alexander Kopeikin - 100 kHz Designed Ice |
| 34 | 2 | 96 | magic, energy flow, astral travel-015 | Alexander Kopeikin - Emotion and Magic |
| 25 | 2 | 96 | MAGShim_Shimmer Loop Small Bell Metal Taps_ESM_SGA3 | Epic Stock Media - Strange Game Ambient Loops 3 |
| 18 | 2 | 48 | magic, drone, tension, spellbound, evanescence-002 | Alexander Kopeikin - Emotion and Magic |
| 11 | 2 | 48 | magic, action gesture, evil presence, onslaught-004 | Alexander Kopeikin - Emotion and Magic |
| 6 | 2 | 96 | MAGMisc_Sleigh Movement, Pulling_344 Audio_Christmas | 344 Audio - Christmas Vol. 1 |
| 5 | 2 | 96 | ELECBuzz_Buzz27_InMotionAudio_Arc | InMotionAudio - Arc |
| 5 | 2 | 48 | magic, energy flow, astonishment-001 | Alexander Kopeikin - Emotion and Magic |
| 5 | 1 | 96 | MAGMisc_Magic Christmas Bells 2_344 Audio_Christmas | 344 Audio - Christmas Vol. 1 |
| 4 | 2 | 96 | ICEBrk_Skill Freeze Whoosh Break Impact Layered Movement Shatter 03_ESM_TDG | Epic Stock Media - Tower Defense Game |
| 4 | 2 | 96 | ELECArc_ArcPowerUpDesign04_InMotionAudio_Arc | InMotionAudio - Arc |
| 3 | 2 | 96 | MAGAngl_Magic Light Spell Enchantment Potion Effect Tonal Bright 03_ESM_FG2 | Epic Stock Media - Fantasy Game 2 - Sound Kit for Enchanted Realms |
| 3 | 2 | 96 | ice, surface cracking, fissure, fast, hard-003 | Alexander Kopeikin - 100 kHz Designed Ice |
| 3 | 2 | 96 | ICEFric_Dry Ice High Metal Squeal Groan Bright Squeak Dissonant Short 13_ESM_HDGM | Epic Stock Media - HD Game Materials |
| 2 | 2 | 48 | ice, crack, ice block snapping-001 | Alexander Kopeikin - 100 kHz Designed Ice |
| 2 | 2 | 96 | ICEFric_Dry Ice Squeak Metal Animal Mouse Imitation Short 07_ESM_HDGM | Epic Stock Media - HD Game Materials |
| 2 | 1 | 96 | MAGMisc_Wrapping Paper, Opening Present 1_344 Audio_Christmas | 344 Audio - Christmas Vol. 1 |
| 1 | 2 | 96 | EXPLDsgn_Explosion Small Blast Enemy Death Crunchy Boom Cartoon Noisy Crash Impact Delay 03_ESM_AG | Epic Stock Media - Anime Game |
| 1 | 2 | 48 | GAMEMisc_Magic Creation 23_CB Sounddesign_APPlicable Sounds | CB_Sounddesign - Applicable Sounds - Organic UI and Building Games SFX |
| 1 | 2 | 48 | ice, block of ice crushed, heavy-015 | Alexander Kopeikin - 100 kHz Designed Ice |
| 1 | 2 | 96 | ELECArc_ArcDesign15_InMotionAudio_Arc | InMotionAudio - Arc |


## Party voices — 30 files, 254 MB

Grunts, breathing, exertion — the stamina system has nothing to say yet.

| MB | ch | kHz | File | Library |
|---:|---:|---:|---|---|
| 199 | 2 | 192 | HMNBrth_Respirator, Specimen Breathing, Mad Scientist Lab_344 Audio_Haunting Ambiences Vol 5 | 344 Audio - Haunting Ambiences Vol. 5 |
| 10 | 2 | 96 | VOXReac_Spectator, Female, Various Questions_344 Audio_Cinema Experience | 344 Audio - Cinema Experience Vol. 1 |
| 9 | 2 | 96 | VOXMale_Voice Wet Male Emergency Broadcast Announcement Dry War 01_ESM_FA | Epic Stock Media - Fake Advertisements and Radio Sound Effects Audio Construction Kit |
| 4 | 2 | 96 | VOXMale_Police Radio, Burglary, Calm, Update, Warning_344 Audio_British Police Radio | 344 Audio - British Police Radio Vol. 1 |
| 3 | 2 | 96 | VOXMale_Police Radio, Car Theft, Relaxed, Update, Helpful_344 Audio_British Police Radio | 344 Audio - British Police Radio Vol. 1 |
| 3 | 2 | 96 | VOXReac_Construction Kit Male Flutter Death Vocal Stuttered Long 05_ESM_HC4 | Epic Stock Media - Humanoid Creatures Vol 4 - Monstrous and Undead Creature Vocalization Sound Sets |
| 3 | 2 | 96 | VOXMale_Boxer Level Up 1_344 Audio_Anime Fight Voices | 344 Audio - Anime Fight Voices Vol. 1 |
| 3 | 2 | 96 | VOXFem_British Detective Custom Lines Vocal Female In A World Of Lies And Deception Im The One They Turn To When They Need The Truth B_ESM_AAAGCBFD | Epic Stock Media - AAA Game Character British Female Detective |
| 2 | 1 | 96 | HMNBrth_Panting Male 02 04_SNDBTS_VH | SoundBits - Vox Hominis - Human Effort Voices |
| 2 | 2 | 96 | VOXCry_British Detective Crying Vocal Female Low Energy Weep_ESM_AAAGCBFD | Epic Stock Media - AAA Game Character British Female Detective |
| 1 | 2 | 96 | VOXMale_Police Officer Custom Lines Vocal Male Emergency Shots Fired Need Backup | Epic Stock Media - AAA Game Character Police Officer |
| 1 | 2 | 96 | VOXMale_Police, Vehicle, Calm, 'Standing By'_344 Audio_British Police Radio | 344 Audio - British Police Radio Vol. 1 |
| 1 | 2 | 96 | VOXReac_British Detective Pain Vocal Female Dying High Grunt Breath_ESM_AAAGCBFD | Epic Stock Media - AAA Game Character British Female Detective |
| 1 | 2 | 96 | FOLYClth_ClothMovement24_InMotionAudio_FoleyT-Shirt | InMotionAudio - Foley T-Shirt |
| 1 | 1 | 96 | VOXCry_Crying Female 04 05_SNDBTS_VH | SoundBits - Vox Hominis - Human Effort Voices |
| 1 | 2 | 96 | HMNBrth_Construction Kit Male Screeching Breath Inhale Weak Squeal 05_ESM_HC4 | Epic Stock Media - Humanoid Creatures Vol 4 - Monstrous and Undead Creature Vocalization Sound Sets |
| 1 | 2 | 96 | CLOTHFlp_Action Inventory Open Flip Cloth Canvas Bag Slide Light 02_ESM_FG2 | Epic Stock Media - Fantasy Game 2 - Sound Kit for Enchanted Realms |
| 1 | 2 | 96 | HMNCough_Police Officer Cough Vocal Male 11 | Epic Stock Media - AAA Game Character Police Officer |
| 1 | 2 | 96 | VOXFem_Anime, Warrior Elf Princess Aggressive Yell_344 Audio_Anime Fight Voices Vol 2_14 | 344 Audio - Anime Fight Voices Vol. 2 |
| 1 | 2 | 96 | VOXMale_Announcer Vocal Male Dry Game Statement Objective Completed 04_ESM_SGAV | Epic Stock Media - Shooter Game Announcer Voice |
| 1 | 2 | 96 | FOLYClth_ClothMovement29_InMotionAudio_FoleyT-Shirt | InMotionAudio - Foley T-Shirt |
| 1 | 2 | 96 | VOXMale_Announcer Vocal Male Dry Special Kill Headshot 01_ESM_SGAV | Epic Stock Media - Shooter Game Announcer Voice |
| 1 | 2 | 96 | HMNBrth_British Detective Breathing Vocal Female Idle Annoyed Breath Grunt_ESM_AAAGCBFD | Epic Stock Media - AAA Game Character British Female Detective |
| 1 | 1 | 96 | WHSTHmn_Whistle Male 06 09_SNDBTS_VH | SoundBits - Vox Hominis - Human Effort Voices |
| 1 | 2 | 96 | VOXMale_Warrior Opponent Attack, 'Ai yah' 2_344 Audio_Anime Fight Voices | 344 Audio - Anime Fight Voices Vol. 1 |
| 1 | 2 | 96 | VOXFem_Anime, Adult Warrior Breaths 40_344 Audio_Anime Fight Voices Vol 2 | 344 Audio - Anime Fight Voices Vol. 2 |
| 0 | 2 | 96 | HMNBrth_Police Officer Gasp Vocal Male Shocked Alert 1 | Epic Stock Media - AAA Game Character Police Officer |
| 0 | 2 | 96 | VOXFem_Anime, Schoolgirl Fight Exhale 15_344 Audio_Anime Fight Voices Vol 2 | 344 Audio - Anime Fight Voices Vol. 2 |
| 0 | 2 | 96 | FOLYClth_SinglePats04_InMotionAudio_FoleyT-Shirt | InMotionAudio - Foley T-Shirt |
| 0 | 2 | 96 | VOXMale_Male Adventurer, Active Yell 19_344 Audio_Anime Fight Voices | 344 Audio - Anime Fight Voices Vol. 1 |


## UI — 19 files, 28 MB

Clicks and denies, to retire the synthesized placeholder.

| MB | ch | kHz | File | Library |
|---:|---:|---:|---|---|
| 13 | 2 | 96 | UIGlitch_User interface_Glitch_High_Electronic_The Noisery_Rich Glitch_05 | The Noisery - Rich Glitch |
| 4 | 2 | 96 | UIAlert_Alert Designed Transition Swell Creature Strange Alien Layered 01_ESM_HALG | Epic Stock Media - Halloween Game - Haunted House and Horror Audio Scare Kit |
| 3 | 2 | 96 | UIGlitch_Designed_Glitch_Corrupted_Data Error_The Noisery_Rich Glitch_06 | The Noisery - Rich Glitch |
| 1 | 2 | 48 | UIMisc_Xylophone Ringtone 2_CB Sounddesign_APPlicable Sounds | CB_Sounddesign - Applicable Sounds - Organic UI and Building Games SFX |
| 1 | 2 | 96 | Button Arp Twinkle | Cinematic Sound Design - User Interface |
| 1 | 2 | 96 | MACHMed_Thermometer_ButtonPress_Beep03_InMotionAudio_MedicalThermometer | InMotionAudio - Medical Thermometer |
| 1 | 2 | 96 | UIAlert_Collect Scifi Futuristic Electronic Bass Burst Sweep Heavy 04_ESM_FG2 | Epic Stock Media - Fantasy Game 2 - Sound Kit for Enchanted Realms |
| 1 | 2 | 96 | Interface Arp Reveal Down Long | Cinematic Sound Design - System & UI Feedback Elements |
| 1 | 1 | 192 | SBvr_Power Button 003 | Sonic Bat - Vintage Radio |
| 0 | 1 | 96 | UIClick_UI Button Analog Vintage Double Click Neutral Dry Press 11_ESM_BG | Epic Stock Media - Board Game - Sound Set Kit for Tabletop and Digital Games |
| 0 | 2 | 96 | Interface Plucks Happy | Cinematic Sound Design - User Interface |
| 0 | 2 | 96 | Interface Sci-Fi Ping Down | Cinematic Sound Design - System & UI Feedback Elements |
| 0 | 2 | 96 | Interface Deny Low Fat Dark | Cinematic Sound Design - System & UI Feedback Elements |
| 0 | 2 | 96 | Interface Percussion Snap | Cinematic Sound Design - Interface & Infographics |
| 0 | 2 | 96 | Interface Pop High Short | Cinematic Sound Design - Interface & Infographics |
| 0 | 2 | 96 | Interface Accept Glassy Snap | Cinematic Sound Design - Interface & Infographics |
| 0 | 2 | 48 | UIMisc_Kalimba 3 Up_CB Sounddesign_APPlicable Sounds | CB_Sounddesign - Applicable Sounds - Organic UI and Building Games SFX |
| 0 | 2 | 96 | MECHClik_USALightSwitch_On05_InMotionAudio_USAHotel | InMotionAudio - USA Hotel |
| 0 | 2 | 96 | BEEPMed_Thermometer_Beep11_InMotionAudio_MedicalThermometer | InMotionAudio - Medical Thermometer |


## Not dungeon material — 186 files, 4737 MB

Left in the archive. Not worth listening time.
