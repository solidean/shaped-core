---
created: 2026-10-17
parent: epic-sv-openpbr-fidelity
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/tracer_shade.sgl
  - symbol: tracer.shadow_payload
---

# Next-event estimation does not pass through glass

A shadow ray is a visibility test with no closure on it, so a point inside a transmissive solid — or behind one — is lit by BSDF sampling alone.
That is what makes a caustic converge slowly and a glass interior noisy.
The fix is a shadow ray that accumulates transmittance instead of stopping at the first hit.
That needs the any-hit to serve two purposes, or a second traversal.
