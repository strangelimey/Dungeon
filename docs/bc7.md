# BC7 texture compression

The reference for `tools/AssetBaker/Bc7Encoder.cpp` and `tools/Bc7Test`. What
the encoder does, why each mode is there, what is actually checked, and what is
still open.

## Why this exists at all

Every texture the game ships is stored as BC7: a fixed 16 bytes per 4x4 pixel
block, a flat 4:1 against RGBA8, decoded in the sampler by fixed-function
hardware at no cost. That took the installed texture set from 384 MB to 96 MB on
disk and the same factor off VRAM and bandwidth.

The format is fixed; the *encoder* is ours, and an encoder is only ever a search.
Every block has 2^128 possible encodings and we pick one. Everything below is
about making that search better, and — more importantly — about being able to
tell whether it got better.

## The format, in one paragraph

A BC7 block is 128 bits. The first few bits are a unary mode marker (N zeros then
a one = mode N), and the mode decides how the remaining bits are cut up: how many
*subsets* the 16 pixels are partitioned into, how many bits each endpoint colour
component gets, whether there are p-bits (a shared low-order bit that extends
endpoint precision), how alpha is handled, and how many bits each pixel's
interpolation *index* gets. Endpoints define a line in colour space; each pixel
stores an index naming a position along it. There are eight modes and they trade
the same 128 bits between those axes in different proportions. A block is encoded
in whichever mode suits its content, and the decoder just reads the marker — so
mixing modes freely across an image costs nothing at runtime.

## The four modes we implement

| | mode 6 | mode 1 | mode 3 | mode 5 |
|---|---|---|---|---|
| subsets | 1 | 2 (of 64 fixed shapes) | 2 (same shapes) | 1 |
| colour endpoints | RGBA 7 bits + p-bit each | RGB 6 bits + 1 p-bit shared per subset | RGB 7 bits + p-bit each | RGB 7 bits, no p-bit |
| alpha | on the same line as RGB | forced opaque | forced opaque | **its own 8-bit endpoints and own index set**, optionally swapped with a colour channel |
| index steps | 16 | 8 | 4 | 4 (colour), 4 (alpha) |
| good at | photographic albedo, smooth blocks | material boundaries with internal gradients | material boundaries, each region smooth | one channel that will not fit the line |

**Mode 6** is the workhorse. It spends everything on one line with the most index
positions available, which is exactly right when the block's 16 pixels really do
lie near a line.

**Mode 1** exists because a 4x4 block that straddles two materials — brick and
mortar, stone and moss — has two colour clusters, and one line through both lands
in the empty middle. That is what a smeared material edge *is*. Mode 1 buys a
second line by giving up endpoint precision and half the index steps, and picks
from 64 hardware-defined partition shapes to say which pixels belong to which.

**Mode 5** exists because of how this project packs its normal maps. `<name>_n.png`
carries the normal in RGB and the parallax **height in alpha**, and those two
signals are unrelated: the height steps where the normal does not, and vice
versa. Mode 6 fits a single line through RGB *and* A together, so every height
edge drags the normal off its line and every normal change drags the height. Mode
5 decouples them — separate endpoints, separate indices — for the price of 4
index steps each. Those blocks are also precisely the ones mode 1 can never take
(it forces alpha opaque), so before mode 5 existed, every normal map in the
project had exactly one option and no benefit from multi-mode encoding at all.

**Mode 3** is mode 1's opposite trade on the same 64 partition shapes: more
endpoint precision (a p-bit per endpoint, so 8 real bits) bought with fewer index
steps. Mode 1 places its *pixels* precisely, mode 3 places its *endpoints*
precisely. Mode 3 wins where the two regions are far apart and each is internally
smooth — there is little for intermediate steps to do, and the endpoints need to
be exact. Mode 1 wins where the regions have gradients inside them. Neither
dominates. Mode 3 also makes flat blocks *lossless*, which mode 6 could not
manage: the flat test tile went from 54.15 dB to 99.99.

