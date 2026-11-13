---
created: 2026-11-13
refs:
  - file: libs/graphics/shaped-shader-library/tests/sgl-shader-model-floor-test.cc
---

# SGL writes a dx12 intersection fused with its any hit, which DXC refuses: an intersection takes no parameters

An intersection fused with its any hit is metal's form, which decides the hit in one function.
SGL's DXIL route refuses it as a native gap: "an intersection fused with its any hit, which only metal writes".
The text route still writes it for dx12, as an `intersection` shader that takes the payload:

```hlsl
[shader("intersection")]
void sgl_layered_slabs_through(inout layers payload)
```

DXC refuses it at every profile: `incorrect number of entry parameters for raytracing stage 'intersection': 1 parameter(s) provided, expected no parameters`.
Seen in slib's floor sweep (`sgl-shader-model-floor-test.cc`, `k_not_compiled`), which lists each such entry against this issue:
`raytracing_end_search.sgl` `sgl_layered_slabs_through`, `raytracing_pipeline.sgl` `sgl_spheres_surface` and `sgl_spheres_occlusion`,
`sgl_mixed/ray_stages.sgl` `sgl_balls_primary` and `sgl_balls_shadow`, and metal's `tracing.sgl` `sgl_boxed_marker`.
sg builds these pipelines on dx12 from separate any-hit and intersection shaders, so nothing runs the fused text there.
The question is whether a dx12 compile should refuse the fused entry, as the DXIL route does, or write nothing for it.
