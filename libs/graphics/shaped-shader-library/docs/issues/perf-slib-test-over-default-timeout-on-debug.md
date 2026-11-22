---
created: 2026-11-08
refs:
  - test: shaped-shader-library-test
  - file: libs/graphics/shaped-shader-library/tests/sgl-real-shaders-test.cc:294
---

# shaped-shader-library-test takes 126 s on debug-clang, past dev.py's default 60 s timeout

On `debug-clang` the binary times out under the default `dev.py test` timeout, so `check --all-presets` reports it as failed though every test passes.
Measured on Windows at `a7ba8ff45`, 12 jobs:

* Default timeout: killed at 60 s, with "slib - a device's micromaps and reordering leave only the named gaps on either route" and
  "slib - the viewer's tracer and the traced drawings lower on both writers, each past its validator" still running.
* `--timeout 600`: all 250 tests pass in 126 s wall.
  The slowest: the viewer's tracer test 125 s, the micromaps test 102 s, "sgl signature - DXIL's packer places every element of the repo's SGL as DXC does" 51 s,
  then two at about 20 s.

Each compiles every fixture through SGL, DXC and the validators; a debug build pays for that many times over.
Options: narrow them under `nx::is_thorough()` or `nx::is_smoke()`, or give the binary a longer declared timeout.

## 2026-11-22: the sweeps run per entry point

The two slowest tests now compile one task per entry point and hand spirv-val one batch instead of a process per module.
That also fixed the relwithdebinfo stall under load, where the binary passed the watchdog's limit beside the whole suite.

Measured on Windows at `b84a2a4e9`: all 267 tests pass on `debug-clang` in 67.5 s wall, at 8.5 cores.
The long pole is now "sgl signature - DXIL's packer places every element of the repo's SGL as DXC does", 66 s,
then "slib - a vulkan entry's draw start as builtins is the SPIR-V DXC made of the system values before (EMIT-128)", 53 s.
Both still compile serially, so the same split is what is left; on relwithdebinfo the binary takes 9.6 s.