**Mode 5's channel rotation** deserves its own note, because the reasoning that
first left it unimplemented was wrong. The argument was: this project's odd
channel out is the height in alpha, and mode 5 already decouples alpha, so
rotations would only serve content we do not have. In fact the rotations are
worth **+0.89 dB**, and the biggest single beneficiary is exactly the content the
argument was about — one scanned normal map went from 35.77 to 39.94 dB, its
mode-5 share rising from 2% of blocks to 93%. The reason is that in a normal map
the awkward channel is frequently *blue*: z is derived from x and y and behaves
nothing like them. Decoupling alpha was never the point; decoupling **whichever**
channel refuses to lie on the line is.

Mode 5 also earns its keep beyond normal maps: alpha cutouts, and up to 59% of
the blocks in a scanned ORM map.

**These modes overlap, and the audit shows it.** Measured against the full set,
mode 3 is worth +0.31 dB and mode 5 +3.45 dB — but with mode 5's rotations
disabled, mode 3's contribution is +3.40 dB. They are competing to solve the same
problem (a block with more structure than one line can carry) from different
directions, so their individual "worth" figures are not additive and depend on
what else is enabled. Quote the full-set number, not a sum.

## The invariant that makes mode selection work

Each mode encoder returns the error its result carries, and the block keeps the
lowest. That only means anything if the errors are **the same quantity**, so by
construction every one of them is the sum of squared differences over the same 16
pixels x 4 channels. Mode 1 contributes no alpha error because it only runs where
alpha is already 255 — not because alpha is excluded from its metric.

The estimate is computed with the hardware's own rounded blend on the
*reconstructed* endpoints, never on the packed bytes. That is deliberate: it
makes the estimate independent of the bit packing, which is what lets the test
harness use one to check the other.

## What is checked, and what is not

`tools\Bc7Test.ps1` — the regression run. Exit 0 = PASS.

```bash
.\tools\Bc7Test.ps1
```

Three checks, in increasing order of how much they would hurt:

