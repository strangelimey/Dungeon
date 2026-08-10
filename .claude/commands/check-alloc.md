---
description: A steady-state frame allocates nothing on the heap
argument-hint: "[selftest]"
allowed-tools: PowerShell, Read, Grep, Glob
---

Measure a window of genuinely steady frames in the running game (~2 min).

- no argument → `.\tools\CheckAll.ps1 -Only alloc`
- `selftest` → `.\tools\CheckAll.ps1 -Only alloc -SelfTest` (must FAIL)

## What it is guarding

ARCHITECTURE.md's memory strategy: a steady-state frame allocates nothing. The
party stands still on purpose — player-driven **events** (a bump message, a
level line) legitimately allocate, so the assertion is about frames where
nothing happened, which is where zero is unambiguously right.

## Reading a failure

`alloctest RESULT=FAIL` prints the violating call sites with symbolized stacks,
also in `dungeon.log`. Each **unique** stack is reported once per session, so a
standing violation cannot drown the log — a frame repeating a known stack stays
silent.

Two policies that look like bugs and are not:

- **Event frames are reported but not asserted on.** Allocation proportional to
  events is not what the rule forbids. They are deliberately *not* wrapped in
  `alloc::Excused`, because that would also hide something allocating every
  frame.
- **Debug allocation counts are not release counts.** MSVC iterator debugging
  makes `vector`'s move constructor allocate, so growth copies rather than
  moves. Do not compare a debug number against a release one.

The self-test arms `allocpoke`, which allocates every frame on purpose, and
requires the run to come back FAIL.
