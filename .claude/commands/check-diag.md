---
description: The health record's ring — wrap, cross-thread writes, torn reads
allowed-tools: PowerShell, Read, Grep, Glob
---

Exercise `Core/Diagnostics` directly. 35 checks, about two seconds.

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

## Reading the output

The tool logs its own synthetic failures to stderr as it runs — lines like
`diag ... exception on 't.hammer'` are the test *working*, not a problem. The
verdict is the `diagtest RESULT=` line.

No self-test mode: it is a unit test whose failure mode is a `[FAIL]` line, not
a silent green.