1. **Consistency.** The harness decodes the packed bytes with its own decoder
   (`tools/Bc7Test/Bc7Decode.cpp`, written from the format's field tables rather
   than from the encoder's bit writers) and demands the decoded error equal the
   encoder's estimate **exactly** — not within a tolerance. Exactness is
   available here and worth insisting on: every term is a small integer, so the
   float sums are exact regardless of the order they are added in. A wrong bit
   offset, a dropped anchor MSB, a p-bit on the wrong endpoint, or a partition
   read against the wrong subset all break this.

   This is the *correctness* gate, not a quality one. That estimate is what picks
   the mode. If it lies, mode selection is a coin toss and every dB claimed below
   is meaningless.

2. **Thread invariance.** Blocks are independent, so the fan-out must not change
   a byte. Encoded once serially and once fanned out, the buffers must match.

3. **Quality against a recorded baseline** (`tools/bc7-baseline.txt`), so a
   refactor that quietly loses a dB fails now rather than being noticed a year
   later. A deliberate trade re-records with `-UpdateBaseline` and says so in the
   commit message.

`-SelfTest` corrupts the encoded bytes and requires the run to come back FAIL. A
harness that cannot fail is not evidence of anything — the same reasoning as
`AllocTest.ps1`'s inverted mode.

**What this cannot catch, stated plainly.** The encoder and the harness's decoder
are independent of each other, but they are not independent of the *spec*. If the
encoder writes a field in the wrong order and the decoder reads it back in that
same wrong order, they agree and the GPU does not. For modes 1 and 6 that risk is
already retired — the game renders their output and has for months. **A new
mode's field layout is only proven once the GPU has drawn it**, so adding a mode
means an in-game look, not just a green harness. That check is not automatable
without either a GPU readback harness or a DirectXTex dependency, and neither has
earned itself yet.

Modes 5, 3 and mode 5's rotations were all cleared this way on 2026-08-04: the
whole texture set rebaked, then the showcase level walked. Pick the surface by
its MEASURED mode mix, not by assumption — `Bc7Test <file.png>` prints the mix
for any image. That mattered: for plain mode 5 the obvious candidate (the brick
wall) turned out to be 1% mode 5 while the floor was 21%, so checking the wall
would have proved almost nothing. After rotations landed the picture inverted —
`wall_brick_old_2k_n` became 99% mode 5 and the albedo 48% mode 3 — so the
corridor view exercises both new layouts on nearly every visible pixel.

The corpus is mostly synthetic and generated in the harness, deterministically:
the real textures are gitignored, so a corpus depending on them would not run on
a fresh clone. Each synthetic image isolates one thing (a material boundary, a
smooth gradient, incompressible noise, a flat block's degenerate maths,
height-in-alpha, an alpha ramp, a cutout). `--assets <dir>` adds a sample of
installed textures, cropped rather than scaled — a resample would invent block
content no shipped texture contains.

## What each knob is worth

`Bc7Test --audit` prints this table. Measured on the 16-image corpus (7 synthetic
+ 9 installed), release build, 8-core machine:

| configuration | mean PSNR | vs mode 6 | ms |
|---|---|---|---|
| mode 6 only (the original encoder) | 42.80 | — | 28 |
| modes 1+6 (what shipped before this work) | 44.41 | +1.61 | 79 |
| modes 1+5+6 (+ rotations) | 51.15 | +8.35 | 190 |
| modes 1+3+6 (no alpha mode) | 48.01 | +5.21 | 347 |
| **all four modes, 8 shapes (current)** | **51.39** | **+8.59** | 225 |
| all four, 16 shapes | 51.46 | +8.65 | 372 |
| all four, 64 shapes (exhaustive) | 51.53 | +8.73 | 1256 |
| all four, no mode-5 rotations | 50.57 | +7.77 | 344 |
| all four, p-bit proxy | 51.33 | +8.53 | 223 |
| current, single-threaded | 51.39 | +8.59 | ~3100 |

**+7.0 dB over what shipped**, for about 3x the encode time — nearly all of which
the fan-out gives back.

A full `AssetBaker mips` over the installed set — 639 PNGs at every resolution,
1k through 4k — takes **33.7 minutes**, against 23.6 for the three-mode encoder.
That is the real-world number to compare against when a change claims to be
affordable; the corpus milliseconds above are for ranking knobs, not for
budgeting a bake.

**Aggregate as the mean of per-image PSNR, never by pooling squared error.** This
is the methodological trap and it cost a wrong decision before it was caught.
Pooled error is dominated by whichever image compresses worst — the noise tile
sits ~1000x higher in MSE than a smooth one — so a pooled number is a report on
that one tile. Pooled, the p-bit trial looked worth +0.01 dB and not worth its
cost. Per image, dropping it costs **1.35 dB on the brick tile** and 0.16–0.27 dB
on real scanned stone: exactly what this dungeon is built out of. The audit's
`worst img` column exists to catch that class of mistake.

Every non-obvious default was set by measurement, and the reasoning lives in the
`Bc7Options` comments so it stays with the code:

- `prescore = Scatter`. The shortlist score was a sum of per-subset bounding-box
  extents, which is blind to how many pixels each subset holds — a 15/1 split
  scores well trivially because the lone pixel's box has zero extent. Scoring
  total *within-subset scatter* (Σ n·variance, the k-means objective) instead is
  the same cost and strictly better, because it is scoring what the solve
  actually goes on to optimise:

  | shortlist | miss rate | excess error |
  |---|---|---|
  | bounding box, top 8 | 30.50% | 15.93% |
  | bounding box, top 16 | 20.64% | 7.43% |
  | scatter, top 8 | 25.53% | **6.56%** |
  | scatter, top 16 | 16.77% | **3.32%** |

  Note that scatter@8 leaves less excess error than bbox@16 at half the solve
  cost — **fixing the score was worth more than doubling the shortlist**, which
  is what the "still open" list had guessed. Note also the two columns disagree
  about how bad bbox@8 is: a miss that costs a hair is not the same failure as
  missing the only good shape, so the rate alone would have understated it.

- `shapeTrials = 8`, having been 16. This number moved *down* when modes were
  added, and that is the interesting part: a block the shortlist mis-partitions
  usually has another *mode* that suits it, and the extra modes get there first.
  Search breadth and mode coverage buy overlapping things. Re-measure this
  whenever a mode lands rather than ratcheting it upward.

- `trialPBits = true`. See the pooled-vs-per-image trap above.

- `trialRotations = true`. Worth +0.89 dB; see the mode 5 section for why the
  argument against it was wrong.

## Parallelism

Blocks are independent, so `EncodeBc7` fans out over block rows — **~14x** on
this machine, which is what makes the multi-mode search affordable at all: the
encoder now trials four modes and mode 5 four times over, and still bakes faster
than the old serial two-mode one. Deliberately **not** a
`Core/ThreadManager` client: that registry is for long-lived workers with
cadences, watchdogs and a supervisor, and this wants a batch that starts,
saturates, and joins. Plain `std::jthread` over an atomic row counter is the
whole implementation.

The fan-out is per *block*, not per *texture*. That accelerates the single-file
`import` path and the editor's live worn-block rebake (which a user sits behind a
busy overlay waiting for), and it load-balances better across a directory of
mixed 1k/2k/4k than per-file would.

