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

`-SelfTest` injects two faults and requires exactly the two checks they aim at to
fail - `consistency` (a corrupted copy of the bytes) and `quality` (the baseline
raised 1 dB, caught on every matched image) - and every other check to pass. Its
last line names any mismatch: `SELF-TEST PASS - 2 of 2 expected failures, 0
unexpected; the raised baseline caught on 16 of 16 matched images`.

## Reading the numbers

**Aggregate PSNR is the mean of per-image PSNR, never pooled squared error.**
Pooling is dominated by whichever tile compresses worst — the noise tile sits
~1000x higher in MSE than a smooth one — and it once hid a knob worth 1.35 dB on
brick behind an average of +0.01 dB. If a report shows pooled figures, the
comparison is meaningless.

A quality *regression* (lower PSNR, same correctness) is a different finding
from a correctness failure (estimate disagrees with the decoder). Say which.
