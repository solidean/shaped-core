---
created: 2026-11-28
refs:
  - issue: feat-sgl-interpreter-bounds-builtin-precision
  - issue: feat-sgl-interpreter-texture-fidelity
  - issue: feat-sgl-interpreter-bounds-textures-and-interpolation
---

# Bounding runs good enough to bound a host's twin tests: store rounding, reordered products, a whole dispatch

**Status: a proposal for review.** Nothing here is decided or built.
Philip and Julius review it; any of the three changes may be dropped or reshaped.

## Where this comes from

Quest3D compares each shader's SGL version against its old HLSL version (its "twins") on the GPU, within a tolerance picked by hand per test (`Ulps(1)`, say).
It tried replacing that tolerance with a bounding run (`run_limits::bounds_floats`): interpret the SGL entry on the test's inputs and require both twins' GPU outputs inside the range.

The prototype is the vignette post pass: a compute entry with one integer `load`, `pow`, and a store to an `rgba16_float` image, no samplers, no workgroup memory.
On an RTX 5080, D3D12 and Vulkan alike, both twins land outside the range on up to 1,697 (HLSL) and 683 (SGL) of 6,771 colour components, by one half step at most.
The counts barely change between `.fast` and `.precise`, so the departures the level allows are not the cause: `pow`'s own precision (feat-sgl-interpreter-bounds-builtin-precision) and the store's rounding are.
The prototype: Quest3D's `core/gfx/test/sgl_interp_bounds_proto_test.cpp` (worktree branch `sgl-interpbounds`, not landed).

## Proposed

### 1. A store to a float image format rounds to a range

A store to an `f16` (or other narrower float) format keeps the exact float today (feat-sgl-interpreter-texture-fidelity).
Proposed: the stored range covers every rounding EVAL-96 allows, to nearest and toward zero.

```
img.store(p, float4(1.0 - 1e-7))   // image_2d[.rgba16_float]
// interpreter today: 1.0     a device rounding toward zero (D3D): 0.99951
```

### 2. A chain of products widens as a chain of sums does

`.fast` allows any reassociation (EVAL-6), but the bounding run widens only `+`/`-` chains and `dot`.
Proposed: a product of k factors widens by a relative error of at most (1+u)^(k-1) when the level allows reassociation.

```
b.out[0] = b.x[0] * b.y[0] * 100.0
// DXC may emit b.x[0] * (b.y[0] * 100.0); the range holds only (x*y)*100, rounded once per step
```

The vignette hits it: DXC reorders `(dist * 100) * amount^3`.

### 3. A whole dispatch without copying per invocation

Each invocation copies every bound texture and returns whole images, so a test's cost grows with the image's area squared.
Measured: about 32 us per invocation plus 3.4 ns per texel of the bound images; a 256x256 pass projects to about 17 s per configuration.
Proposed: run a dispatch over shared textures, images written in place, each invocation reading the inputs without a copy.

## Not proposed here

- Builtin precision: feat-sgl-interpreter-bounds-builtin-precision. Cancelling a driver's `pow` error between the twins was tried and is not enough; twin tests need the measured table.
- Texture coordinates, filter weights and interpolants at hardware precision: feat-sgl-interpreter-bounds-textures-and-interpolation. Most of Quest3D's post effects need it.

## Open

- The order: for Quest3D, store rounding and builtin precision come first; the dispatch matters once a test is larger than about 128x128.
- Whether product widening (2) applies at `.fast` only, or also where a backend contracts without reassociating.
