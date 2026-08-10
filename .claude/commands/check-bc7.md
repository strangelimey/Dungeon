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

`-SelfTest` corrupts the bytes and requires a FAIL.

## Reading the numbers

**Aggregate PSNR is the mean of per-image PSNR, never pooled squared error.**
Pooling is dominated by whichever tile compresses worst — the noise tile sits
~1000x higher in MSE than a smooth one — and it once hid a knob worth 1.35 dB on
brick behind an average of +0.01 dB. If a report shows pooled figures, the
comparison is meaningless.

A quality *regression* (lower PSNR, same correctness) is a different finding
from a correctness failure (estimate disagrees with the decoder). Say which.
