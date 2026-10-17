---
created: 2026-10-17
parent: epic-sv-denoising-integration
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/tracer_bindings.sgl
  - symbol: tracer.surface_payload
---

# Measure what the denoiser guides' and the lobe split's payload growth costs

`tracer.surface_payload` went from 27 to 38 scalar components.
3 are the diffuse albedo, 4 the specular pair, and 4 more the split (`direct_specular` and `lobe`).
They are carried on every ray rather than only primary ones.
Nothing has measured what that costs the trace.
