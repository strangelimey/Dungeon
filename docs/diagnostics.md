# Diagnostics: exceptions, stalls and thread health

**Status:** BUILT (Michael, 2026-08-10), branch `exception-handling`. All six
phases are done and checked:

```
diagtest   RESULT=PASS checks=35 failures=0     # the record, incl. torn-read detection
healthtest RESULT=PASS cases=7  failures=0      # the real game, broken on purpose
healthtest RESULT=FAIL ... self_test=1          # and the harness proven able to fail
alloctest  RESULT=PASS frames=1921 violations=0 # the symbolizer lift changed nothing
```

Left undone deliberately: the `Killed` event kind has no scripted coverage (a
hard kill is a panel button, not a command), and balance/tuning of the log rate
limit is a matter of living with it.

When the game dies, it should say why. Today it does not: it disappears, and
the log stops mid-sentence. This is the machinery that replaces that silence —
one record of every way a thread can fail, captured at every boundary a failure
can cross, readable live and after the fact, and **checked by a harness that
breaks the game on purpose** rather than by reading the code and believing it.

The requirement, in Michael's words: *if a process or thread crashes or hangs
and I don't know why, this system has failed.*

## Diagrams

Drawn in the tech-diary house style (light theme, 760 wide), so they carry
across to the diary entry unchanged when this thread lands.

| | |
|---|---|
| [diagram_thread_wiring.svg](diagram_thread_wiring.svg) | Ownership and registration — who spawns workers, what the Manager holds, what a thread registers with on entry, and why that storage belongs to the registry. |
| [diagram_thread_tick.svg](diagram_thread_tick.svg) | One worker's tick loop inside `Manager::Run`, annotated with what each step buys and where today's safety net begins and ends. |
| [diagram_thread_states.svg](diagram_thread_states.svg) | The seven stored states and the derived `Stalled` view, the supervisor, and the 250 ms grace behind `Kill`/`Restart`. |

## Why

Five things are true of the code as it stands, and each is a hole rather than
something merely worth improving.

- **There is no top-level handler at all.** `wWinMain` (src/Main/Main.cpp) has
  no try/catch, no `SetUnhandledExceptionFilter`, no `set_terminate`, no dump
  writer. A C++ exception escaping `game.Update` reaches `std::terminate` and
  the process vanishes with no line in the log. An access violation vanishes
  the same way, and that is the majority of what actually kills a game.
- **Worker crash capture exists, but keeps no evidence.** `Manager::Run`
  catches a throwing job and stores `w->lastError = e.what()` — a bare string,
  **overwritten every tick and never logged**. A worker throwing on every tick
  shows one string in the console panel and leaves nothing in `dungeon.log`.
  There is no iteration number, no time, no stack, no count.
- **Stalls are derived, never recorded.** `State::Stalled` is computed inside
  `Inspect` — a tick still running past `watchdogMs`. Nothing records that a
  stall *happened*, so it has no history at all; `restarts` is a bare counter
  with no when and no why. A worker that stalled, was rebooted by the
  supervisor and is now healthy looks exactly like one that has been fine all
  along.
- **The profiler is compiled out of ordinary builds.** `DN_PROFILE` is set only
  by the `debug-profile` / `release-profile` presets. Crashes happen in plain
  debug and release, so the health record cannot live behind that gate.
- **The stack walker already exists, in the wrong place.** `Core/AllocTrack`
  has `RtlCaptureStackBackTrace` + `SymFromAddr` for its violating-frame
  report. That wants lifting into a shared `Core/StackTrace`, not writing
  twice.

## Decisions taken

Settled before any code, because each changes the shape of the work:

1. **Scope: C++ exceptions, SEH, and a minidump.** Access violations,
   divide-by-zero and stack overflow are caught alongside thrown exceptions,
   and a `.dmp` lands beside `dungeon.log` so a crash can be opened in the
   debugger after the fact. Exceptions-only would miss most real crashes.
