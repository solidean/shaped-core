---
created: 2026-10-17
parent: epic-sv-openpbr-fidelity
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/openpbr_microfacet.sgl
  - file: libs/graphics/shaped-viewer/tests/openpbr-bsdf-test.cc
  - symbol: sheen_albedo
---

# Tabulated directional albedos in place of the analytic energy fits

GGX energy compensation is Turquin's analytic fit rather than a tabulated directional albedo (named at the top of `openpbr_microfacet.sgl`).

**Two of the energy bounds are a fit's error rather than a lobe's.**
A white metal furnace overshoots by about 3% (Turquin's analytic compensation).
The fuzz overshoots by about 6% at grazing: `sheen_albedo` charges the layers below it less than the Conty-Estevez lobe actually reflects.
`openpbr-bsdf-test`'s `1.06` energy bound is exactly those two, so it is what tightens when tabulated albedos land.

The same tables serve two more entries:

- **A GGX sample that reflects below the horizon is dropped rather than redistributed**, so `bsdf_pdf` legitimately claims less than the full hemisphere — around a tenth of it for a rough lobe.
  That is unbiased and standard; multiple-scattering GGX sampling is what would put that mass back.
- The film's Fresnel reaching the layer coupling (`bug-sv-thin-film-layer-coupling`).
