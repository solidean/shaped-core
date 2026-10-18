---
created: 2026-10-17
parent: epic-sv-denoising-integration
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/tracer_bindings.sgl
  - symbol: tracer.surface_payload
---

# Measure what the denoiser guides' and the lobe split's payload growth costs

`tracer.surface_payload` grew from 27 scalar components, carried on every ray rather than only primary ones.
The guides added the diffuse albedo and the specular pair, the split `direct_specular` and `lobe`, and light sampling and ReSTIR fields of their own since.
Nothing has measured what that costs the trace.