2. **Always compiled in.** `Core/Diagnostics` is in plain debug and release,
   not behind `DN_PROFILE`. A fixed ring per thread costs bytes, and the work
   happens on the failure path, so a healthy frame pays nothing. The profiler
   timeline (phase 5) stays profile-only, as one *view* of an always-present
   record.
3. **Main-thread policy: keep running, die after N repeats.** A worker that
   throws keeps running, as it does today. The main thread does too — but the
   same call site throwing on N consecutive frames is treated as
   unrecoverable, and takes the clean-exit path (full report, flushed log,
   deliberate shutdown). An isolated glitch is survived; a persistent one is
   not spun on forever, and a frame loop that throws every frame cannot produce
   thousands of identical reports.

## The shape

### One record

`Core/Diagnostics` owns a per-thread ring of typed events:

| kind        | raised when                                              |
|-------------|----------------------------------------------------------|
| `Exception` | a C++ throw crossed a managed boundary                    |
| `Fault`     | an SEH fault (access violation, divide-by-zero, overflow) |
| `Stall`     | a tick outran its watchdog                                |
| `Restart`   | a worker was rebooted (supervisor or manual)              |
| `Killed`    | a worker was force-terminated and quarantined             |
| `Fatal`     | an assert, a terminate, or the repeat limit reached       |

