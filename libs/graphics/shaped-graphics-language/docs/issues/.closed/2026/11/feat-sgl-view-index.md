---
created: 2026-11-05
priority: next
tags: [xr]
parent: epic-xr
closed: 2026-11-05
resolution: fixed
---

# `@view_index`: which view a shader invocation renders

SGL's stereo surface, decided in `epic-xr`: an input, with the view count a host decision on the pipeline, so one shader serves mono and stereo.

## Wanted

- `@view_index` as a vertex and pixel input beside `@layer`.
- Lowers to `SV_ViewID` on dx12, `ViewIndex` on vulkan, the amplification id on metal.
- On webgpu, a per-draw value, so the same source runs as a draw per view.
- Corpus tests for each target's emitted text.

## Known

- The host side is `feat-sg-view-instancing`.

## Done (2026-11-05)

- `@view_index` is CHK-437: a vertex and pixel stage input, needing `view_instancing` in every stage that reads it.
- It is `SV_ViewID` in HLSL, which DXC writes as SPIR-V's `ViewIndex` for vulkan, and `[[amplification_id]]` in MSL.
- An SGL pipeline sets `view_count`, 1 to 4, through the prelude's mirror of sg's description.
- WGSL refuses it as `target-lacks-feature`; the per-draw lowering is `feat-sg-view-instancing-metal-webgpu`.
- Tested in C++ rather than a corpus file, beside the other stage inputs' emit and feature checks.
