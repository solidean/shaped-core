---
created: 2026-11-03
parent: epic-sv-openpbr-fidelity
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/hybrid.sgl
  - test: sv agreement - every material type draws the same sphere in every mode
---

# The hybrid draws a subsurface material near black, its highlight alone

`bounce_from` ends at a refraction into a subsurface medium, and `direct` lights only the closure's surface lobes.
So a primary hit on a subsurface material gets its specular reflection and nothing of the light the walk would carry back out.
The hybrid is what `render_mode::automatic` picks, so this is what a subsurface material looks like by default.

Measured by the material test's `openpbr-subsurface` case, reported rather than enforced: 14 of 64 tiles past five standard errors in both lightings, the worst 86% off.

A cheap approximation would light the hit as a diffuse surface of `subsurface_color` weighted by `subsurface_weight`, which the thin G-buffer would have to carry for the deferred lighting.
The tracers' own image of it is still noisy, `feat-sv-medium-next-event-estimation`, so the test holds the hybrid to a speckled reference until that lands.
