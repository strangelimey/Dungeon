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

## The three modes we implement

| | mode 6 | mode 1 | mode 5 |
|---|---|---|---|
| subsets | 1 | 2 (of 64 fixed shapes) | 1 |
| colour endpoints | RGBA 7 bits + p-bit each | RGB 6 bits + 1 p-bit shared per subset | RGB 7 bits, no p-bit |
| alpha | on the same line as RGB | forced opaque | **its own 8-bit endpoints and own index set** |
| index steps | 16 | 8 | 4 (colour), 4 (alpha) |
| good at | photographic albedo, smooth blocks | material boundaries | height-in-alpha, cutouts |

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

Mode 5 turns out to earn its keep beyond normal maps: it also wins on alpha
cutouts and on a surprising share of ORM blocks (up to 25% on scanned stone).

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

Mode 5 was cleared this way on 2026-08-04: the whole texture set rebaked, then
the showcase level walked. The floor there is `floor_slabs`, whose normal+height
map is **21% mode-5 blocks**, and it draws with coherent parallax relief — a
wrong alpha field layout would have made that depth noise. Do the same for the
next mode, and pick a surface whose mode mix you have actually measured rather
than one you assume uses it (`Bc7Test <file.png>` prints the mix for any image;
the wall in that same room turned out to be 1%).

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
| mode 6 only (the original encoder) | 42.91 | — | 44 |
| modes 1+6 (the previous default) | 44.55 | +1.65 | 116 |
| modes 1+5+6, 8 shapes | 46.88 | +3.97 | 132 |
| modes 1+5+6, 16 shapes **(current)** | 47.10 | +4.19 | 206 |
| modes 1+5+6, 64 shapes (exhaustive) | 47.27 | +4.37 | 677 |
| current, single-threaded | 47.10 | +4.19 | 2492 |

Mode 5 is the single biggest win available: **+2.33 dB** over what shipped, for
about 14% more time.

A full `AssetBaker mips` over the installed set — 639 PNGs at every resolution,
1k through 4k — takes **23.6 minutes**. That is the number to compare against
when a future change claims to be affordable.

**Aggregate as the mean of per-image PSNR, never by pooling squared error.** This
is the methodological trap and it cost a wrong decision before it was caught.
Pooled error is dominated by whichever image compresses worst — the noise tile
sits ~1000x higher in MSE than a smooth one — so a pooled number is a report on
that one tile. Pooled, the p-bit trial looked worth +0.01 dB and not worth its
cost. Per image, dropping it costs **1.35 dB on the brick tile** and 0.16–0.27 dB
on real scanned stone: exactly what this dungeon is built out of. The audit's
`worst img` column exists to catch that class of mistake.

Both non-obvious defaults were set by measurement, and their reasoning lives in
the `Bc7Options` comments so it stays with the code:

- `shapeTrials = 16`. The bounding-box prescore that shortlists partition shapes
  **is** lossy — exhaustive search finds a better shape on ~31% of blocks, which
  answers a question that had been open on argument alone. But the shortlist is
  cheap to widen and expensive to perfect: 8 → 16 buys +0.22 dB for +66% time,
  16 → 64 a further +0.17 dB for another +200%. 16 is the knee.
- `trialPBits = true`. See above.

## Parallelism

Blocks are independent, so `EncodeBc7` fans out over block rows — **12.1x** on
this machine (206 ms vs 2492 ms for the same corpus), which more than repays the
multi-mode search: the current encoder does three modes and twice the partition
search of the old one and still finishes a bake in a fraction of the time.
Deliberately **not** a
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

- **Modes 3, 0, 2.** Mode 3 (two subsets, 7-bit endpoints, 2-bit indices) is the
  natural companion to mode 1 for two-material blocks that are smooth *within*
  each region. Modes 0 and 2 are three-subset and narrow. Diminishing returns
  after mode 3.
- **Mode 5 channel rotation.** A non-zero rotation swaps alpha with one of R/G/B
  before encoding, letting a block whose odd channel out is a *colour* use the
  decoupled path. Currently always 0, so only literal alpha benefits. Costs a 4x
  solve; unmeasured.
- **A better prescore.** The current one sums bounding-box extents with no
  population term, so a 15/1 split scores well trivially. It misses on ~31% of
  blocks; a population-weighted score might close some of that for free, which
  would be worth more than raising `shapeTrials` further.
- **GPU-side verification**, to close the one hole the harness cannot: decode a
  block on the GPU and read it back, making a new mode's field layout provable
  without a human looking at it.
