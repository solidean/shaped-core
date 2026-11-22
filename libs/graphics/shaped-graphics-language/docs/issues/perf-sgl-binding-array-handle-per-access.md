---
created: 2026-11-22
priority: normal
tags: [porting-feedback, dx12]
---

# A dx12 shader creates a descriptor handle per access to a binding array's element

A port measured about 1.7 times the `dx.op` count of the hand-written HLSL in a TAA pass, which it attributes to one handle per binding-array access.
That is a reported measurement, not one made here, and nothing measured whether drivers merge the handles.

## Measured: one handle per distinct element on every route

Not reproduced: SGL's native DXIL and SGL's text through DXC each create one handle per distinct element, as DXC does for the hand-written HLSL.
Measured on 2026-11-22 with the pinned DXC 1.9.2602.24 at `-O3 -enable-16bit-types`, cs_6_6 and cs_6_8 alike, on four shaders.
The one that stands for the rest is a TAA neighborhood clamp, its elements written inline rather than through a `let`:

```sgl
@compute(64) fun taa2(@thread_id id: int3){textures, work}:
    let i = work.picks[id.x] % 16
    let j = work.picks[id.x + 64] % 16
    let p = id.xy
    let mut lo = textures.maps[nonuniform i].load(p)
    let mut hi = lo
    for dy in -1..<2:
        for dx in -1..<2:
            let c = textures.maps[nonuniform i].load(p + int2(dx, dy))
            lo = min(lo, c)
            hi = max(hi, c)
    let mut h = textures.maps[nonuniform j].sample((p as float2) / 64.0, textures.s, level = 0.0)
    if h.w > 0.5:
        h = clamp(h, lo, hi) + textures.maps[nonuniform i].load(p + int2(2, 0))
    else:
        h = textures.maps[nonuniform j].load(p) + textures.maps[nonuniform i].load(p)
    work.results[id.x] = h
```

`maps` is a `texture_2d[float4][16]`, `picks` a `buffer[int]` and `results` a `mut buffer[float4]`.
The hand-written HLSL takes `Texture2D<float4> cur = textures_maps[NonUniformResourceIndex(...)]` once per element and loads through it.

| Shader | Route | `createHandleFromBinding` | `annotateHandle` | `dx.op` calls |
|---|---|---|---|---|
| `taa2`, inline elements in a loop and both arms | SGL native DXIL | 5 | 5 | 36 |
| | SGL text through DXC | 5 | 7 | 39 |
| | hand-written HLSL through DXC | 5 | 7 | 39 |
| `taa`, the same through two `let`s | SGL native DXIL | 5 | 5 | 36 |
| | SGL text through DXC | 5 | 7 | 39 |
| | hand-written HLSL through DXC | 5 | 7 | 39 |
| one `let` element, two loads and two samples | SGL native DXIL | 4 | 4 | 15 |
| | SGL text through DXC | 4 | 4 | 14 |
| | hand-written HLSL through DXC | 4 | 4 | 14 |
| ps_6_6, the element handed twice to a three-tap helper | SGL native DXIL | 2 | 2 | 18 |
| | SGL text through DXC | 2 | 10 | 26 |
| | hand-written HLSL through DXC | 2 | 12 | 28 |

Five handles are the two elements plus `picks`, `results` and the sampler, so each element is made once wherever its accesses sit.
SGL's text does write `textures_maps[NonUniformResourceIndex(at)]` at every access, the index a local evaluated once, and DXC merges those into one handle.
The native writer makes an element's handle once and reuses it across blocks.
What does repeat is DXC's own `annotateHandle` before each use, on the hand-written HLSL as much as on SGL's text.
It is a type annotation on a handle already made, and it is the one place the `dx.op` counts of the routes part.
`uv run dev.py run shan -- diff` of SGL's text route against the hand-written one differs only by one live word.
The one extra `dx.op` in the third shader's native route is a second `sampleLevel` at a coordinate equal to the first, which DXC folds; it has nothing to do with handles.

## What would reopen it

The 1.7 times is not from handles in any of these shapes, so it lives in something the TAA pass does that they do not.
The next step is the pass itself: its `.sgl` and the HLSL it is held against, compared with `shan diff`, or `native-parity.py lines` to find the lines where the counts part.
