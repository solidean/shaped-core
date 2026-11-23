---
created: 2026-11-23
refs:
  - test: sg - an atomic update reaches the element of an SGL binding array a thread picks
  - file: libs/graphics/shaped-graphics/tests/compute/compute-sync-test.cc
---

# An atomic update into an SGL binding array, and the SGL differential test, fail on dx12 and vulkan at 6b08b3c91

Measured (2026-11-23, Windows, the hardware adapter, `relwithdebinfo-clang`): `shaped-graphics-test` fails 5 of 2217 tests, 8 checks, deterministically, at 6b08b3c91.
- `sg - an atomic update reaches the element of an SGL binding array a thread picks`, on dx12-hw, dx12-native-dxil, vulkan and vulkan-native-spirv:
  `compute-sync-test.cc:215`, `CHECK(got_even[0] == count / 2)`.
- `sg differential - every corpus entry point computes on the GPU what SGL's interpreter says`: `differential-test.cc:41`.

`uv run dev.py test "atomic update reaches the element"` reproduces it in under a second.
The failed run's logs were kept under `build/<preset>/crashes/shaped-graphics-test-20261123-161031-failed` (not copied out; a clean of `build/` loses them).
Not yet bisected: which of the commits since d4e672a54 brought it in is unknown.
