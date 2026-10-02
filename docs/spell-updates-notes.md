# Spell updates - notes

Branch `spell-updates` (2026-10-01), worktree C:\Dev\Dungeon-spell-updates.

## Raw notes (Michael's brain dump, 2026-10-01)

Context: started from the list of current spells - 16, four schools, each a
1-symbol bolt plus Project / Protect / Sight 2-symbol forms. Starting with the
1-symbol spells, i.e. "magic school".

1. "I think these are too powerful." (the four 1-symbol spells: gust, rock,
   flame, splash - all bolts today)

2. FIRE, 1 symbol = Puff of Flame. "It is NOT a bolt. It is a simple puff in
   the character's hand."
   - Holding an UNLIT TORCH in the other hand -> it lights it.
   - Standing in front of an UNLIT TORCH -> it lights it.
   - Standing in front of an UNLIT BRAZIER -> lights ONLY if the spell power is
     high enough.

3. EARTH, 1 symbol = Pebble. "Pebble is a small stone in the hand."

4. AIR, 1 symbol = Puff of Wind. "A breeze from the hand."
   - If it's HIGH ENOUGH LEVEL, it could PUSH BACK a monster or an ARROW.
   - In front of a TORCH or BRAZIER -> it BRIEFLY FLARES UP.
   - "Other than that, it's not much use."

5. WATER, 1 symbol = Splash. "A handful of water."
   - Holding an EMPTY WATER CONTAINER (flask, waterskin, etc.) -> adds SOME
     water to it.
   - In front of a TORCH -> puts it out.
   - In front of a BRAZIER and HIGH ENOUGH POWER -> "it will blow it out"
     (extinguishes it).
   - Torch and brazier alike, when extinguished, BRIEFLY INCREASE THE SQUARE'S
     TURBIDITY.

6. BOLTS = <element> + PROJECT (the 2-symbol Project form). "So fire bolt,
   water bolt, earth bolt (magic missile), air bolt."
   - "An element that travels through the air."
   - On impact it does NOT EXPLODE - it just attacks the SINGLE TARGET.
   - (Today's names for these: Fire Burst / Water Bolt / Slingshot / Push.)

7. A THIRD-TIER SYMBOL is needed. Start with two: MULTIPLE and EXPLODE. "Find
   suitable Futhark symbols."
   - MULTIPLE: e.g. turns a bolt into MULTIPLE BOLTS. Higher power -> more bolts.
   - EXPLODE: creates a BLAST RADIUS on impact. Higher power -> higher
     radius / damage.
   - Rune candidates offered (Claude, on request): Multiple = Ingwaz (seed, one
     becoming many; alt Fehu, abundance, but its glyph is close to Ansuz/Air);
     Explode = Hagalaz (hail, sudden destructive force; alt Thurisaz, the thorn).
   - DECIDED: MULTIPLE = INGWAZ, EXPLODE = HAGALAZ.

8. Third-tier runes after a WARD (element + Protect):
   - MULTIPLE + ward = WHOLE PARTY rather than just the caster.
   - EXPLODE + ward = a BLAST RADIUS of the element CENTERED ON THE CASTER; the
     CASTER'S SQUARE TAKES NO DAMAGE.

9. RUNE NAMES: a rune's NAME should not be "fire" etc. - it should be the NAME
   OF THE FUTHARK RUNE (Kenaz, Laguz, ...), with "fire" (its meaning) mentioned
   in the DESCRIPTION. (Applies to all runes, schools and forms alike.)

10. REPEL (after Phase 2, on the turned-back arrow): "repel would be compared to
    the power of the projectile (spell, arrow, rock, etc.). It would reduce the
    attack strength and, if powerful enough, fling it back where it came from."
    ANSWERED (the arithmetic): the breeze's power P comes off the shot's
    strength S. P < S: it flies on at S - P. P >= S: it is flung back at what P
    had left over, P - S, capped at S - a strong breeze returns it hard, a bare
    match returns it weakly.

11. DOUSED-FIRE HAZE (during Phase 3): "when a torch/brazier goes out, it
    increases the square's turbidity through the existing effects system: it
    creates increased turbidity of x power and diminishes over y seconds."
    BUILT: a `smoke` effect class (effects.cat `haze = 1`) lands on the FIRE's
    own effect list when it goes out, by any cause, from its kind's
    `on_douse = smoke <x> <y>` (fixtures.cat: sconce 0.6 / 2.5 s, brazier
    1.0 / 4 s); the square's extra haze is read off it each frame as
    x * time-left / y.

## Organized (2026-10-01)

The dump reshapes the spell ladder into three tiers with one meaning each:

| Tier | Recipe | Meaning |
| --- | --- | --- |
| 1 | <school> | a small thing IN THE HAND - utility, not a weapon (notes 1-5) |
| 2 | <school> + <form> | the form's job: Project = a single-target bolt (6); Protect / Sight as today |
| 3 | <school> + <form> + <modifier> | Ingwaz = MORE (bolts, or the whole party); Hagalaz = BURST (7, 8) |

A. TIER 1 - THE HAND SPELLS (notes 1-5). None is a projectile any more.
   - Kenaz / Puff of Flame: lights a held unlit torch (other hand), a wall torch
     ahead, a brazier ahead only past a power threshold.
   - Berkano / Pebble: a small stone appears in the hand.
   - Ansuz / Puff of Wind: flares a torch or brazier ahead; past a LEVEL
     threshold pushes back a monster or an in-flight arrow. "Not much use"
     otherwise.
   - Laguz / Splash: adds SOME water to an empty held container; douses a wall
     torch ahead, a brazier ahead only past a power threshold; a douse briefly
     raises that square's turbidity.
B. TIER 2 - BOLTS (note 6): element + Tiwaz, flies, strikes ONE target, never
   explodes. Fire Bolt / Water Bolt / Earth Bolt ("magic missile") / Air Bolt.
C. TIER 3 - MODIFIERS (notes 7, 8): Ingwaz (Multiple), Hagalaz (Explode), both
   scaling with power.
   - on a bolt: N bolts / a blast on impact (radius + damage up with power).
   - on a ward: the whole party / an elemental blast round the caster, the
     caster's square unharmed.
D. NAMING (note 9): runes display their Futhark name; the meaning goes in the
   description.
E. BALANCE (note 1): the tier-1 spells are "too powerful" - answered by A,
   which takes their damage away entirely.

### What the code says (mapped 2026-10-01; details in the plan)

- There is NO TORCH ITEM (a torch pack was bought, never imported) and the
  party's own light is an unconditional light at the eye. A1's "held unlit
  torch" needs one.
- Items carry NO PER-INSTANCE STATE (a slot is an id). "Unlit / lit torch" and
  "empty / some water" have to be separate catalog ids swapped in place.
- The only water container is `waterskin`, and drinking deletes it (one use).
- Wall torches and braziers cannot change lit state at runtime cheaply, and lit
  state is not saved. A smashed sconce probably keeps burning today
  (DouseFixture updates the map record, not the live fire) - a likely bug.
- Turbidity is a baked texture: a change costs a GPU stall and re-renders every
  shadow cube. "Briefly" needs a cheap transient path (a shader-constant puff,
  like the Sight peephole).
- Today's Fire Burst IS a blast, and the `skel_mage` casts `flame` as its bolt.
  Several eval suites and AllocTest -Impact lean on both.
- A spell sees no hand, no inventory and no world beyond spawning a bolt,
  messaging and applying an effect to a character.
- Recipes are exact sequences, any length; the rune grid holds 8 symbols (9 is
  a compile error) and a 3-rune spell starts at ~70% fumble at level 0.

### Gaps and questions (to ask one at a time)

1. Torch: is a held torch a new LIGHT SOURCE (the party light depends on it), or
   just an item that can be lit/unlit for now?
   ANSWERED: THE HELD TORCH IS THE LIGHT. The party is lit only by a lit torch
   in someone's hand (or, later, a light spell). Follow-ups this raises: how dark
   is "no torch" (total, or a dim floor); does a torch burn down; where torches
   come from.
   FOLLOW-UP, darkness: "Depends on the level's ambient light setting. If 0,
   then it's pitch black." (the level's existing `atmosphere ambient=` knob is
   the floor; no torch adds nothing on top of it)
   FOLLOW-UP, burn-down: first "not yet" - then REVERSED (2026-10-01, with the
   go for Phase 1): "a lit torch (in a hand) will burn down over time. When put
   in a backpack or placed on the floor, it is put out."
   FOLLOW-UP, supply: the starter party holds one lit torch, unlit torches are
   placed in levels, AND clicking a wall torch takes it off the sconce into the
   hand (the sconce goes dark).