## Adding a mode

1. Write `EncodeModeN` returning the same error quantity as the others.
2. Add it to the `Bc7Mode` mask and to `EncodeBlock`'s `consider` chain, with its
   eligibility rule (which content it can legally represent).
3. Teach `tools/Bc7Test/Bc7Decode.cpp` to decode it, **from the spec's field
   table, not from the encoder's writer**.
4. Run `Bc7Test.ps1`. Consistency must be 0 bad blocks.
5. Run `Bc7Test --audit` to find out whether it is worth its time.
6. **Look at it in the game.** Step 4 cannot prove the field layout (see above).
7. `-UpdateBaseline`, and quote the delta in the commit message.

A new mode can never make output worse — it is only ever chosen when it measures
better — but that guarantee rests entirely on the error estimate being honest,
which is what step 4 establishes. Without the harness, step 2 would be a leap of
faith.

## Still open

Four of the eight modes remain unimplemented, and the case for each is now much
weaker than it was, because the four we have overlap so heavily. Every remaining
item should be measured before it is built, not after.

- **Modes 0 and 2 — three subsets.** The last real quality on the table, and the
  narrowest. Mode 0 gives three regions but only 4-bit endpoints and 16 partition
  shapes; mode 2 gives 5-bit endpoints and 64 shapes but 2-bit indices. A 4x4
  block containing three distinct materials is not common, and both mode 3 and a
  rotated mode 5 already rescue many of the blocks that motivated them. Estimate
  the ceiling first: encode the corpus with the current set, then check how much
  error remains in blocks whose best mode is still a poor fit. If that number is
  small, this is finished work.
- **Modes 4 and 7.** Mode 4 is mode 5's sibling with an index-selector bit and
  3-bit indices on one of the two index sets; mode 7 is two subsets *with* alpha.
  Mode 7 is the more interesting of the two — nothing we have gives a
  material boundary and real alpha at the same time, so an alpha-cutout texture
  with two materials in a block currently has to give one of them up.
- **A smarter prescore still.** Scatter closed most of the gap (6.56% excess at
  top 8, from 15.93%), but exhaustive search still wins on a quarter of blocks.
  The remaining error is a line-fit problem, not a clustering one: scatter scores
  a subset by spread about its *mean*, while the solve fits a *line*. Subtracting
  the principal component would score the real residual, but computing an
  eigenvector per shape per block is far too slow. A cheap approximation might
  not be.
- **GPU-side verification**, the one structural hole in the harness: decode a
  block on the GPU and read it back, so a new mode's field layout is provable
  without a human looking at the game. Deferred twice now on the same reasoning —
  the manual check takes five minutes and has twice been sufficient — but the
  argument weakens each time a mode is added, since the modes are getting rarer
  and harder to find on screen. Mode 7 would be the point to build it.
