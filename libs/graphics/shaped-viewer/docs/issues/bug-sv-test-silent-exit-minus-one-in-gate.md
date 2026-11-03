---
created: 2026-11-03
priority: next
refs:
  - test: shaped-viewer-test
---

# shaped-viewer-test exits with -1, silently, partway through a gate's run

Four times on 2026-11-03, in the relwithdebinfo and debug-nopch presets of `dev.py check`, the binary ended with exit code 0xFFFFFFFF.
It ended after 16 to 84 s, each time during different tests, and wrote no results file.

Ruled out, measured:
- clean-core's crash handler: no report, and the crashes' `incoming` folders are empty.
- dev.py's timeout kill, which writes a TIMEOUT line: none.
- The memory guard's caps: 77 of 125 GB free at the time.
- A driver reset or a Windows Error Reporting entry: the System and Application logs hold no warning or error in the window.
- shaped-core's own code: nothing in it exits with -1.

Not reproduced outside the gate: alone, four repeats in debug-nopch, the full suite, and alone under a parallel full build all pass.

Hypothesis, unverified: something outside shaped-core calls ExitProcess(-1) or TerminateProcess(..., -1), the GPU driver's user-mode part or another process.
Next: Windows' silent process exit monitoring (gflags, SilentProcessExit) names the terminating process and can dump; it needs an administrator.
