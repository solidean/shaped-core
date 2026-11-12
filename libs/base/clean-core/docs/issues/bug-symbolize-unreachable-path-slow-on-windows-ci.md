---
created: 2026-10-27
refs:
  - file: libs/base/clean-core/tests/platform/symbolize-test.cc
  - test: symbolize - a module on an unreachable path resolves without waiting for the network
---

# symbolize test of an unreachable module path takes 33 s on Windows CI

On CI's Windows MSVC (VS2026) leg, run 40476619703 on PR 235, the test measured 33.5 s against its 10 s bound (`symbolize-test.cc:350`).
The nexus watchdog had seen no test progress for 30 s by then, and stopped `clean-core-test` with exit code 4.
Two other DbgHelp users were running beside it.
One was `symbolize - an address in no module resolves to nothing, and says so`.
The other was `stacktrace - current() reports frames wherever the toolchain can render them`.
The rerun of the failed job is the only other data point so far; the same workflow is green on `main` around it.

Two hypotheses, neither measured.
One network timeout on `\\192.0.2.1\share\ghost.exe` may still happen despite the test's premise.
Or the test queues behind another test's symbol load on the DbgHelp lock for longer than the 10 s its comment allows.
A trace of where the 33 s went (time on the DbgHelp lock vs. inside a file open) decides between them.

**Likely the cause, found locally on 2026-11-07** ([bug-cc-symbolize-unreachable-path-flaky-locally](.closed/2026/11/bug-cc-symbolize-unreachable-path-flaky-locally.md)).
`symbolize - a module table with no usable binaries still names the module` registered a local path with no file.
Its first resolve made DbgHelp walk the working directory recursively for the image, holding the lock every symbolizer shares.
CI runs from the repository root with its build tree, and that test ran beside each failure here.
The symbolizer no longer registers such a module, but no CI run has shown the failures gone yet, so this stays open until one does.

Sighting, 2026-11-12, locally under a full gate: `symbolize - a module table with no usable binaries still names the module` took 1.14 s against its 1 s bound and failed; a rerun passed.
