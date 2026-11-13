---
created: 2026-10-29
refs:
  - file: libs/base/clean-core/src/clean-core/platform/symbolize.cc
  - test: "symbolize - *"
---

# clean-core-test hangs on the VS2026 CI runner with several symbolize tests in DbgHelp

The Windows MSVC (VS2026) job's clean-core-test tripped nexus's 30 s no-progress watchdog twice on main, two days apart.

- run 40483658126 (306628d0b, 2026-10-27): running "symbolize - the same address answers the same way, from the cache".
  Beside it ran "stacktrace - current() reports frames wherever the toolchain can render them" and "symbolize - a module table with no usable binaries still names the module".
- run 40850074084 (fdbefff67, 2026-10-29): running the stacktrace test.
  Beside it ran "symbolize - a module table with no usable binaries still names the module" and "symbolize - a recorded module table resolves addresses this process did not produce".

Both reports say another thread was inside DbgHelp, so the minidump and the other threads' stacks were skipped.
`symbolize - a module table with no usable binaries still names the module` is in both.

Every DbgHelp call in clean-core takes `dbghelp_lock()`, so this reads as one call holding it past 30 s rather than a lock-order deadlock.
A symbol load or a network lookup on the hosted runner is the likely long call; the VS2022 and clang Windows jobs have not shown it.
Nothing reproduces it off Windows, and the next step needs a Windows machine: time each symbolize test's DbgHelp calls on the runner, or log `_NT_SYMBOL_PATH` there.

**Likely the cause, found locally on 2026-11-07** ([bug-cc-symbolize-unreachable-path-flaky-locally](.closed/2026/11/bug-cc-symbolize-unreachable-path-flaky-locally.md)).
`symbolize - a module table with no usable binaries still names the module` registered a local path with no file.
Its first resolve made DbgHelp walk the working directory recursively for the image, holding the lock every symbolizer shares.
CI runs from the repository root with its build tree, and that test ran beside each failure here.
The symbolizer no longer registers such a module, but no CI run has shown the failures gone yet, so this stays open until one does.
