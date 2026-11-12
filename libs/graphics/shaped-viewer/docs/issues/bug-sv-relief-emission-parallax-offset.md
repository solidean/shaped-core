---
created: 2026-11-12
parent: epic-sv-openpbr-fidelity
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/material/shader_generator.cc
  - symbol: sv_parallax
---

# An emissive relief's emission is read at the parallax offset where it is hit, and at none where it is sampled

A relief's textures are read where its parallax finds the ray meets the relief, `shading_context::uv_offset`.
Next-event estimation reads an emitter's emission at level 0 and with no offset, since it samples the emitter rather than tracing to it.
So an emissive relief lights the scene by one texel and shows another, and multiple importance sampling weighs the two as one.
No relief material is emissive yet, so nothing shows it.
