---
created: 2026-10-17
parent: epic-sv-openpbr-fidelity
blocked-by: [feat-sv-openpbr-tabulated-albedo]
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/openpbr_closure.sgl
---

# The thin film's Fresnel does not reach the layer coupling

`spec_transmission` still reads the film-free `spec_f0`, so what the diffuse substrate is charged for the crossing ignores the interference above it.
Averaged over the spectrum a film redistributes reflectance rather than adding it, so the error is small.
Closing it wants the same tabulated albedo the energy compensation does.
