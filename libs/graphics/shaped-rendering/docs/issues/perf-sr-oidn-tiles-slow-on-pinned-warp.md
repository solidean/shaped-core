---
created: 2026-10-28
refs:
  - file: libs/graphics/shaped-rendering/tests/oidn-network-test.cc
  - test: sr - the OIDN network in tiles agrees with the same image run whole
---

# The OIDN tiles test runs about 25x slower on the pinned NuGet WARP than on Windows' own

On the pinned WARP 1.0.21 (extern/warp), this one test takes about 50 s on a 16-core desktop, warm and repeatably, against 2 s on Windows' own WARP 10.0.26100.9711.
On CI's Windows Clang runner it took 21.6 s on the system WARP and ran past nexus' 90 s no-progress watchdog on the pinned one, failing the binary.
The WARP's worker threads were executing JIT-compiled shader code when the watchdog fired, so it is slow rather than hung.
Every other WARP test ran within its time on CI.

Measured: the two numbers above, on the same build, with only the staged d3d10warp.dll swapped.
Not known: which of the network's passes is slow, and whether it is one shader construct the new WARP handles badly.

Meanwhile the test skips on the pinned WARP, `TEMPORARY(perf-sr-oidn-tiles-slow-on-pinned-warp)` in the test.
It still runs on hardware and on the system WARP.
Wanted: profile the passes on the pinned WARP, then either avoid the slow construct or report it to Microsoft and add it to docs/bugs-external.
