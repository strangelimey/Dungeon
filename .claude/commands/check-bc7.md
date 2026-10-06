---
description: The BC7 encoder's error estimate against an independent decoder
argument-hint: "[selftest|audit]"
allowed-tools: PowerShell, Read, Grep, Glob
---

Verify the texture encoder (release build — the debug encoder is too slow to be
worth waiting for, and the script defaults the same way).

- no argument → `.\tools\CheckAll.ps1 -Only bc7`
- `selftest` → `.\tools\CheckAll.ps1 -Only bc7 -SelfTest` (must FAIL)
- `audit` → `.\tools\Bc7Test.ps1 -Config release -Audit` — the knob sweep that
  set the encoder's defaults, not a pass/fail run

## What it is guarding

The encoder records the error it believes each block carries, and **that
estimate is what picks the mode**. The harness decodes the packed bytes with an
independent decoder and demands exact agreement. If the estimate lies, mode
selection is a coin toss and every quality claim in docs/bc7.md is void.

Quality is gated by `tools\bc7-baseline.txt`. That gate read NOTHING until
2026-10-05 (the file's `#` header ended the loader), so a run that prints
`matched=0`, or fails `baseline: at least one entry matches`, is that bug or a
broken file - never a pass. The synthetic (`syn.*`) images are the same on every
machine, so they must pair up EXACTLY with the `syn.*` rows: a `FAIL synthetic,
no baseline entry` or `FAIL syn.* entry, no such image` line is a lost row, or
an image renamed or added without re-recording - that image is no longer gated.
Only the real-texture rows, printed as `note:` lines, may legitimately not match:
the installed pool decides which ones are sampled. Re-record with
`.\tools\Bc7Test.ps1 -UpdateBaseline` for a deliberate trade, a new synthetic
image, or when the pool's sample has moved (check every row that DID match is
unchanged first), and say which in the commit - never to clear a regression
nobody can explain.

`-SelfTest` injects three faults and requires exactly the three checks they aim
at to fail - `consistency` (a corrupted copy of the bytes), `quality` (the
baseline raised 1 dB, caught on every matched image) and the mip filter's sRGB
check (the filter as it was before code-review C414, averaging the stored bytes,
which takes the black-and-white checker to 128; the rounding check beside it must
stay green) - and every other check to pass. It prints `SELF-TEST: the raised
baseline caught on 16 of 16 matched images`, with any image the raised bar missed
named above it, then by name any check that failed or passed against
expectation, and last the self-test's own verdict, `SELF-TEST PASS - 3 of 3
expected failures, 0 unexpected`. That line counts the missed images too (one
miss makes it `SELF-TEST FAIL - ..., 1 other miss (named above)`), so it always
agrees with the `caught=` on the line after it.

The last line is the shared verdict every native judge prints
(tools\Common\Verdict.h): `bc7test RESULT=PASS checks=9 failures=0 images=..
consistency_bad=0 thread_diff=0 regressed=0 matched=.. minpsnr=.. self_test=0`.
Under `-SelfTest` it reads `RESULT=FAIL ... self_test=1 caught=1` - the checks
failed on the faults, exactly the expected ones. `Bc7Test.ps1` reads that line
back against the exit code (tools\Verdict.ps1) and fails a run where they
disagree.

## Reading the numbers

**Aggregate PSNR is the mean of per-image PSNR, never pooled squared error.**
Pooling is dominated by whichever tile compresses worst — the noise tile sits
~1000x higher in MSE than a smooth one — and it once hid a knob worth 1.35 dB on
brick behind an average of +0.01 dB. If a report shows pooled figures, the
comparison is meaningless.

A quality *regression* (lower PSNR, same correctness) is a different finding
from a correctness failure (estimate disagrees with the decoder). Say which.
