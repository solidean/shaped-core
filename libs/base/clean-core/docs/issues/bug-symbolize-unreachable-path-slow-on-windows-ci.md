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
