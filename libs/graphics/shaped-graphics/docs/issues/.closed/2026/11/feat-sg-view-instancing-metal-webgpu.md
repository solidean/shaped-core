---
created: 2026-11-05
priority: normal
tags: [xr]
parent: epic-xr
closed: 2026-11-09
resolution: fixed
---

# Several views per draw on metal and webgpu

`sg::feature::view_instancing` and SGL's `@view_index` exist on dx12 and vulkan; metal and webgpu report the feature false.

- **metal**: vertex amplification (`supportsVertexAmplificationCount`, `setVertexAmplificationCount`), with `[[amplification_id]]` already what SGL's MSL spells `@view_index`.
  Not wired up because the machine it was written on builds no metal backend; the visionOS path needs it.
- **webgpu**: WebGPU has no multiview, so `epic-xr` decided `@view_index` lowers to a per-draw value there and a draw per view.
  Today WGSL refuses it as `target-lacks-feature`; the lowering and the host's draw per view are what `feat-xr-webxr` needs.

## Resolution

Metal landed 2026-11-09 as vertex amplification: a pipeline of `view_count` views allows that many amplifications, and a scope sets them with view `i` mapped to layer `i` of every target.
No stage has to write the layer, since the mapping adds it, and a device that amplifies fewer views refuses the pipeline by name.
sg's five view-instancing tests run on metal, an M4 amplifying two views.
The webgpu half moved to `feat-sg-view-instancing-webgpu`.
