---
description: The health record's ring — wrap, cross-thread writes, torn reads
allowed-tools: PowerShell, Read, Grep, Glob
---

Exercise `Core/Diagnostics` directly. 62 checks, about four seconds (tests 9
and 11 each wait one log window out).

```
.\tools\CheckAll.ps1 -Only diag
```

## What it is guarding

A lock-free ring with a sequence-number publish, written from any thread and
read while it is being written — exactly the kind of code that reads correctly
and behaves otherwise.

**The load-bearing test is number 6**: four writers hammering one slot while a
reader walks it, every event self-describing so a torn read cannot pass.
Normally ~16k writes against ~40k live reads, 0 torn. If that one fails, the
publish ordering or the sequence check is broken and nothing else in the suite
should be trusted either — the health record is what `/check-threads` and
`/check-health` both read their verdicts from.

Tests 8 to 11 check the LOG throttle against the real `diagtest.log`, found
through `log::FilePath()`: the exact per-window budget, one swallowed-count line
with the right count once a window rolls, the lines a thread's exit writes, a
repeat run of 57 and of 150 each writing one closing line for the repeats after
its last power of ten, a run in a spent window losing none of its events, and a
33rd thread name taking a dormant slot with a clean window. Test 12 fills a
`stack::SeenSet` (the one-stack-per-site memory both the record and the
allocation guard log through) past its 64: no further site may read as new,
each offer is counted (a repeat counts again - it is a count of offers, not of
sites), and exactly one "the stack set is full" line is written. An
unreadable log is a `[FAIL] the log can be read back`, never a skip.

## Reading the output

The tool logs its own synthetic failures to stderr as it runs — lines like
`diag ... exception on 't.hammer'` are the test *working*, not a problem. The
verdict is the `diagtest RESULT=` line.

No self-test mode: it is a unit test whose failure mode is a `[FAIL]` line, not
a silent green.
