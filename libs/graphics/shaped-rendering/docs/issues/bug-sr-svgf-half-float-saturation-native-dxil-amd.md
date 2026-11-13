---
created: 2026-11-14
refs:
  - file: libs/graphics/shaped-rendering/tests/denoise-test.cc:985-1030
  - file: libs/graphics/shaped-rendering/tests/backends/dx12-entry.cc
---

# On AMD's dx12 native-DXIL context, svgf saturates no pixel of a half-float output

What is wrong or wanted, and what is already known.

`sr - atrous and svgf saturate a pixel past a half float's range in a half-float output` fails for svgf on the `sr dx12 - native-dxil` context:

```
check failed off-thread: saturated == k_size * k_size — 0 == 1024 | member: "svgf" | saturated pixels: 0
```

It is SGL's own DXIL (`native_dxil = true`) on the hardware adapter, which on the desktop is the AMD Radeon iGPU (RDNA 2, driver 32.0.21042.62).
It fails every run, and it fails the same way at 30b13520f, the merge-base of jn/sgl-sm66-floor, so no change of that branch brought it.
The failing check names svgf alone; atrous, which the same test checks the same way, does not fail there.
So every svgf output pixel the test drives past a half float's range comes back short of 65504, or not written.

Not yet known: what svgf writes there instead, whether the text route's context or another GPU fails too, and whether SGL's DXIL or AMD's driver is at fault.

## Reproducing

Running the test by name reaches only the warp and hardware contexts (bug-sr-single-test-alias-skips-native-dxil).
Add, temporarily, beside the two aliases in `tests/backends/dx12-entry.cc`:

```cpp
static bool const sr_dx12_native_registered = sr_test::register_backend("sr dx12 - native-dxil", "dx12-native-dxil");
```

then `uv run dev.py test shaped-rendering-test --test-args "saturate a pixel past a half float"`, or run the whole binary, whose `sr dx12 - native-dxil` test runs it.
