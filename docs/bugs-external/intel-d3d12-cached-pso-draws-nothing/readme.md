# Intel D3D12: a graphics pipeline restored from the driver's shader cache draws nothing

**Status:** open, not yet filed upstream; the tests that trip it are skipped in shaped-viewer, with no workaround.
**Affects:** Intel Arc B580, driver 32.0.101.9150 (the newest as of October 2026), Windows 11; WARP is not affected.
**Found by:** three `sv::raster_routine` tests in `shaped-viewer-test` failing on hardware dx12 after 8fdd181cf, on the first run after a shader change only.

## What happens

A graphics pipeline is created twice, in two processes, from the same DXIL and the same root signature.
The first process meets a cold driver cache: `CreateGraphicsPipelineState` compiles the pipeline (about 65 ms) and the draw lands.
The second process gets it from the driver's own shader cache (about 1 ms), and the same draw writes nothing.
Every render target still holds its clear value, and the depth buffer too.

Not every time: about one fresh compile in eight writes a cache entry that restores correctly.
Those fresh compiles take twice as long, 124 to 152 ms against 64 to 79 ms, over 30 attempts with the shader here.
So the driver seems to compile this pipeline one of two ways, and only the faster one's cache entry is broken.

No call fails, the device is not removed, and the D3D12 debug layer reports no error.
The cache is the driver's: `LocalLow\Intel\ShaderCache` is written on the fresh run and read on the restore.
No persistent cache of the application is involved, and the D3D cache (`Local\D3DSCache`) moved aside changes nothing.

## Why it is not our bug

`repro.cc` is D3D12, DXGI and the C++ standard library; the shaders are HLSL compiled by the Windows SDK's dxc.
Each attempt is a fresh nonce, so the first draw is always a cold compile, then a second process restores it.
Five attempts per row; every fresh draw landed:

```
adapter                     configuration                                restores that drew nothing
--------------------------  -------------------------------------------  --------------------------
Arc B580, 32.0.101.9150     as found                                     5 of 5
Arc B580, 32.0.101.9150     shaped-core's whole root signature           5 of 5
Arc B580, 32.0.101.9150     without the light loop                       0 of 5
Arc B580, 32.0.101.9150     without the ambient term                     0 of 5
Arc B580, 32.0.101.9150     debug layer and GPU-based validation         0 of 5
WARP                        every row above                              0 of 5
```

The pipeline is the same object in both processes, so a restore that differs from the fresh compile is the driver's.
The debug layer, GPU-based validation included, reports no error on either run, so the pipeline is not a misuse a validation layer can see.
Under GPU-based validation the restore draws, presumably because the patched shaders take another path through the cache.

## What is load-bearing

Found by cutting shaped-core's own pipeline down in a harness of this shape, first by hand, then mechanically:

- **The pixel shader.** A trivial one writing the same eight targets restores correctly.
- **Of its outputs, `color` alone.** Every other target can be a constant, and with `color` constant too it restores correctly.
- **Not any one construct.** The mechanical reduction left the BSDF evaluation in the light loop, about 360 of `pixel.hlsl`'s 800 lines, and each further cut made it reproduce less often.
  Most of the material's parameter loads became constants; the few left feed the lobes that keep that code alive.
- **Both the light loop and the ambient term.** Either one removed and the restore draws, though the loop never runs: the frame has no lights.
  Any one of the three light paths suffices, but a loop with a one-line body does not.
- **Not the vertex shader.** shaped-core's own, which reads `SV_StartVertexLocation`, and the plain one here reproduce alike.
- **Not the root signature's static samplers.** None, 18 at `s0`, or 18 at `s8752` in space 1 reproduce alike.
- **Not register pressure or scratch memory as such.** Synthetic shaders with 256 live `float4`s, or a dynamically indexed 512-float array, restore correctly.
- **Not `NonUniformResourceIndex`.** All 51 uses removed, the restore still draws nothing.

## What the driver's cache keys on

The root signature is part of the key.
The same shaders under a root signature that differs per process, by one unused root constant, compile fresh in both processes and draw in both.

`ID3D12Device9::ShaderCacheControl` cannot turn the cache off instead: it fails with `DXGI_ERROR_SDK_COMPONENT_MISSING` unless Windows Developer Mode is on.

## How `pixel.hlsl` was reduced

From about 1900 lines, by a script that kept a step only if the result still compiled with `-WX` and still reproduced:

- right-hand sides replaced by a constant of their type, in shrinking chunks;
- then every local, struct field and declaration nothing reads, removed with its writes.

So no variable is read uninitialized, which a driver report could not rest on.
The structs a structured buffer holds keep their full layout, so each view's stride matches the shader's.
Folding `if` conditions to constants took it to 700 lines, but reproducing in only four attempts of five, so it stops here.

## Reproducing it

```bash
uv run run.py
```

It builds `repro.cc` with `clang-cl` and compiles `vertex.hlsl` and `pixel.hlsl` with the newest Windows SDK's dxc, at shader model 6.8.
Then it runs each configuration five times, each a fresh nonce compiled and then restored, on the high-performance adapter and on WARP.
It exits 1 when a restore draws nothing where the fresh compile drew, and 2 when a run is inconclusive.

## What shaped-core does about it

No workaround: it skips the tests that trip it, on an Intel dx12 driver up to 32.0.101.9150.
`has_intel_cached_pso_bug` in [raster-test.cc](../../../libs/graphics/shaped-viewer/tests/raster-test.cc) is the gate.
On an affected machine these pass on the first run after a shader change and fail on every run after it:

- `sv::raster_routine - a cutout thresholds at its cutoff, and cuts coverage below one half`
- `sv::raster_routine - a minified textured cutout cuts against level 0`
- `sv::raster_routine - a baked occlusion of one half halves the environment's light`

8fdd181cf did not cause it; it changed the raster pixel stage's bytecode, which is what this trigger depends on.

## Re-checking after a driver update

Run `uv run run.py`.
A driver newer than 32.0.101.9150 runs the tests again by itself.
If they fail there, raise the version in `has_intel_cached_pso_bug`; if every row draws on the restore, remove the gate and this directory.