Each event carries wall time, TSC (so it sits on the profiler's timeline),
the worker id and iteration, a message, and a captured stack. Fixed capacity,
no allocation — the steady-state rule applies to the failure path too, since
the failure path is exactly where the heap is least trustworthy.

**Storage is owned by the registry, not `thread_local`** — the same argument
`Core/Profile.h` makes for its collectors. `ThreadManager::Kill` force-
terminates with `TerminateThread`, which runs no destructors and frees the
thread's TLS out from under anyone holding it. A killed worker's last event is
precisely the evidence worth keeping, so it must not live in storage the kill
destroys.

### Capture at every boundary

The try-catch in `Manager::Run` is the model; the work is to make it record
properly and to extend it to the boundaries it does not cover.

- **Workers** — the existing catch records a full event and logs it once, with
  a repeat count so a per-tick thrower does not flood the log.
- **The supervisor** — records `Stall` with the age that tripped it, and
  `Restart` when it reboots.
- **`Kill`** — records `Killed` before quarantining the slot.
- **The main thread** — gets a health slot of its own and the same treatment
  around the frame body, plus the repeat counter that decides when to stop.
- **The process** — `SetUnhandledExceptionFilter` for faults, `set_terminate`,
  and `DN_ASSERT` routed through the record before it aborts. This is what
  turns "it vanished" into a report.

### Stacks worth reading

The one genuinely tricky part. At a `catch` site the stack has **already
unwound**, so the throw point is gone and the stack you capture there is
useless. A **vectored exception handler** runs at throw time, before unwind,
and is the only way to get the real stack for a C++ throw. That handler
records the stack; the catch site attaches it to the event.

For a stalled thread the question is different — it is still running, and the
answer is to walk it from outside: suspend, `StackWalk64`, resume. That is
what answers *what is it stuck on*, and it is the heart of the probe.

### Two readouts

- **Live** — health columns on the console THREADS panel, and a `health
  [worker]` command listing recent events. A stalled worker can be probed on
  demand for its current stack.
- **Over time** — health events as marks on the per-thread rows the profiler's
  graph view already draws, clickable for the detail. The GPU work set the
  precedent: a new source becomes one more row in the readout that exists,
  never a panel of its own.

## Phases

Each phase lands with the injection command that breaks it on purpose, so
nothing is built untested.

1. **The record — DONE.** `Core/Diagnostics`: the six event kinds, a per-thread
   ring of 16 events (32 slots, 32-frame stacks, 192-char messages), registry-
   owned storage, cross-thread `RecordFor`, the merged newest-first view, and
   throttled log output. Checked by `tools/DiagTest` (build, then run
   `DiagTest.exe`; one verdict line, exit 0 = PASS).
2. **Capture — DONE.** The worker catch records a full event (kind, worker id,
   tick, message) instead of overwriting a string; the supervisor records the
   stall AND the reboot as two separate facts; `Kill` records against the
   victim's timeline; the main thread has a slot, a frame try/catch and the
   die-after-10-consecutive policy. `Core/CrashHandler` adds what no catch can
   see — `SetUnhandledExceptionFilter` for SEH faults, `set_terminate`,
   `DN_ASSERT` routed through `ReportFatal`, and minidumps (capped at 3 a run).
   Injected with the `crashpoke <throw|worker|fault|assert>` dev command and
   read back with `health`. Measured against the running game:

   | injection | before | after |
   |---|---|---|
   | main-thread throw | process vanished | recorded, logged, **game keeps playing** |
   | access violation | process vanished | `fault on 'main': access violation writing 0x0 at 0x7ff7…` + 33.6 MB dump |
   | worker throws every tick | one overwritten string, no log | every tick recorded with its number; worker keeps running |
   | assertion | log line, then a CRT dialog | FATAL recorded + dump written **before** `abort()` |
3. **Stacks — DONE.** `Core/StackTrace` holds the capture, the DbgHelp
   symbolizer (serialized — DbgHelp is single-threaded by contract), the
   `SeenSet`, and two walkers the record could not get any other way:

   - **Throw-time capture.** A vectored exception handler installed FIRST in the
     chain records the stack of every C++ throw (`0xE06D7363`) on the throwing
     thread, before any unwinding. Catch sites read it back through
     `ThrowFrames`. Measured: a `crashpoke throw` now names
     `Game_DevCommands.cpp:275` — the `throw` itself — where the catch site is
     `Main.cpp:131`.
   - **Fault-context walk.** `StackWalk64` over the `CONTEXT_RECORD`, since a
     filter's own stack says only that a filter ran. Measured: a null write
     names `Game_DevCommands.cpp:299`, the faulting line. Run LAST on the crash
     path, after the record and the dump, because it is the riskiest step there.

   `AllocTrack`'s private symbolizer is gone (~45 lines) and it now shares this
   one, keeping its own `SeenSet` so crash sites and allocation sites cannot
   mask each other. A stack is logged ONCE per distinct site, or a repeating
   failure would undo the rate limit.
4. **The probe — DONE.** `health probe <id|name>` suspends a live worker, walks
   it and resumes — the answer to "what is it stuck on", which the record alone
   can never give because a stalled thread has thrown nothing. Measured against
   a wedged worker:

   ```
   probe 'demo.wedged' #6 [stalled] tick 0, beat 5159 ms ago:
     ZwDelayExecution → SleepEx → sleep_for
     Game_DevCommands.cpp:260        <- the line it is stuck on
   ```

   The walk uses `RtlLookupFunctionEntry` + `RtlVirtualUnwind`, **not**
   `StackWalk64`, because StackWalk64 takes DbgHelp's lock and the thread you
   just froze might be the one holding it — the probe would deadlock the process
   it was meant to diagnose. Symbolizing happens after the resume. Probe output
   goes to `dungeon.log` as well as the console: a probe is evidence, and the
   console scrolls away.

   Unlike a crash report, the probe prints EVERY frame — for a wedged thread the
   OS frame is the diagnosis (`NtWaitForSingleObject` names a lock,
   `NtDelayExecution` a sleep).

   Also here: `health <thread>` (that thread's events with stacks), and a health
   column on the THREADS panel. And the phase-2 gap is closed — stall DETECTION
   no longer rides the reboot path, so a stall on a worker with no `autoRestart`
   is recorded too (once per episode, not once per 100 ms poll).
5. **The timeline — DONE.** A HEALTH section in the console: one strip per
   thread that has failed, sharing the profile graphs' x-axis (240 samples ×
   50 ms = 12 s), marks coloured by kind, click a mark for the event and its
   stack.

   **Not** rows on the profile graphs, as originally planned, for two reasons
   found while building it: those rows are per NODE and health is per THREAD, so
   the granularity does not match; and the profiler is compiled out of plain
   debug and release while the record is not — a crash happens in an ordinary
   build, which is exactly where a timeline is most wanted. Its own strip on the
   same axis gets both.

   A cell keeps the MOST SEVERE kind in its window, not the last, for the same
   reason the profile series keeps the max: the one event worth seeing must not
   be averaged away by the three around it. The section only exists once
   something has gone wrong — a permanently empty strip trains you to skip it.
6. **The harness — DONE.** `tools\HealthTest.ps1`: seven cases, each breaking
   the real game and then reading `dungeon.log`, because that is the surface a
   crash is meant to be found on. Nothing inspects engine internals — if the
   answer is not in the log, it does not count.

   ```
   healthtest RESULT=PASS cases=7 failures=0
   ```

   Cases: `throw` (recorded, game plays on) · `worker` (per-tick, keeps running)
   · `stall` · `probe` (a live wedged worker names its own line) · `restart`
   (supervisor reboot) · `fault` (report + dump) · `assert` (report + dump
   before the abort).

   `-SelfTest` skips every injection and requires the run to come back FAIL.
   Measured: all 7 cases and every single expectation fail, so no check is
   vacuously satisfied by an ordinary run.

   **Not covered, said out loud rather than quietly skipped:** the `Killed`
   kind. A hard force-terminate is the THREADS panel's kill button, not a
   console command, so it cannot be driven from a script.

## The harness

`tools\HealthTest.ps1` follows `AllocTest.ps1` exactly, because that shape has
already earned itself: drive the real game, wait on **log lines rather than
sleeps** so a slow cold-cache load stretches the wait instead of failing the
run, and print one machine-readable verdict.

A `crashpoke <kind>` dev command injects each failure — `throw`, `fault`,
`stall`, `wedge`, `assert` — on the main thread or a named worker. The harness
fires each, then asserts the record came back with the right kind, the right
thread, a non-empty stack and a dump file where one is expected.

`-SelfTest` inverts the verdict, as `AllocTest` and `Bc7Test` both do: the
harness must catch a real injected failure to pass. A checker that cannot be
seen to fail is not evidence.

## What phase 1 changed about the plan

**The log needs a rate limit, not just a repeat-collapse.** The plan said a
repeating failure should be "logged once, with a repeat count". Built that way
— collapse identical consecutive events, log at powers of ten — and the first
concurrency run wrote a **1.2 MB log from 16k events**, because the collapse is
blind to a message carrying a tick number or a coordinate. Every one of those
messages was distinct, so nothing collapsed.

That is the same failure the system exists to prevent, wearing a different hat:
a crash worth finding, buried under a thousand lines of the bug that came
first. So there are now TWO layers — identical repeats collapse to powers of
ten, and *distinct* events are rate-limited per thread (8 lines a second, with
a line saying how many were swallowed). The RECORD still takes every event; only
the log is throttled. Same run afterwards: **2.7 KB**.

The lesson generalises to phase 2, where the capture sites land: throttling has
to key on the THREAD, not on the message, because the message is exactly the
part a failing worker varies.

## Traps

- **The failure path must not allocate.** A report written after heap
  corruption, or from inside a `TerminateThread` window that leaked the CRT
  heap lock, is a second crash on top of the first. Fixed buffers throughout.
- **`DN_ASSERT` calls `abort()`,** which in a debug build raises a CRT dialog —
  the process *looks* alive but is wedged (already noted in CLAUDE.md). The
  report must be flushed **before** the abort, never after.
- **Stack overflow has no stack to walk on.** It needs a guard page and a
  separate handler stack, or the handler faults trying to report the fault.
- **Suspending a thread to walk it can deadlock** if it holds the symbol
  handler's lock. Walk raw addresses while suspended, resume, then symbolize.
- **`DevConsole.cpp` is at 1101 lines** and phases 4–5 both add to it. Split
  the panel rendering out rather than push it toward 1500.
