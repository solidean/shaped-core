---
created: 2026-11-14
refs:
  - file: libs/graphics/shaped-rendering/tests/backends/dx12-entry.cc:103-104
closed: 2026-11-26
resolution: fixed
---

# Running one sr test by name never reaches the dx12 native-DXIL context

What is wrong or wanted, and what is already known.

`tests/backends/dx12-entry.cc` registers the single-test aliases of two dx12 contexts alone:

```cpp
static bool const sr_dx12_warp_registered = sr_test::register_backend("sr dx12 - warp", "dx12-warp");
static bool const sr_dx12_hw_registered = sr_test::register_backend("sr dx12 - hardware", "dx12-hw");
```

The binary also runs every invocable test on `sr dx12 - native-dxil` (`dx12-native-dxil`, SGL's own DXIL on the hardware adapter), which is the default route.
Running one test by name therefore never reaches that context, though the whole binary does, so a failure only it shows cannot be reproduced alone.
bug-sr-svgf-half-float-saturation-native-dxil-amd is one: it needed a temporary third alias to run by itself.

A fix is likely the missing alias, `sr_test::register_backend("sr dx12 - native-dxil", "dx12-native-dxil")`; whether the text-route context beside it needs one too is to be checked.
Seen on 2026-11-14, on the desktop, at jn/sgl-sm66-floor and at its merge-base 30b13520f alike.

## Log

**2026-11-26: fixed, both native contexts are aliased.**
`dx12-entry.cc` registers `sr dx12 - native-dxil`, and `vulkan-entry.cc` the same gap's twin, `sr vulkan - native-spirv`.
A test run by name now reaches all four hardware contexts, which `--repeat` of the svgf test showed.
