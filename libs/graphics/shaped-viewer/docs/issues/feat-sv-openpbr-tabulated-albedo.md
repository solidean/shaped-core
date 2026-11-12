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

**`furnace-test` measures the same fits on a rendered sphere, viewing angle by viewing angle**, which reaches angles the probe's few directions do not:
- White fuzz over a white base is 2% dark at the center and 16% bright at the rim; that test reports it rather than failing on it until this closes.
- A white metal is 3% bright at roughness 0.6, as the probe says, but 14% dark at the center at roughness 0.8: the fit errs both ways.
- White plastic (OpenPBR's default specular over a white base) loses 8.5% at every angle, and `pbr` 7%.
  Measured, not yet explained: a hypothesis is that the substrate takes `(1 - E(mu_o))(1 - E(mu_i))` without Kulla-Conty's `1 / (1 - E_avg)`, which a table would also supply.

The megakernel reproduces the probe's 0.6 metal to within a percent, so these are the closure's numbers rather than an integrator's bias.

The same tables serve two more entries:

- **A GGX sample that reflects below the horizon is dropped rather than redistributed**, so `bsdf_pdf` legitimately claims less than the full hemisphere — around a tenth of it for a rough lobe.
  That is unbiased and standard; multiple-scattering GGX sampling is what would put that mass back.
- The film's Fresnel reaching the layer coupling (`bug-sv-thin-film-layer-coupling`).
