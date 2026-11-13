---
created: 2026-11-07
refs:
  - test: symbolize - a module on an unreachable path resolves without waiting for the network
  - file: libs/base/clean-core/tests/platform/symbolize-test.cc
closed: 2026-11-07
resolution: fixed
---

# The unreachable-path symbolize test fails about half the time locally, at 10.3 s

What is wrong or wanted, and what is already known.

On 2026-11-07, on this Windows machine, `uv run dev.py test clean-core-test` failed this test's `seconds < 10.0` in 6 of 10 runs, every time at 10.25 to 10.49 s.
Alone, with `--repeat 3`, it takes 113 ms, so the time is spent waiting rather than working.
The test's own comment allows 10 s for queuing behind another test's symbol load on the DbgHelp lock, and a `--sample` run put 76% of its time in `SymLoadModuleExW`, under `cc::symbolizer::resolve`.

What is excluded: c2ec6db41, the crash handler's second-abort wait.
With it reverted the test failed 1 of 4 runs, and with it 5 of 6, which is no difference at these counts.

It failed in this session's ships twice today, where it had been a rare flake before.
Recent clean-core and nexus commits beside c2ec6db41 are b07eade63, 4062f5d15, d26a52b47 and 0e45e8e6b, file-access recording and the runner report's sampling, and none of them was tested against it.
Which test holds the lock, and for what, is not known.
That the 10.3 s is one constant suggests a single fixed-cost load rather than contention that varies, but that is inference.

## Cause, measured on 2026-11-07

The lock holder is `symbolize - a module table with no usable binaries still names the module`.
It registers `C:/nowhere/ghost.exe`, a local path with no file behind it, and its first resolve makes DbgHelp search for that image.
The sample shows the stack: `SymFromAddr`, `SymLoadModuleExW`, `FindExecutableImageExW`, `EnumDirTreeW`, 78% of the binary's busy time.
That search is recursive through the session's search path, which is `.` when `SymInitialize` is given none, and dev.py runs every test binary from the repository root.

Calling `FindExecutableImageEx` for `ghost.exe` directly took 18.6 s from the main checkout's root, which holds its agents' worktrees, and 0.25 s from a fresh worktree's.
That is why the main checkout failed and a worktree passed: the same binary passed from a worktree's build.
The worktree's own failed once junctions to twenty other worktrees made its root as large, at 4.95 s.
The unreachable-path test is only where the wait showed, queued on the lock the walk held.

## Fix

`cc::symbolizer` no longer registers a module whose local path has no file, as it already did not register one whose path is not locally reachable.
Without the image there is no debug record to find a PDB by, so the walk could only ever come back empty, and `resolve` names the frame from the table either way.
The absent-image test now also bounds its own time at 1 s.
With the junctions in place, it passed, and `clean-core-test` passed 20 of 20 runs at 2.1 s each.

Not changed: the sessions still search `.` for a PDB a present image names, and whether that search recurses was not measured.
