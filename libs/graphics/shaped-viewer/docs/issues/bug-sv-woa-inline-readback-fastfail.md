---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/tests/openpbr-bsdf-test.cc
  - file: libs/graphics/shaped-viewer/tests/volumetric-furnace-test.cc
  - file: libs/graphics/shaped-viewer/tests/pathtraced-view-test.cc
  - file: libs/graphics/shaped-viewer/tests/quadric-trace-test.cc
  - file: libs/graphics/shaped-viewer/tests/decal-trace-test.cc
  - file: libs/graphics/shaped-viewer/tests/pt-support-probe-test.cc
  - file: libs/graphics/shaped-viewer/tests/runtime-probe-test.cc
  - file: docs/bugs-external/warp-arm64-pac-fastfail/readme.md
  - test: shaped-viewer-test
---

# sv's Windows-on-ARM skips are very likely WARP's fastfail, not the inline readback path

Every sv test that reads back through `cmd.download` is skipped on Windows on ARM, under `CC_ARCH_ARM64 && _WIN32`.
They name the inline readback path, which is what bisecting with flushed markers pointed at, before anyone had a stack.

## What it turned out to be

The Windows ARM CI image has no GPU, and its Basic Render Driver enumerates once without DXGI's software flag.
sg took it for hardware until it learned that Microsoft's vendor id is always WARP, so sv's "hardware" tests ran on WARP there.
That WARP fastfails out of its pixel JIT: `0xC0000409` with no output, past every handler, a few seconds in.
[warp-arm64-pac-fastfail](../../../../../docs/bugs-external/warp-arm64-pac-fastfail/readme.md) has the stack and what does not trigger it.
The Windows ARM job runs no dx12 test since, as every Windows job but Windows Clang already did.

The skips are very likely that fault: the same silent exit, on the same adapter, with no vendor driver in the process.
They stay until a run on Windows on ARM with a real GPU says otherwise, since CI can no longer reach them.

## Re-checking

On a Windows on ARM machine with a GPU, drop the skips and run sv's tests: that answers whether the readback path ever had a fault of its own.
With `SC_DX12_ADAPTER=warp` the same run says whether WARP is fixed.
