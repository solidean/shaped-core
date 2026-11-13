---
created: 2026-11-13
refs:
  - file: libs/graphics/shaped-viewer/tests/headless-viewer-test.cc:1200
  - test: "sv - a guest settled render advances no epoch of its own, and settles in its host's frames"
---

# A guest settled render draws nothing on metal, and the run then hangs

On an Apple M4, relwithdebinfo, the test fails every time, run alone too (seed 1534530569865945033).
The render settles after about 4 s, but no pixel of the quad is covered: `covered` is 0 against 256.
The `settled` and epoch-count checks pass; only the image is empty.
The run then hangs with no tracked work outstanding, until the watchdog ends it at half sv's timeout, 300 s.
It came in with 3419bddba on 2026-11-12, which added the guest mode; on dx12 it is unknown.
Crash reports: `build/macos-arm-llvm-relwithdebinfo/crashes/shaped-viewer-test-20261113-111818-12180` (full suite) and `-112858-13690` (alone).
