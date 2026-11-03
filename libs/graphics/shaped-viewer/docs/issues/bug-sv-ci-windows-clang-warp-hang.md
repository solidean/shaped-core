---
created: 2026-11-02
tags: [flaky, ci]
refs:
  - file: .github/workflows/ci-windows-clang.yml
  - test: "sv dx12 - warp"
---

# shaped-viewer-test hangs on the Windows Clang CI's WARP run, in a different test each time

Every Windows Clang run on main on 2026-11-02 failed in shaped-viewer-test: nexus' watchdog killed it after no test started or finished for 300 s (exit code 4), once an access violation instead.
The test awaited under "sv dx12 - warp" differs per run:
- run 41482062516: "sv - a capture that times out writes beside the requested path, not to it";
- run 41504898877: "sv - a view accumulates across frames under its id";
- run 41522700574: "sv - a capture writes a complete image and ends the loop".

In every report the main thread waits in `participate_until_ready` under `nx::execute_tests`, the pool's workers are idle, and the watchdog lists no tracked work in flight.
So something the test awaits is never resolved: hypothesis, a frame loop or capture waiting on a GPU fence WARP never signals, or an untracked wake that is lost; not shown.
- Locally (Windows, relwithdebinfo-clang, hardware adapter) the binary passes in 76 s; WARP runs there only under `--thorough`, so the default local run never takes this path.
- The same runs saw clean-core-test exit 4 twice (41494273936, 41516332305), also a watchdog kill; whether it shares the cause is not known.
- The diagnostics artifact keeps the crash report and minidump under `crashes/incoming/`.
