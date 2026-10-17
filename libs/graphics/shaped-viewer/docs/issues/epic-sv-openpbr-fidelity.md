---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/openpbr_closure.sgl
  - file: libs/graphics/shaped-viewer/shaders/sgl/openpbr_evaluate.sgl
  - file: libs/graphics/shaped-viewer/shaders/sgl/openpbr_microfacet.sgl
---

# OpenPBR surface: fidelity and integration

`openpbr.surface` is OpenPBR's parameter set and module `openpbr` (shaders/sgl/openpbr_*.sgl) is the layered BSDF over it.
Fuzz over coat over a base that mixes the metal against a dielectric specular layer over the diffuse substrate.
The path tracer shades through it: the closest hit evaluates the closure, estimates both light sources through it, and importance-samples the continuation.

The parameter set is complete — every group OpenPBR specifies is carried and shaded.
What is left is fidelity and integration, not coverage, and this epic's children are it.