2. Water: what is "some" water - fill levels on the waterskin (empty / half /
   full), and does drinking then step it down instead of consuming it? Is a
   flask a separate container to add?
   ANSWERED: FILL LEVELS - waterskin_empty / _half / full; drinking steps down
   a level, Splash steps up a level. No flask for now.
3. Pebble: which hand does the stone land in - the casting hand, the other, the
   first free one - and what if both are full? Is it just the existing
   throwable `rock`?
   ANSWERED: a NEW small throwable `pebble`; casting hand if empty, else the
   other hand, else it drops at the caster's feet.
4. Third-tier order: always school, form, modifier (Kenaz Tiwaz Hagalaz)? Do
   modifiers apply to tier 1 (Kenaz Ingwaz) or to Sight? Can both modifiers
   stack (four runes)?
   ANSWERED: school, form, ONE modifier. Only on Project and Protect for now
   (Sight + modifier fizzles); not on tier 1; no stacking.
5. Multiple on a bolt: how do the N bolts fly - both lanes at once, a quick
   volley down one lane, or a spread?
   ANSWERED: "Staggered, but each stays in the caster's lane. They can vary
   side-to-side a little." (a stream down ONE lane, small lateral jitter
   within it)
6. Thresholds: Puff of Wind's push says LEVEL, the brazier says POWER - keep
   that distinction?
   ANSWERED: POWER for all thresholds (power already folds in level + stats).
7. Fumble: ~70% base for any 3-rune spell at level 0 - intended?
   ANSWERED: keep it; tuning belongs to the balance pass.
8. Names: Fire Burst / Slingshot / Push become Fire Bolt / Earth Bolt / Air
   Bolt - rename the ids too (fireburst -> firebolt)? Old saves' spell lists
   would lose them.
   ANSWERED: rename the ids - firebolt / earthbolt / airbolt / waterbolt.
9. The skel_mage's bolt: Fire Bolt (it shoots) or something else?
   ANSWERED: "Can start as simple fire bolt. Higher level mages shoot with
   ingwaz (volley) and even higher ones do Hagalaz".
10. Where do players get Ingwaz and Hagalaz?
   ANSWERED: placed tablets in levels (deeper, guarded); not the starter kit.
