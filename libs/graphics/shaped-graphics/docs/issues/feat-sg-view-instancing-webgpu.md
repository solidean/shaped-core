---
priority: normal
tags: [xr]
parent: epic-xr
created: 2026-11-09
---

# Several views per draw on webgpu, as a draw per view

`sg::feature::view_instancing` and SGL's `@view_index` exist on dx12, vulkan and metal; webgpu reports the feature false.
WebGPU has no multiview, so `epic-xr` decided `@view_index` lowers to a per-draw value there and a draw per view.
Today WGSL refuses it as `target-lacks-feature`; the lowering and the host's draw per view are what `feat-xr-webxr` needs.
