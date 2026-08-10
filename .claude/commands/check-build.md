---
description: Both configs compile clean — including release, which rots unwatched
argument-hint: "[debug|release]"
allowed-tools: PowerShell, Read, Grep, Glob
---

- no argument → both: `.\tools\CheckAll.ps1 -Only build-debug,build-release`
- `debug` → `.\tools\CheckAll.ps1 -Only build-debug`
- `release` → `.\tools\CheckAll.ps1 -Only build-release`

Incremental builds are seconds; a clean release build is minutes.

## Why release is in here

It is the config that rarely gets built during ordinary work, so it is the one
that rots silently — a debug-only include, a template that only instantiates
under `NDEBUG`, an `assert`-shaped side effect. The quick tier builds debug
only; this is how release gets exercised without waiting for the full suite.

## Reading a failure

Full output is in `%TEMP%\checkall-build-<config>.txt`. Report the first real
error rather than the last line — MSVC template errors cascade, and the tail is
usually the least informative part.

Note that `build.cmd` must be invoked as `.\build.cmd`; a bare `build.cmd` is
not found because `NoDefaultCurrentDirectoryInExePath` is set (the same setting
behind the harmless `vswhere.exe` warning documented in CLAUDE.md).

No self-test mode, and it does not need one: a broken build produces compiler
errors, not a confident green table.
